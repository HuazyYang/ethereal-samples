// Mixed mesh/volume renderer using only NVRHI and offline Slang artifacts.
// SPDX-License-Identifier: Apache-2.0
#include "SlangSceneRT.h"
#include "SlangSceneRTData.h"
#include "ComputeRuntime.h"
#include <nvrhi/shader-program.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <stdexcept>
using namespace nvrhi;
namespace gvdb::sample {
namespace {
using slang::Float4;
using slang::UInt4;
void check(FRESULT result, const char* action) {
    if (NVRHI_FAILED(result))
        throw std::runtime_error(std::string(action) + " failed (" + std::to_string(result) + ")");
}
void require(bool valid, const char* reason) {
    if (!valid)
        throw std::invalid_argument(reason);
}
bool finite(Float4 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w);
}
void validateTransform(const rt::AffineTransform& transform) {
    for (float v : transform)
        require(std::isfinite(v), "Scene transform must be finite");
    const double a = transform[0], b = transform[1], c = transform[2];
    const double d = transform[4], e = transform[5], f = transform[6];
    const double g = transform[8], h = transform[9], i = transform[10];
    const double determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    require(std::isfinite(determinant) && std::abs(determinant) > 1e-12, "Scene transform must be invertible");
}
scene_gpu::Material packMaterial(const SceneMaterial& m) {
    scene_gpu::Material result{
        {m.ambColor.x,  m.ambColor.y,  m.ambColor.z,  m.lightWidth },
        {m.diffColor.x, m.diffColor.y, m.diffColor.z, m.specPower  },
        {m.specColor.x, m.specColor.y, m.specColor.z, m.shadowWidth},
        {m.envColor.x,  m.envColor.y,  m.envColor.z,  m.shadowBias },
        {m.reflColor.x, m.reflColor.y, m.reflColor.z, m.reflWidth  },
        {m.refrColor.x, m.refrColor.y, m.refrColor.z, m.refrWidth  },
        {m.reflBias,    m.refrIor,     m.refrAmount,  m.refrOffset },
        {m.refrBias,    0,             0,             0            }
    };
    const Float4 values[] = {result.ambient,    result.diffuse,    result.specular, result.environment,
                             result.reflection, result.refraction, result.optical,  result.bias};
    for (auto v : values)
        require(finite(v), "Scene material must be finite");
    require(m.refrIor > 0 && m.specPower >= 0 && m.lightWidth >= 0 && m.shadowWidth >= 0 && m.reflWidth >= 0 &&
                m.refrWidth >= 0,
            "Scene material index of refraction, power or scattering width is invalid");
    if (m.refrWidth > 0)
        require(m.refrOffset > 0, "Enabled refraction requires a positive attenuation distance");
    if (m.envColor.x == 1)
        require(m.envColor.y > 0, "Checkerboard cell size must be positive");
    return result;
}
void validateMesh(const SceneMesh& mesh) {
    require(!mesh.positions.empty() && !mesh.indices.empty() && mesh.indices.size() % 3 == 0,
            "Scene mesh needs indexed triangles");
    require(mesh.positions.size() <= UINT32_MAX / sizeof(Float4) &&
                mesh.indices.size() <= UINT32_MAX / sizeof(uint32_t),
            "Scene mesh exceeds portable addressing");
    require(mesh.normals.empty() || mesh.normals.size() == mesh.positions.size(),
            "Scene normal count must match positions");
    for (auto v : mesh.positions)
        require(finite(v), "Scene mesh positions must be finite");
    for (auto v : mesh.normals)
        require(finite(v), "Scene mesh normals must be finite");
    for (auto i : mesh.indices)
        require(i < mesh.positions.size(), "Scene mesh index is out of range");
}
void validateVolume(const SceneVolume& v) {
    require(v.scalar && v.nodes && v.children && v.levels, "Scene volume requires scalar and topology buffers");
    require(uint32_t(v.mode) <= uint32_t(SceneVolumeMode::EmptySkip), "Unknown scene volume mode");
    const auto& i = v.info;
    require(finite(i.boundsMin) && finite(i.boundsMax) && i.boundsMin.x < i.boundsMax.x &&
                i.boundsMin.y < i.boundsMax.y && i.boundsMin.z < i.boundsMax.z,
            "Scene volume bounds must be finite and nonempty");
    const auto r = i.atlasResolutionAndApron, b = i.atlasBrickCountAndWidth;
    require(r.x && r.y && r.z && b.x && b.y && b.z && b.w > 2ull * r.w && uint64_t(b.x) * b.w == r.x &&
                uint64_t(b.y) * b.w == r.y && uint64_t(b.z) * b.w == r.z,
            "Scene volume atlas shape is invalid");
    require(uint64_t(r.x) * r.y <= UINT32_MAX && uint64_t(r.x) * r.y * r.z <= UINT32_MAX / 4,
            "Scene volume exceeds portable addressing");
    const uint64_t bytes = uint64_t(r.x) * r.y * r.z * 4;
    // DX raw SRV bases require 16-byte alignment, not just scalar alignment.
    // Keep one portable scene contract and reject before native view creation.
    require((!v.rawScalar || v.scalarRange.byteOffset % 16 == 0) && v.nodesRange.byteOffset % 16 == 0 &&
                v.childrenRange.byteOffset % 16 == 0 && (!v.color || v.colorRange.byteOffset % 16 == 0) &&
                v.levelsRange.byteOffset % sizeof(slang::LevelInfo) == 0,
            "Scene view base alignment");
    auto rangeSize = [](IBuffer* buffer, BufferRange range) {
        const uint64_t size = buffer->getDesc().byteSize;
        require(range.byteOffset <= size && range.byteOffset % 4 == 0, "Scene buffer range offset");
        const uint64_t count =
            (range.byteSize == 0 || range.byteSize == UINT64_MAX) ? size - range.byteOffset : range.byteSize;
        require(count <= size - range.byteOffset, "Scene buffer range size");
        return count;
    };
    require((v.rawScalar ? v.scalar->getDesc().canHaveRawViews : v.scalar->getDesc().structStride == 4) &&
                rangeSize(v.scalar, v.scalarRange) >= bytes,
            "Scene scalar buffer has an invalid shape");
    require(i.hierarchy.z && i.hierarchy.z <= slang::kMaxLevels && i.hierarchy.x < i.hierarchy.z &&
                v.levels->getDesc().structStride == sizeof(slang::LevelInfo) &&
                rangeSize(v.levels, v.levelsRange) >= uint64_t(i.hierarchy.z) * sizeof(slang::LevelInfo),
            "Scene hierarchy levels are invalid");
    for (auto* p : {v.nodes.Get(), v.children.Get()})
        require(p->getDesc().canHaveRawViews && p->getDesc().byteSize >= 4 &&
                    p->getDesc().byteSize <= uint64_t(UINT32_MAX) + 1,
                "Scene topology requires portable raw buffers");
    require(rangeSize(v.nodes, v.nodesRange) >= 64 && rangeSize(v.children, v.childrenRange) >= 8,
            "Scene topology buffers must hold complete node and child records");
    if (v.color)
        require(v.color->getDesc().canHaveRawViews && rangeSize(v.color, v.colorRange) >= bytes,
                "Scene color requires a packed RGBA8 atlas");
    require(finite(v.steps) && v.steps.x > 0 && v.steps.y > 0 && v.steps.z > 0 && v.steps.w >= 0 &&
                finite(v.extinct) && v.extinct.x <= 0 && v.extinct.y >= 0 && finite(v.cutoff) &&
                v.cutoff.y > 0 && v.cutoff.y <= 1 && finite(v.threshold) && v.threshold.y != v.threshold.z,
            "Scene volume rendering parameters are invalid");
    require(!v.transfer.empty() && v.transfer.size() <= UINT32_MAX / sizeof(Float4),
            "Scene volume needs a transfer function");
    require(v.transferLastIndex == UINT32_MAX || v.transferLastIndex < v.transfer.size(),
            "Scene transfer endpoint is out of range");
    for (auto t : v.transfer)
        require(finite(t) && t.w >= 0, "Scene transfer values must be finite with nonnegative extinction");
}
uint32_t modeGroup(SceneVolumeMode mode) { return uint32_t(mode) + 1; }
uint32_t modeMask(SceneVolumeMode mode) {
    if (mode == SceneVolumeMode::Deep)
        return 8;
    if (mode == SceneVolumeMode::LevelSet)
        return 4;
    return 2;
}
}  // namespace
SceneVolume SceneVolume::from(const slang::SlangVolume& volume) {
    SceneVolume result;
    result.info = volume.info();
    result.scalar = volume.atlas();
    result.nodes = volume.nodes();
    result.children = volume.children();
    result.levels = volume.levels();
    result.transfer = {
        {0, 0, 0, 0},
        {1, 1, 1, 1}
    };
    return result;
}
SceneVolume SceneVolume::from(portable::SnapshotHandle snapshot, uint32_t scalarChannel,
                              uint32_t colorChannel) {
    require(bool(snapshot), "Scene requires a snapshot");
    SceneVolume result;
    result.ownedSnapshot = snapshot;
    result.rawScalar = true;
    result.info = snapshot->getDesc().volume;
    portable::ChannelSnapshotDesc scalar;
    check(snapshot->getChannel(scalarChannel, scalar), "Scene snapshot scalar");
    require(scalar.desc.format == portable::AtlasFormat::R32_FLOAT, "Scene snapshot scalar must be R32_FLOAT");
    result.scalar = scalar.raw.buffer;
    result.scalarRange = {scalar.raw.byteOffset, scalar.raw.byteSize};
    auto topology = [&](portable::TopologyBuffer kind, BufferHandle& buffer, BufferRange& range) {
        portable::BorrowedBufferBinding binding;
        check(snapshot->getTopologyBuffer(kind, binding), "Scene snapshot topology");
        buffer = binding.buffer;
        range = {binding.byteOffset, binding.byteSize};
    };
    topology(portable::TopologyBuffer::Nodes, result.nodes, result.nodesRange);
    topology(portable::TopologyBuffer::Children, result.children, result.childrenRange);
    topology(portable::TopologyBuffer::Levels, result.levels, result.levelsRange);
    if (colorChannel != UINT32_MAX) {
        portable::ChannelSnapshotDesc color;
        check(snapshot->getChannel(colorChannel, color), "Scene snapshot color");
        require(color.desc.format == portable::AtlasFormat::RGBA8_UINT,
                "Scene snapshot color must be RGBA8_UINT");
        result.color = color.raw.buffer;
        result.colorRange = {color.raw.byteOffset, color.raw.byteSize};
    }
    result.transfer = {
        {0, 0, 0, 0},
        {1, 1, 1, 1}
    };
    return result;
}
struct SlangSceneRT::Impl {
    ethereal::slang::ComputeRuntime& runtime;
    DeviceHandle device;
    ShaderProgramHandle program;
    BindingLayoutDesc layoutDesc;
    BindingLayoutHandle layout;
    rt::PipelineHandle pipeline;
    rt::ShaderTableHandle table;
    CommandListHandle commands;
    SceneDescription scene;
    BufferHandle positions, normals, indices, meshMetadata, instanceMetadata, volumeMetadata, materials;
    std::vector<BufferHandle> bounds, transfers;
    std::vector<rt::AccelStructHandle> meshBlas, volumeBlas;
    std::vector<rt::GeometryDesc> geometries;
    rt::AccelStructHandle tlas;
    std::array<BufferHandle, 4> defaults;
    TextureHandle environment;
    SamplerHandle sampler;
    bool environmentEnabled = false, geometryDirty = true, topologyDirty = true, dataDirty = true,
         hasCamera = false, topBuilt = false;
    slang::RaycastParameters lastCamera{};
    BufferHandle accumulation;
    uint32_t samples = 0, randomSeed = 4732, dataFrame = 0;
    Float4 pointLight{100, 100, 100, 1};
    Impl(ethereal::slang::ComputeRuntime& r, const std::filesystem::path& artifact)
        : runtime(r), device(r.device()) {
        require(device->queryFeatureSupport(Feature::RayTracingPipeline),
                "Mixed scene ray tracing is unavailable on this device");
        std::ifstream file(artifact, std::ios::binary);
        if (!file)
            throw std::runtime_error("Missing mixed scene artifact: " + artifact.string());
        const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        check(createShaderProgram(device, bytes.data(), bytes.size(), &program), "create mixed scene program");
        std::map<std::string, ShaderHandle> shaders;
        for (uint32_t n = 0; n < program->getEntryPointCount(); ++n) {
            ShaderEntryPointDesc entry;
            check(program->getEntryPointDesc(n, entry), "reflect scene entry");
            check(program->createShader(n, &shaders[entry.name.c_str()]), "create scene shader");
            BindingLayoutDesc part;
            check(program->getBindingLayoutDesc(n, 0, part), "reflect scene bindings");
            if (n == 0)
                layoutDesc = part;
            else
                for (const auto& binding : part.bindings) {
                    auto found =
                        std::find_if(layoutDesc.bindings.begin(), layoutDesc.bindings.end(),
                                     [&](auto& b) { return b.type == binding.type && b.slot == binding.slot; });
                    if (found == layoutDesc.bindings.end())
                        layoutDesc.addItem(binding);
                    else
                        require(found->size == binding.size, "Scene entries disagree on binding array size");
                }
        }
        layoutDesc.visibility = ShaderType::AllRayTracing;
        check(device->createBindingLayout(layoutDesc, &layout), "create scene binding layout");
        auto shader = [&](const std::string& name) -> IShader* {
            const auto found = shaders.find(name);
            if (found == shaders.end() || !found->second)
                throw std::runtime_error("Missing scene entry: " + name);
            return found->second;
        };
        rt::PipelineDesc desc;
        desc.setMaxPayloadSize(32).setMaxAttributeSize(20).setMaxRecursionDepth(8).addBindingLayout(layout);
        for (const char* name : {"sceneRaygen", "sceneMissRadiance", "sceneMissShadow", "sceneMissRefraction"})
            desc.addShader(rt::PipelineShaderDesc().setExportName(name).setShader(shader(name)));
        for (const char* mode : {"mesh", "surface", "deep", "levelSet", "emptySkip"}) {
            const std::string prefix = mode;
            for (const char* purpose : {"Radiance", "Shadow", "Refraction"}) {
                const std::string name = prefix + purpose;
                rt::PipelineHitGroupDesc group;
                group.setExportName(name.c_str()).setIsProceduralPrimitive(prefix != "mesh");
                const bool shadow = std::string(purpose) == "Shadow";
                const bool refraction = std::string(purpose) == "Refraction";
                if (prefix != "mesh") {
                    const std::string intersection = prefix == "surface"     ? "sceneSurfaceIntersection"
                                                     : prefix == "deep"      ? "sceneDeepIntersection"
                                                     : prefix == "emptySkip" ? "sceneEmptyIntersection"
                                                     : refraction ? "sceneLevelSetRefractionIntersection"
                                                                  : "sceneLevelSetIntersection";
                    group.setIntersectionShader(shader(intersection));
                }
                const std::string shading = prefix == "mesh"   ? "sceneMesh"
                                            : prefix == "deep" ? "sceneDeep"
                                                               : "sceneSurface";
                if (shadow)
                    group.setAnyHitShader(shader(shading + "Shadow"));
                else
                    group.setClosestHitShader(shader(shading + (refraction ? "Refraction" : "Radiance")));
                desc.addHitGroup(group);
            }
        }
        check(device->createRayTracingPipeline(desc, &pipeline), "create mixed scene pipeline");
        check(pipeline->createShaderTable(rt::ShaderTableDesc(), &table), "create mixed scene table");
        table->setRayGenerationShader("sceneRaygen");
        uint32_t n = 0;
        for (const char* name : {"sceneMissRadiance", "sceneMissShadow", "sceneMissRefraction"})
            require(table->addMissShader(name) == n++, "Scene miss record ordering failed");
        n = 0;
        for (const char* mode : {"mesh", "surface", "deep", "levelSet", "emptySkip"})
            for (const char* purpose : {"Radiance", "Shadow", "Refraction"})
                require(table->addHitGroup((std::string(mode) + purpose).c_str()) == n++,
                        "Scene hit record ordering failed");
        check(device->createCommandList(CommandListParameters(), &commands), "create scene command list");
        const std::array<uint8_t, 64> zero{};
        defaults[0] = runtime.buffer(4, 4, zero.data());
        defaults[1] = runtime.buffer(64, 0, zero.data(), true);
        defaults[2] = runtime.buffer(64, 64, zero.data());
        defaults[3] = runtime.buffer(16, 16, zero.data());
        check(device->createSampler(SamplerDesc()
                                        .setAllFilters(true)
                                        .setAddressU(SamplerAddressMode::Wrap)
                                        .setAddressV(SamplerAddressMode::Clamp)
                                        .setAddressW(SamplerAddressMode::Clamp),
                                    &sampler),
              "create environment sampler");
        uploadEnvironment({});
    }
    ~Impl() {
        // Vulkan TLAS resources do not retain their BLAS inputs. Complete work before
        // destroying this generation's explicit geometry owners.
        device->waitForIdle();
    }
    void submit() {
        commands->close();
        if (!device->executeCommandList(commands))
            throw std::runtime_error("Mixed scene submission failed");
    }
    BufferHandle dataBuffer(const void* data, size_t count, uint32_t stride, bool buildInput = false) {
        if (!count)
            return runtime.buffer(stride, stride, std::vector<uint8_t>(stride, 0).data());
        if (!buildInput)
            return runtime.buffer(count * stride, stride, data);
        BufferHandle buffer;
        check(device->createBuffer(BufferDesc()
                                       .setByteSize(count * stride)
                                       .setStructStride(stride)
                                       .setIsAccelStructBuildInput(true)
                                       .setInitialState(ResourceStates::CopyDest)
                                       .setKeepInitialState(true),
                                   &buffer),
              "create scene geometry buffer");
        runtime.upload(buffer, data, count * stride);
        return buffer;
    }
    void uploadEnvironment(const SceneEnvironment& env) {
        const bool enabled = env.width || env.height || !env.rgba.empty();
        if (enabled)
            require(env.width && env.height && env.width <= 16384 && env.height <= 16384 &&
                        uint64_t(env.width) * env.height * 4 == env.rgba.size(),
                    "Environment dimensions and RGBA data disagree");
        TextureHandle replacement;
        check(device->createTexture(TextureDesc()
                                        .setWidth(enabled ? env.width : 1)
                                        .setHeight(enabled ? env.height : 1)
                                        .setFormat(Format::RGBA8_UNORM)
                                        .setInitialState(ResourceStates::ShaderResource)
                                        .setKeepInitialState(true),
                                    &replacement),
              "create environment texture");
        const uint32_t white = 0xffffffffu;
        runtime.flush();
        commands->open();
        commands->writeTexture(replacement, 0, 0, enabled ? static_cast<const void*>(env.rgba.data()) : &white,
                               (enabled ? env.width : 1) * 4);
        submit();
        environment = replacement;
        environmentEnabled = enabled;
        samples = 0;
    }
    void buildGeometry() {
        runtime.flush();
        // Geometry replacement is an explicit synchronization point in this initial
        // renderer. Keep all old BLAS/buffers alive until submitted traces finish.
        require(device->waitForIdle(), "Scene geometry synchronization failed");
        std::vector<Float4> hostPositions, hostNormals;
        std::vector<uint32_t> hostIndices;
        std::vector<UInt4> metadata;
        for (const auto& m : scene.meshes) {
            require(hostPositions.size() + m.positions.size() <= UINT32_MAX / sizeof(Float4) &&
                        hostIndices.size() + m.indices.size() <= UINT32_MAX / sizeof(uint32_t),
                    "Combined scene meshes exceed portable addressing");
            metadata.push_back(
                {uint32_t(hostPositions.size()), uint32_t(hostIndices.size()), uint32_t(m.indices.size()), 0});
            hostPositions.insert(hostPositions.end(), m.positions.begin(), m.positions.end());
            if (m.normals.empty())
                hostNormals.resize(hostPositions.size());
            else
                hostNormals.insert(hostNormals.end(), m.normals.begin(), m.normals.end());
            hostIndices.insert(hostIndices.end(), m.indices.begin(), m.indices.end());
        }
        positions = dataBuffer(hostPositions.data(), hostPositions.size(), sizeof(Float4), true);
        normals = dataBuffer(hostNormals.data(), hostNormals.size(), sizeof(Float4));
        indices = dataBuffer(hostIndices.data(), hostIndices.size(), sizeof(uint32_t), true);
        meshMetadata = dataBuffer(metadata.data(), metadata.size(), sizeof(UInt4));
        meshBlas.clear();
        volumeBlas.clear();
        bounds.clear();
        geometries.clear();
        for (size_t n = 0; n < scene.meshes.size(); ++n) {
            const auto& m = scene.meshes[n];
            const auto& offsets = metadata[n];
            rt::GeometryDesc geometry;
            geometry.setTriangles(rt::GeometryTriangles()
                                      .setVertexBuffer(positions)
                                      .setIndexBuffer(indices)
                                      .setVertexFormat(Format::RGB32_FLOAT)
                                      .setIndexFormat(Format::R32_UINT)
                                      .setVertexStride(sizeof(Float4))
                                      .setVertexOffset(uint64_t(offsets.x) * sizeof(Float4))
                                      .setIndexOffset(uint64_t(offsets.y) * sizeof(uint32_t))
                                      .setVertexCount(uint32_t(m.positions.size()))
                                      .setIndexCount(uint32_t(m.indices.size())));
            rt::AccelStructHandle blas;
            check(
                device->createAccelStruct(rt::AccelStructDesc().addBottomLevelGeometry(geometry).setBuildFlags(
                                              rt::AccelStructBuildFlags::PreferFastTrace),
                                          &blas),
                "create mesh BLAS");
            meshBlas.push_back(blas);
            geometries.push_back(geometry);
        }
        for (const auto& v : scene.volumes) {
            const auto lo = v.info.boundsMin, hi = v.info.boundsMax;
            const rt::GeometryAABB box{lo.x, lo.y, lo.z, hi.x, hi.y, hi.z};
            auto buffer = dataBuffer(&box, 1, sizeof(box), true);
            bounds.push_back(buffer);
            rt::GeometryDesc geometry;
            geometry.setAABBs(rt::GeometryAABBs().setBuffer(buffer).setCount(1).setStride(sizeof(box)));
            rt::AccelStructHandle blas;
            check(
                device->createAccelStruct(rt::AccelStructDesc().addBottomLevelGeometry(geometry).setBuildFlags(
                                              rt::AccelStructBuildFlags::PreferFastTrace),
                                          &blas),
                "create volume BLAS");
            volumeBlas.push_back(blas);
            geometries.push_back(geometry);
        }
        runtime.flush();
        commands->open();
        for (size_t n = 0; n < meshBlas.size(); ++n)
            commands->buildBottomLevelAccelStruct(meshBlas[n], &geometries[n], 1);
        for (size_t n = 0; n < volumeBlas.size(); ++n)
            commands->buildBottomLevelAccelStruct(volumeBlas[n], &geometries[meshBlas.size() + n], 1);
        submit();
        geometryDirty = false;
        topologyDirty = true;
        // Recreate the TLAS after its old generation has completed.
        check(device->createAccelStruct(rt::AccelStructDesc()
                                            .setTopLevelMaxInstances(scene.instances.size())
                                            .setBuildFlags(rt::AccelStructBuildFlags::AllowUpdate),
                                        &tlas),
              "create scene TLAS");
        topBuilt = false;
    }
    void uploadData() {
        std::vector<UInt4> instances;
        for (const auto& i : scene.instances)
            instances.push_back({uint32_t(i.kind), i.geometry, i.material, 0});
        instanceMetadata = dataBuffer(instances.data(), instances.size(), sizeof(UInt4));
        std::vector<scene_gpu::Material> materialValues;
        for (const auto& m : scene.materials)
            materialValues.push_back(packMaterial(m));
        materials = dataBuffer(materialValues.data(), materialValues.size(), sizeof(scene_gpu::Material));
        std::vector<scene_gpu::Volume> volumes;
        transfers.clear();
        for (size_t n = 0; n < scene.volumes.size(); ++n) {
            const auto& v = scene.volumes[n];
            volumes.push_back({
                v.info,
                v.steps,
                v.extinct,
                v.cutoff,
                v.threshold,
                {uint32_t(n), (v.color ? 1u : 0u) | (v.rawScalar ? 2u : 0u),
                            v.transferLastIndex == UINT32_MAX ? uint32_t(v.transfer.size() - 1) : v.transferLastIndex,
                            uint32_t(v.mode)}
            });
            transfers.push_back(dataBuffer(v.transfer.data(), v.transfer.size(), sizeof(Float4)));
        }
        volumeMetadata = dataBuffer(volumes.data(), volumes.size(), sizeof(scene_gpu::Volume));
        dataDirty = false;
    }
    slang::RaycastOutput render(slang::RaycastParameters p, uint32_t frame) {
        require(!scene.instances.empty(), "Set a nonempty scene before rendering");
        // Camera marching fields belong to per-volume descriptors; retain only image iteration cap and clip
        // interval.
        p.volume = {};
        p.image.z = 0;
        p.march.x = 1;
        p.march.w = 0;
        p.valueRange = {0, 1, 0, 1};
        p.light = {};
        p.rayOrigin.w = p.directionDx.w = p.directionDy.w = 0;
        slang::validateRaycastParameters(p, {
                                                {0, 0, 0, 0}
        });
        require(p.march.y >= 0 && p.march.y < p.march.z, "Scene camera needs nonnegative nonempty clip planes");
        require(p.image.x <= 16384 && p.image.y <= 16384, "Scene output exceeds supported image dimensions");
        const Float4 center{
            p.direction00.x + .5f * p.image.x * p.directionDx.x + .5f * p.image.y * p.directionDy.x,
            p.direction00.y + .5f * p.image.x * p.directionDx.y + .5f * p.image.y * p.directionDy.y,
            p.direction00.z + .5f * p.image.x * p.directionDx.z + .5f * p.image.y * p.directionDy.z, 0};
        require(finite(center) && (center.x != 0 || center.y != 0 || center.z != 0),
                "Scene camera direction is invalid");
        if (!hasCamera || std::memcmp(&p, &lastCamera, sizeof(p)) != 0 || frame == 0)
            samples = 0;
        require(frame == UINT32_MAX || frame == samples,
                "Explicit scene sample must match the next sample index (zero resets)");
        if (samples == UINT32_MAX)
            throw std::overflow_error("Scene accumulation sample count exhausted");
        const size_t pixels = size_t(p.image.x) * p.image.y;
        if (!hasCamera || p.image.x != lastCamera.image.x || p.image.y != lastCamera.image.y)
            accumulation = runtime.buffer(pixels * sizeof(Float4), sizeof(Float4));
        if (geometryDirty)
            buildGeometry();
        if (dataDirty)
            uploadData();
        slang::RaycastOutput result;
        result.width = p.image.x;
        result.height = p.image.y;
        result.color = runtime.buffer(pixels * sizeof(Float4), sizeof(Float4));
        result.depth = runtime.buffer(pixels * sizeof(float), sizeof(float));
        const scene_gpu::Parameters parameters{
            p,
            {samples,   uint32_t(scene.instances.size()), uint32_t(scene.volumes.size()),
              environmentEnabled ? 1u : 0u                                                  },
            {dataFrame, randomSeed,                       0,                              0},
            pointLight
        };
        std::array<uint8_t, 512> padded{};
        std::memcpy(padded.data(), &parameters, sizeof(parameters));
        BufferHandle constants;
        check(device->createBuffer(BufferDesc()
                                       .setByteSize(padded.size())
                                       .setIsConstantBuffer(true)
                                       .setInitialState(ResourceStates::ConstantBuffer)
                                       .setKeepInitialState(true),
                                   &constants),
              "create scene parameters");
        runtime.upload(constants, padded.data(), padded.size());
        std::vector<BindingSetItem> available = {BindingSetItem::ConstantBuffer(0, constants),
                                                 BindingSetItem::RayTracingAccelStruct(0, tlas),
                                                 BindingSetItem::StructuredBuffer_SRV(1, instanceMetadata),
                                                 BindingSetItem::StructuredBuffer_SRV(2, meshMetadata),
                                                 BindingSetItem::StructuredBuffer_SRV(3, positions),
                                                 BindingSetItem::StructuredBuffer_SRV(4, normals),
                                                 BindingSetItem::StructuredBuffer_SRV(5, indices),
                                                 BindingSetItem::StructuredBuffer_SRV(6, volumeMetadata),
                                                 BindingSetItem::StructuredBuffer_SRV(7, materials),
                                                 BindingSetItem::Texture_SRV(8, environment),
                                                 BindingSetItem::Sampler(0, sampler),
                                                 BindingSetItem::StructuredBuffer_UAV(0, result.color),
                                                 BindingSetItem::StructuredBuffer_UAV(1, result.depth),
                                                 BindingSetItem::StructuredBuffer_UAV(2, accumulation)};
        for (uint32_t n = 0; n < MaxVolumes; ++n) {
            const SceneVolume* v = n < scene.volumes.size() ? &scene.volumes[n] : nullptr;
            available.push_back(BindingSetItem::StructuredBuffer_SRV(
                                    16, v && !v->rawScalar ? v->scalar.Get() : defaults[0].Get(),
                                    Format::UNKNOWN, v && !v->rawScalar ? v->scalarRange : BufferRange{})
                                    .setArrayElement(n));
            available.push_back(BindingSetItem::RawBuffer_SRV(24, v ? v->nodes.Get() : defaults[1].Get(),
                                                              v ? v->nodesRange : BufferRange{})
                                    .setArrayElement(n));
            available.push_back(BindingSetItem::RawBuffer_SRV(32, v ? v->children.Get() : defaults[1].Get(),
                                                              v ? v->childrenRange : BufferRange{})
                                    .setArrayElement(n));
            available.push_back(
                BindingSetItem::StructuredBuffer_SRV(40, v ? v->levels.Get() : defaults[2].Get(),
                                                     Format::UNKNOWN, v ? v->levelsRange : BufferRange{})
                    .setArrayElement(n));
            available.push_back(
                BindingSetItem::RawBuffer_SRV(48, v && v->color ? v->color.Get() : defaults[1].Get(),
                                              v && v->color ? v->colorRange : BufferRange{})
                    .setArrayElement(n));
            available.push_back(
                BindingSetItem::RawBuffer_SRV(64, v && v->rawScalar ? v->scalar.Get() : defaults[1].Get(),
                                              v && v->rawScalar ? v->scalarRange : BufferRange{})
                    .setArrayElement(n));
            available.push_back(
                BindingSetItem::StructuredBuffer_SRV(56, v ? transfers[n].Get() : defaults[3].Get())
                    .setArrayElement(n));
        }
        BindingSetDesc bindingDesc;
        for (const auto& reflected : layoutDesc.bindings)
            for (uint32_t element = 0; element < std::max(1u, uint32_t(reflected.size)); ++element) {
                const auto found = std::find_if(available.begin(), available.end(), [&](const auto& item) {
                    return item.type == reflected.type && item.slot == reflected.slot &&
                           item.arrayElement == element;
                });
                if (found == available.end())
                    throw std::runtime_error("Unexpected reflected mixed-scene binding");
                bindingDesc.addItem(*found);
            }
        BindingSetHandle bindings;
        check(device->createBindingSet(bindingDesc, layout, &bindings), "create scene binding set");
        runtime.flush();
        commands->open();
        if (topologyDirty) {
            std::vector<rt::InstanceDesc> instances;
            for (size_t n = 0; n < scene.instances.size(); ++n) {
                const auto& i = scene.instances[n];
                const bool mesh = i.kind == SceneInstance::Kind::Mesh;
                const auto mode = mesh ? SceneVolumeMode::Surface : scene.volumes[i.geometry].mode;
                rt::InstanceDesc instance;
                instance.setBLAS(mesh ? meshBlas[i.geometry].Get() : volumeBlas[i.geometry].Get())
                    .setInstanceID(uint32_t(n))
                    .setTransform(i.transform)
                    .setInstanceMask(mesh ? 1 : modeMask(mode))
                    .setInstanceContributionToHitGroupIndex(mesh ? 0 : 3 * modeGroup(mode));
                instances.push_back(instance);
            }
            commands->buildTopLevelAccelStruct(
                tlas, instances.data(), instances.size(),
                topBuilt ? rt::AccelStructBuildFlags::PerformUpdate : rt::AccelStructBuildFlags::None);
            topBuilt = true;
            topologyDirty = false;
        }
        commands->setRayTracingState(rt::State().setShaderTable(table).addBindingSet(bindings));
        commands->dispatchRays(rt::DispatchRaysArguments().setDimensions(p.image.x, p.image.y));
        submit();
        lastCamera = p;
        hasCamera = true;
        ++samples;
        return result;
    }
};
SlangSceneRT::SlangSceneRT(ethereal::slang::ComputeRuntime& runtime, const std::filesystem::path& artifact)
    : m_impl(std::make_unique<Impl>(runtime, artifact)) {}
SlangSceneRT::~SlangSceneRT() = default;
void SlangSceneRT::setScene(const SceneDescription& scene) {
    require(!scene.instances.empty() && scene.instances.size() <= 0xffffff,
            "Scene instance count must fit 24 bits and be nonzero");
    require(scene.volumes.size() <= MaxVolumes, "Mixed scenes support at most eight volume resource sets");
    require(scene.meshes.size() <= UINT32_MAX, "Too many scene meshes");
    SceneDescription copy = scene;
    if (copy.materials.empty())
        copy.materials.emplace_back();
    for (const auto& m : copy.meshes)
        validateMesh(m);
    for (const auto& v : copy.volumes) {
        require(!v.ownedSnapshot || v.ownedSnapshot->getDevice() == m_impl->device.Get(),
                "Scene snapshot device mismatch");
        validateVolume(v);
    }
    for (const auto& m : copy.materials)
        packMaterial(m);
    for (const auto& i : copy.instances) {
        require(i.kind == SceneInstance::Kind::Mesh || i.kind == SceneInstance::Kind::Volume,
                "Invalid scene instance kind");
        require(i.geometry < (i.kind == SceneInstance::Kind::Mesh ? copy.meshes.size() : copy.volumes.size()) &&
                    i.material < copy.materials.size(),
                "Scene instance references missing geometry or material");
        validateTransform(i.transform);
    }
    m_impl->scene = std::move(copy);
    m_impl->geometryDirty = m_impl->topologyDirty = m_impl->dataDirty = true;
    resetAccumulation();
}
void SlangSceneRT::updateVolume(uint32_t index, const SceneVolume& volume) {
    require(index < m_impl->scene.volumes.size(), "Volume index is out of range");
    require(!volume.ownedSnapshot || volume.ownedSnapshot->getDevice() == m_impl->device.Get(),
            "Scene snapshot device mismatch");
    validateVolume(volume);
    m_impl->scene.volumes[index] = volume;
    m_impl->geometryDirty = m_impl->dataDirty = true;
    resetAccumulation();
}
void SlangSceneRT::updateMesh(uint32_t index, const SceneMesh& mesh) {
    require(index < m_impl->scene.meshes.size(), "Mesh index is out of range");
    validateMesh(mesh);
    m_impl->scene.meshes[index] = mesh;
    m_impl->geometryDirty = true;
    resetAccumulation();
}
void SlangSceneRT::updateTransform(uint32_t instance, const rt::AffineTransform& transform) {
    require(instance < m_impl->scene.instances.size(), "Instance index is out of range");
    validateTransform(transform);
    std::memcpy(m_impl->scene.instances[instance].transform, transform, sizeof(transform));
    m_impl->topologyDirty = true;
    resetAccumulation();
}
void SlangSceneRT::setMaterial(uint32_t index, const SceneMaterial& material) {
    require(index < m_impl->scene.materials.size(), "Material index is out of range");
    packMaterial(material);
    m_impl->scene.materials[index] = material;
    m_impl->dataDirty = true;
    resetAccumulation();
}
void SlangSceneRT::setEnvironment(const SceneEnvironment& environment) {
    m_impl->uploadEnvironment(environment);
}
void SlangSceneRT::setPointLight(Float4 position) {
    require(finite(position), "Scene point light must be finite");
    position.w = 1;
    if (std::memcmp(&position, &m_impl->pointLight, sizeof(position))) {
        m_impl->pointLight = position;
        resetAccumulation();
    }
}
void SlangSceneRT::setRandomSeed(uint32_t seed, uint32_t dataFrame) {
    if (seed != m_impl->randomSeed || dataFrame != m_impl->dataFrame) {
        m_impl->randomSeed = seed;
        m_impl->dataFrame = dataFrame;
        resetAccumulation();
    }
}
void SlangSceneRT::resetAccumulation() { m_impl->samples = 0; }
slang::RaycastOutput SlangSceneRT::render(slang::RaycastParameters camera, uint32_t frame) {
    return m_impl->render(camera, frame);
}
uint32_t SlangSceneRT::sampleCount() const { return m_impl->samples; }
}  // namespace gvdb::sample
