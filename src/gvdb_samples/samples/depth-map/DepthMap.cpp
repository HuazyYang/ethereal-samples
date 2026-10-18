// SPDX-License-Identifier: Apache-2.0
#include "DepthMap.h"
#include "DepthMapShaderData.h"
#include "../sample-utils/ObjLoader.h"
#include "ComputeRuntime.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <donut/app/ApplicationBase.h>
#include <donut/app/DeviceManager.h>
#include <donut/core/vfs/VFS.h>
#include <donut/engine/ShaderFactory.h>
#include <fstream>
#include "../sample-utils/OwnedVolumeIO.h"
#include <nvrhi/shader-program.h>
#include <stdexcept>
namespace depth_map
{
using namespace nvrhi;
namespace g = gvdb::slang;
namespace
{
void check(FRESULT r, const char *what)
{
    if (NVRHI_FAILED(r))
        throw std::runtime_error(std::string(what) + ": " + std::to_string(r));
}
void require(bool value, const char *what)
{
    if (!value)
        throw std::runtime_error(what);
}
using detail::DepthParameters;
struct RasterParameters
{
    dm::float4x4 viewProjection;
    dm::float4 eye, light;
};
static_assert(sizeof(DepthParameters) == 272 && sizeof(RasterParameters) == 96);
void submit(IDevice *d, ICommandList *list)
{
    list->close();
    const auto id = d->executeCommandList(list);
    require(id != 0 || d->getGraphicsAPI() == GraphicsAPI::D3D11, "Command submission failed");
}
BufferHandle constants(IDevice *device, size_t bytes)
{
    BufferDesc desc;
    desc.byteSize = (bytes + 255) & ~size_t(255);
    desc.isConstantBuffer = true;
    desc.initialState = ResourceStates::ConstantBuffer;
    desc.keepInitialState = true;
    BufferHandle result;
    check(device->createBuffer(desc, &result), "create constants");
    return result;
}
void writeConstants(ICommandList *list, IBuffer *buffer, const void *data, size_t bytes)
{
    std::vector<uint8_t> padded(size_t(buffer->getDesc().byteSize));
    std::memcpy(padded.data(), data, bytes);
    list->writeBuffer(buffer, padded.data(), padded.size());
}
} // namespace
std::vector<g::Float4> referenceTransfer()
{
    std::vector<g::Float4> result(16384);
    const auto range = [&](float low, float high, dm::float4 a, dm::float4 b) {
        int first = int(std::floor(low * result.size())), last = int(std::floor(high * result.size()));
        for (int i = first; i < last; ++i)
        {
            auto v = a + (b - a) * (float(i - first) / float(last - first));
            result[i] = {v.x, v.y, v.z, v.w};
        }
    };
    range(0, .1f, {0, 0, 0, 0}, {0, 0, 0, 0});
    range(.1f, .5f, {1, 1, 0, .1f}, {1, 0, 0, .3f});
    range(.5f, .75f, {1, 0, 0, .3f}, {.2f, .2f, .2f, .1f});
    range(.75f, 1, {.2f, .2f, .2f, .1f}, {.1f, .1f, .1f, .3f});
    return result;
}
Scene loadScene(const std::filesystem::path &volume, const std::filesystem::path &mesh)
{
    Scene scene;
    std::ifstream input(volume, std::ios::binary);
    require(bool(input), "Cannot open depth-map VBX");
    scene.volumeVBX.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    require(!input.bad() && !scene.volumeVBX.empty(), "Cannot read depth-map VBX");
    auto fs = MAKE_RC_OBJ_PTR(donut::vfs::NativeFileSystem);
    auto source = SampleUtils::ObjLoader()(fs, mesh.string().c_str());
    require(!source.positions.empty() && !source.indices.empty(), "Cannot load depth-map OBJ mesh");
    scene.mesh.positions = std::move(source.positions);
    scene.mesh.normals = std::move(source.normals);
    for (auto &position : scene.mesh.positions)
        position *= 100.f;
    for (auto &normal : scene.mesh.normals)
        normal = dm::snorm8ToVector<3>(dm::vectorToSnorm8(normal));
    for (const auto &triangle : source.indices)
        for (unsigned i = 0; i < 3; ++i)
            scene.mesh.indices.push_back(uint32_t(triangle[i]));
    scene.transfer = referenceTransfer();
    return scene;
}
dm::affine3 orbitCamera(dm::float3 angles, dm::float3 target, float distance)
{
    float a = dm::radians(angles.x), b = dm::radians(angles.y);
    dm::float3 back(std::cos(b) * std::sin(a), std::sin(b), std::cos(b) * std::cos(a)), worldUp(0, 1, 0);
    if (std::abs(dm::dot(back, worldUp)) > .999f)
        worldUp = {0, 0, 1};
    auto right = dm::normalize(dm::cross(worldUp, back)), up = dm::normalize(dm::cross(back, right));
    return {dm::float3x3(right, up, -back), target + back * distance};
}
dm::affine3 volumeTransform(dm::float3 angles, dm::float3 translation)
{
    return dm::translation(dm::float3(-125, -160, -125)) * dm::scaling(dm::float3(.5f)) *
           dm::rotationQuat(dm::radians(angles)).toAffine() * dm::translation(translation);
}
View defaultView()
{
    View v;
    v.cameraToWorld = orbitCamera({-40, 20, 0}, {0, 0, 0}, 400);
    v.indexToWorld = volumeTransform({0, 0, 0}, {0, 0, -25});
    v.light = orbitCamera({299, 57.3f, 0}, {0, 0, 0}, 500).m_translation;
    return v;
}
struct Renderer::Impl
{
    DeviceHandle graphics, compute;
    GraphicsInteropDeviceHandle exporter;
    InteropDeviceHandle importer;
    ExternalFenceHandle fence, importedFence;
    uint64_t frame = 0;
    ethereal::slang::ComputeRuntime runtime;
    gvdb::portable::SnapshotHandle volume;
    gvdb::portable::SnapshotDesc volumeDesc;
    gvdb::portable::ChannelSnapshotDesc scalar;
    gvdb::portable::BorrowedBufferBinding nodes, children, levels;
    BufferHandle dummyChildren;
    BufferHandle transfer;
    BufferHandle positions, normals, indices, rasterConstants, depthConstants;
    uint32_t indexCount = 0;
    CommandListHandle rasterCommands, computeCommands, compositeCommands;
    ShaderHandle meshVS, meshPS, compositeVS, compositePS;
    ShaderProgramHandle computeProgram;
    BindingLayoutHandle rasterLayout, compositeLayout, computeLayout;
    BindingSetHandle rasterBindings, compositeBindings, computeBindings;
    InputLayoutHandle inputLayout;
    GraphicsPipelineHandle rasterPipeline, compositePipeline;
    ComputePipelineHandle computePipeline;
    TextureHandle meshColor, rasterDepth, depthStencil, volumeColor, finalColor, importedDepth, importedColor;
    ExternalMemoryHandle depthMemory, colorMemory;
    SharedResourceFlags textureSharingFlags = SharedResourceFlags::None;
    FramebufferHandle rasterFramebuffer, finalFramebuffer;
    uint32_t width = 0, height = 0;
    bool first = true;
    Impl(IDevice *gr, IDevice *co, const Scene &scene)
        : graphics(gr), compute(co), runtime(co, donut::app::GetDirectoryWithExecutable() / "shaders/gvdb_slang" /
                                                     donut::app::GetShaderTypeName(co->getGraphicsAPI()))
    {
        require(scene.transfer.size() == 16384, "Depth-map transfer requires 16384 entries");
        const auto owned = donut::app::GetDirectoryWithExecutable() / "shaders/gvdb_owned" /
                           donut::app::GetShaderTypeName(co->getGraphicsAPI());
        gvdb::sample::OwnedVolumeLoadOptions load;
        if (scene.refreshApron)
        {
            load.refreshApronChannels = 1;
            load.mutationArtifact = owned / "GVDBAtlasMutation_mutateAtlas.bin";
        }
        volume = gvdb::sample::loadOwnedVBX(co, owned / "GVDBAtlasRepack_relocateAtlas.bin", scene.volumeVBX.data(),
                                            scene.volumeVBX.size(), load);
        volumeDesc = volume->getDesc();
        check(volume->getChannel(0, scalar), "depth-map scalar channel");
        require(volumeDesc.channelMask == 1 && scalar.desc.format == gvdb::portable::AtlasFormat::R32_FLOAT,
                "Depth-map requires exactly one R32_FLOAT channel");
        check(volume->getTopologyBuffer(gvdb::portable::TopologyBuffer::Nodes, nodes), "depth-map nodes");
        check(volume->getTopologyBuffer(gvdb::portable::TopologyBuffer::Children, children), "depth-map children");
        check(volume->getTopologyBuffer(gvdb::portable::TopologyBuffer::Levels, levels), "depth-map levels");
        const auto raw = [](const gvdb::portable::BorrowedBufferBinding &binding, uint64_t minimum) {
            return bool(gvdb::portable::validateRawView({binding.buffer, binding.byteOffset, binding.byteSize}, minimum,
                                                        false));
        };
        require(raw(scalar.raw, scalar.storage.allocatedBytes) && raw(nodes, sizeof(gvdb::portable::Node)) &&
                    raw(children, 0),
                "Depth-map canonical raw ranges");
        require(
            levels.buffer && levels.buffer->getDesc().structStride == sizeof(g::LevelInfo) &&
                gvdb::portable::containsBytes(levels.buffer->getDesc().byteSize, levels.byteOffset, levels.byteSize) &&
                levels.byteOffset % sizeof(g::LevelInfo) == 0 &&
                levels.byteSize >= uint64_t(volumeDesc.levelCount) * sizeof(g::LevelInfo),
            "Depth-map level range");
        if (!children.buffer)
        {
            const uint32_t missing[2] = {UINT32_MAX, UINT32_MAX};
            dummyChildren = runtime.buffer(sizeof(missing), 0, missing, true);
            children = {dummyChildren, 0, sizeof(missing)};
        }
        transfer = runtime.buffer(scene.transfer.size() * sizeof(g::Float4), sizeof(g::Float4), scene.transfer.data());
        runtime.flush();
        if (gr != co)
        {
            require(co->getGraphicsAPI() == GraphicsAPI::CUDA, "Cross-API depth-map requires CUDA compute");
            check(nvrhiCreateGraphicsInteropDevice(gr, &exporter), "create graphics exporter");
#if ETHEREAL_TEST_CUDA
            check(cuda::nvrhiCudaCreateInteropDevice(co, &importer), "create CUDA importer");
#else
            throw std::runtime_error("CUDA backend was not built");
#endif
            InteropRouteCapabilities route;
            check(exporter->queryRoute(GraphicsAPI::CUDA, importer->getAdapterIdentity(), route),
                  "same-adapter texture/timeline route");
            require(route.textureSharing && route.timelineFences, "Missing native texture/timeline interoperability");
            textureSharingFlags = route.textureSharingFlags;
            check(exporter->createFence(0, &fence), "create timeline");
            check(importer->importFence(fence->getDesc(), &importedFence), "import timeline");
        }
        check(gr->createCommandList(CommandListParameters(), &rasterCommands), "raster commands");
        check(co->createCommandList(CommandListParameters(), &computeCommands), "compute commands");
        check(gr->createCommandList(CommandListParameters(), &compositeCommands), "composition commands");
        rasterConstants = constants(gr, sizeof(RasterParameters));
        depthConstants = constants(co, sizeof(DepthParameters));
        auto fs = MAKE_RC_OBJ_PTR(donut::vfs::NativeFileSystem);
        auto shaderDir = donut::app::GetDirectoryWithExecutable() / "shaders/depth_map";
        donut::engine::ShaderFactory shaders(gr, fs, shaderDir / donut::app::GetShaderTypeName(gr->getGraphicsAPI()));
        meshVS = shaders.CreateShader("DepthRaster.slang", "meshVS", nullptr, ShaderType::Vertex);
        meshPS = shaders.CreateShader("DepthRaster.slang", "meshPS", nullptr, ShaderType::Pixel);
        compositeVS = shaders.CreateShader("DepthRaster.slang", "compositeVS", nullptr, ShaderType::Vertex);
        compositePS = shaders.CreateShader("DepthRaster.slang", "compositePS", nullptr, ShaderType::Pixel);
        require(meshVS && meshPS && compositeVS && compositePS, "Missing depth-map raster/composition artifacts");
        BindingLayoutDesc layout;
        layout.visibility = ShaderType::Vertex | ShaderType::Pixel;
        layout.bindings = {BindingLayoutItem::ConstantBuffer(0)};
        check(gr->createBindingLayout(layout, &rasterLayout), "raster layout");
        BindingSetDesc set;
        set.bindings = {BindingSetItem::ConstantBuffer(0, rasterConstants)};
        check(gr->createBindingSet(set, rasterLayout, &rasterBindings), "raster bindings");
        layout.bindings = {BindingLayoutItem::Texture_SRV(0), BindingLayoutItem::Texture_SRV(1)};
        check(gr->createBindingLayout(layout, &compositeLayout), "composition layout");
        VertexAttributeDesc attributes[] = {VertexAttributeDesc()
                                                .setName("POSITION")
                                                .setFormat(Format::RGB32_FLOAT)
                                                .setBufferIndex(0)
                                                .setElementStride(sizeof(dm::float3)),
                                            VertexAttributeDesc()
                                                .setName("NORMAL")
                                                .setFormat(Format::RGB32_FLOAT)
                                                .setBufferIndex(1)
                                                .setElementStride(sizeof(dm::float3))};
        check(gr->createInputLayout(attributes, 2, meshVS, &inputLayout), "mesh input layout");
        auto path = shaderDir / donut::app::GetShaderTypeName(co->getGraphicsAPI()) / "DepthClip_clipVolume.bin";
        std::ifstream file(path, std::ios::binary);
        require(bool(file), "Missing depth-map compute artifact");
        std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        check(createShaderProgram(co, bytes.data(), bytes.size(), &computeProgram), "load depth clipping program");
        uint32_t entry;
        check(computeProgram->findEntryPoint("clipVolume", ShaderType::Compute, &entry), "depth clipping entry");
        ShaderEntryPointDesc entryDesc;
        check(computeProgram->getEntryPointDesc(entry, entryDesc), "depth clipping entry ABI");
        require(entryDesc.threadGroupSize[0] == 8 && entryDesc.threadGroupSize[1] == 8 &&
                    entryDesc.threadGroupSize[2] == 1 && entryDesc.entryParamsSize == 0,
                "Depth clipping threadgroup/entry ABI");
        check(computeProgram->getBindingLayoutDesc(entry, 0, layout), "depth clipping reflection");
        const ResourceType types[] = {ResourceType::ConstantBuffer,       ResourceType::RawBuffer_SRV,
                                      ResourceType::RawBuffer_SRV,        ResourceType::RawBuffer_SRV,
                                      ResourceType::StructuredBuffer_SRV, ResourceType::StructuredBuffer_SRV,
                                      ResourceType::Texture_SRV,          ResourceType::Texture_UAV};
        const uint32_t slots[] = {0, 0, 1, 2, 3, 4, 5, 0};
        const uint32_t strides[] = {sizeof(DepthParameters), 1, 1, 1, sizeof(g::LevelInfo), sizeof(g::Float4), 0, 0};
        require(layout.bindings.size() == 8, "Depth clipping resource ABI");
        for (uint32_t i = 0; i < 8; ++i)
            require(std::count_if(layout.bindings.begin(), layout.bindings.end(),
                                  [&](const auto &b) { return b.type == types[i] && b.slot == slots[i]; }) == 1,
                    "Depth clipping binding ABI");
        uint32_t globals = 0, globalMask = 0, memberMask = 0, constantIndex = UINT32_MAX;
        std::vector<ShaderParameterDesc> parameters;
        for (uint32_t i = 0; i < computeProgram->getParameterCount(entry); ++i)
        {
            ShaderParameterDesc p;
            check(computeProgram->getParameterDesc(entry, i, p), "depth clipping parameter ABI");
            parameters.push_back(p);
            require(p.registerSpace == 0, "Depth clipping register space");
            if (p.scope == ShaderParameterScope::Global)
            {
                ++globals;
                require(p.count == 1 && p.parentParameter == UINT32_MAX, "Depth clipping global ABI");
                uint32_t found = 8;
                for (uint32_t j = 0; j < 8; ++j)
                    if (p.resourceType == types[j] && p.registerIndex == slots[j])
                        found = j;
                require(found < 8, "Depth clipping unexpected global");
                require(!(globalMask & (1u << found)) && (!strides[found] || p.elementStride == strides[found]),
                        "Depth clipping resource stride");
                globalMask |= 1u << found;
                if (found == 0)
                    constantIndex = i;
            }
            else
            {
                require(p.scope == ShaderParameterScope::ConstantBuffer && p.resourceType == ResourceType::None &&
                            p.elementStride == 16 && p.size && p.size % 16 == 0 && p.count == p.size / 16 &&
                            p.offset % 16 == 0 && uint64_t(p.offset) + p.size <= sizeof(DepthParameters),
                        "Depth clipping constant member ABI");
                for (uint32_t offset = p.offset; offset < p.offset + p.size; offset += 16)
                {
                    const auto bit = 1u << (offset / 16);
                    require(!(memberMask & bit), "Depth clipping overlapping constants");
                    memberMask |= bit;
                }
            }
        }
        require(globals == 8 && globalMask == 255 && memberMask == ((1u << (sizeof(DepthParameters) / 16)) - 1) &&
                    constantIndex != UINT32_MAX,
                "Depth clipping constant block ABI");
        for (const auto &p : parameters)
            if (p.scope == ShaderParameterScope::ConstantBuffer)
                require(p.parentParameter == constantIndex, "Depth clipping constant parent ABI");
        ShaderHandle shader;
        check(computeProgram->createShader(entry, &shader), "depth clipping shader");
        check(co->createBindingLayout(layout, &computeLayout), "depth clipping layout");
        check(co->createComputePipeline(ComputePipelineDesc().setComputeShader(shader).addBindingLayout(computeLayout),
                                        &computePipeline),
              "depth clipping pipeline");
    }
    ~Impl()
    {
        compute->waitForIdle();
        graphics->waitForIdle();
    }
    void setMesh(const Mesh &mesh)
    {
        require(mesh.positions.size() == mesh.normals.size() && mesh.indices.size() % 3 == 0, "Invalid depth-map mesh");
        require(mesh.positions.size() <= UINT32_MAX / sizeof(dm::float3) &&
                    mesh.indices.size() <= UINT32_MAX / sizeof(uint32_t),
                "Depth-map mesh exceeds 32-bit resource limits");
        for (size_t i = 0; i < mesh.positions.size(); ++i)
        {
            for (unsigned axis = 0; axis < 3; ++axis)
                require(std::isfinite(mesh.positions[i][axis]) && std::isfinite(mesh.normals[i][axis]),
                        "Nonfinite depth-map vertex");
            require(dm::lengthSquared(mesh.normals[i]) > 0, "Zero depth-map normal");
        }
        for (uint32_t index : mesh.indices)
            require(index < mesh.positions.size(), "Depth-map index outside vertex array");
        graphics->waitForIdle();
        indexCount = uint32_t(mesh.indices.size());
        positions = nullptr;
        normals = nullptr;
        indices = nullptr;
        if (!indexCount)
            return;
        rasterCommands->open();
        auto upload = [&](BufferHandle &result, const void *data, size_t bytes, bool index) {
            BufferDesc d;
            d.byteSize = bytes;
            d.isIndexBuffer = index;
            d.isVertexBuffer = !index;
            d.initialState = ResourceStates::CopyDest;
            check(graphics->createBuffer(d, &result), "mesh buffer");
            rasterCommands->beginTrackingBufferState(result, ResourceStates::CopyDest);
            rasterCommands->writeBuffer(result, data, bytes);
            rasterCommands->setPermanentBufferState(result,
                                                    index ? ResourceStates::IndexBuffer : ResourceStates::VertexBuffer);
        };
        upload(positions, mesh.positions.data(), mesh.positions.size() * sizeof(dm::float3), false);
        upload(normals, mesh.normals.data(), mesh.normals.size() * sizeof(dm::float3), false);
        upload(indices, mesh.indices.data(), mesh.indices.size() * 4, true);
        submit(graphics, rasterCommands);
    }
    void resize(uint32_t w, uint32_t h)
    {
        if (width == w && height == h)
            return;
        require(w && h && uint64_t(w) * h <= 16 * 1024 * 1024, "Invalid depth-map image dimensions");
        compute->waitForIdle();
        graphics->waitForIdle();
        computeBindings = nullptr;
        compositeBindings = nullptr;
        rasterFramebuffer = nullptr;
        finalFramebuffer = nullptr;
        importedDepth = nullptr;
        importedColor = nullptr;
        depthMemory = nullptr;
        colorMemory = nullptr;
        rasterPipeline = nullptr;
        compositePipeline = nullptr;
        width = w;
        height = h;
        first = true;
        TextureDesc d;
        d.width = w;
        d.height = h;
        d.isRenderTarget = true;
        d.format = Format::RGBA8_UNORM;
        d.initialState = ResourceStates::RenderTarget;
        d.keepInitialState = true;
        check(graphics->createTexture(d, &meshColor), "polygon color target");
        check(graphics->createTexture(d, &finalColor), "composited color target");
        check(graphics->createFramebuffer(FramebufferDesc().addColorAttachment(finalColor), &finalFramebuffer),
              "composition framebuffer");
        d.format = Format::D24S8;
        d.isTypeless = true;
        d.initialState = ResourceStates::DepthWrite;
        check(graphics->createTexture(d, &depthStencil), "hardware depth target");
        d.format = Format::R32_FLOAT;
        d.isTypeless = false;
        d.initialState = ResourceStates::Common;
        d.keepInitialState = false;
        d.sharedResourceFlags = textureSharingFlags;
        check(graphics->createTexture(d, &rasterDepth), "projected depth target");
        if (exporter)
        {
            check(exporter->exportTexture(rasterDepth, &depthMemory), "export projected depth");
            check(importer->importTexture(depthMemory->getDesc(), d, &importedDepth), "import projected depth");
        }
        else
            importedDepth = rasterDepth;
        d.format = Format::R32_UINT;
        d.isRenderTarget = false;
        d.isUAV = true;
        check(graphics->createTexture(d, &volumeColor), "packed volume image");
        if (exporter)
        {
            check(exporter->exportTexture(volumeColor, &colorMemory), "export volume image");
            check(importer->importTexture(colorMemory->getDesc(), d, &importedColor), "import volume image");
        }
        else
            importedColor = volumeColor;
        check(graphics->createFramebuffer(FramebufferDesc()
                                              .addColorAttachment(meshColor)
                                              .addColorAttachment(rasterDepth)
                                              .setDepthAttachment(depthStencil),
                                          &rasterFramebuffer),
              "mesh MRT framebuffer");
        GraphicsPipelineDesc pipeline;
        pipeline.VS = meshVS;
        pipeline.PS = meshPS;
        pipeline.inputLayout = inputLayout;
        pipeline.bindingLayouts = {rasterLayout};
        pipeline.primType = PrimitiveType::TriangleList;
        pipeline.renderState.rasterState.cullMode = RasterCullMode::None;
        pipeline.renderState.depthStencilState.depthTestEnable = true;
        pipeline.renderState.depthStencilState.depthWriteEnable = true;
        pipeline.renderState.depthStencilState.depthFunc = ComparisonFunc::LessOrEqual;
        check(graphics->createGraphicsPipeline1(pipeline, rasterFramebuffer->getFramebufferInfo().getInfo(),
                                                &rasterPipeline),
              "mesh raster pipeline");
        BindingSetDesc bindings;
        bindings.bindings = {BindingSetItem::Texture_SRV(0, meshColor), BindingSetItem::Texture_SRV(1, volumeColor)};
        check(graphics->createBindingSet(bindings, compositeLayout, &compositeBindings), "composition bindings");
        bindings.bindings = {
            BindingSetItem::ConstantBuffer(0, depthConstants),
            BindingSetItem::RawBuffer_SRV(0, scalar.raw.buffer, {scalar.raw.byteOffset, scalar.raw.byteSize}),
            BindingSetItem::RawBuffer_SRV(1, nodes.buffer, {nodes.byteOffset, nodes.byteSize}),
            BindingSetItem::RawBuffer_SRV(2, children.buffer, {children.byteOffset, children.byteSize}),
            BindingSetItem::StructuredBuffer_SRV(3, levels.buffer, Format::UNKNOWN,
                                                 {levels.byteOffset, levels.byteSize}),
            BindingSetItem::StructuredBuffer_SRV(4, transfer),
            BindingSetItem::Texture_SRV(5, importedDepth),
            BindingSetItem::Texture_UAV(0, importedColor)};
        check(compute->createBindingSet(bindings, computeLayout, &computeBindings), "depth clipping bindings");
    }
    void render(uint32_t w, uint32_t h, const View &view, IFramebuffer *target)
    {
        require(std::isfinite(view.nearPlane) && std::isfinite(view.farPlane) && view.nearPlane > 0 &&
                    view.farPlane > view.nearPlane && std::isfinite(view.step) && view.step > 0 &&
                    std::isfinite(view.bias) && view.bias >= 0,
                "Invalid depth-map projection/step/bias");
        resize(w, h);
        if (!target)
            target = finalFramebuffer;
        require(target->getFramebufferInfo().width == w && target->getFramebufferInfo().height == h,
                "Mismatched composition target size");
        const float horizontal = std::tan(dm::radians(25.f)), vertical = horizontal * float(h) / w;
        const auto p = detail::clipParameters(volumeDesc.volume, w, h, view);
        RasterParameters r{};
        r.viewProjection = dm::affineToHomogeneous(dm::inverse(view.cameraToWorld)) *
                           dm::perspProjD3DStyle(2 * std::atan(vertical), float(w) / h, view.nearPlane, view.farPlane);
        r.eye = dm::float4(view.cameraToWorld.m_translation, 1);
        r.light = dm::float4(view.light, 1);
        rasterCommands->open();
        rasterCommands->beginTrackingTextureState(rasterDepth, AllSubresources,
                                                  first ? ResourceStates::Common : ResourceStates::RenderTarget);
        rasterCommands->beginTrackingTextureState(volumeColor, AllSubresources,
                                                  first ? ResourceStates::Common : ResourceStates::ShaderResource);
        rasterCommands->clearTextureFloat(meshColor, AllSubresources, Color(.1f, .2f, .4f, 1));
        rasterCommands->clearTextureFloat(rasterDepth, AllSubresources, Color(1));
        rasterCommands->clearDepthStencilTexture(depthStencil, AllSubresources, true, 1, true, 0);
        writeConstants(rasterCommands, rasterConstants, &r, sizeof(r));
        if (indexCount)
        {
            GraphicsState state;
            state.pipeline = rasterPipeline;
            state.framebuffer = rasterFramebuffer;
            state.bindings = {rasterBindings};
            state.vertexBuffers = {VertexBufferBinding{positions, 0, 0}, VertexBufferBinding{normals, 1, 0}};
            state.indexBuffer = IndexBufferBinding{indices, Format::R32_UINT, 0};
            state.viewport.addViewportAndScissorRect(Viewport(float(w), float(h)));
            rasterCommands->setGraphicsState(state);
            DrawArguments args;
            args.vertexCount = indexCount;
            rasterCommands->drawIndexed(args);
        }
        ICommandList *active = rasterCommands;
        if (exporter)
        {
            check(exporter->releaseResource(rasterCommands, depthMemory), "release depth to CUDA");
            check(exporter->releaseResource(rasterCommands, colorMemory), "release output to CUDA");
            submit(graphics, rasterCommands);
            check(exporter->signalFence(CommandQueue::Graphics, fence, frame * 2 + 1), "graphics producer signal");
            computeCommands->open();
            check(importer->waitFence(computeCommands, importedFence, frame * 2 + 1), "CUDA depth wait");
            active = computeCommands;
        }
        writeConstants(active, depthConstants, &p, sizeof(p));
        ComputeState computeState;
        computeState.pipeline = computePipeline;
        computeState.bindings = {computeBindings};
        active->setComputeState(computeState);
        active->dispatch((w + 7) / 8, (h + 7) / 8, 1);
        if (exporter)
        {
            check(importer->signalFence(computeCommands, importedFence, frame * 2 + 2), "CUDA output signal");
            submit(compute, computeCommands);
            check(exporter->waitFence(CommandQueue::Graphics, fence, frame * 2 + 2), "graphics output wait");
            compositeCommands->open();
            check(exporter->acquireResource(compositeCommands, depthMemory, ResourceStates::RenderTarget),
                  "reacquire depth");
            check(exporter->acquireResource(compositeCommands, colorMemory, ResourceStates::ShaderResource),
                  "acquire CUDA output");
            active = compositeCommands;
        }
        else
            active->setTextureState(rasterDepth, AllSubresources, ResourceStates::RenderTarget);
        if (!compositePipeline)
        {
            GraphicsPipelineDesc desc;
            desc.VS = compositeVS;
            desc.PS = compositePS;
            desc.bindingLayouts = {compositeLayout};
            desc.primType = PrimitiveType::TriangleStrip;
            desc.renderState.rasterState.cullMode = RasterCullMode::None;
            desc.renderState.depthStencilState.depthTestEnable = false;
            desc.renderState.depthStencilState.depthWriteEnable = false;
            check(graphics->createGraphicsPipeline1(desc, target->getFramebufferInfo().getInfo(), &compositePipeline),
                  "composition pipeline");
        }
        GraphicsState state;
        state.pipeline = compositePipeline;
        state.framebuffer = target;
        state.bindings = {compositeBindings};
        state.viewport.addViewportAndScissorRect(Viewport(float(w), float(h)));
        active->setGraphicsState(state);
        DrawArguments args;
        args.vertexCount = 4;
        active->draw(args);
        submit(graphics, active);
        first = false;
        ++frame;
        graphics->runGarbageCollection();
        if (compute != graphics)
            compute->runGarbageCollection();
    }
    std::vector<uint8_t> readTexture(ITexture *texture, ResourceStates state)
    {
        auto desc = texture->getDesc();
        desc.sharedResourceFlags = SharedResourceFlags::None;
        desc.isUAV = false;
        desc.isRenderTarget = false;
        StagingTextureHandle staging;
        check(graphics->createStagingTexture(desc, CpuAccessMode::Read, &staging), "create screenshot readback");
        compositeCommands->open();
        compositeCommands->beginTrackingTextureState(texture, AllSubresources, state);
        compositeCommands->copyTexture2(staging, {}, texture, {});
        compositeCommands->setTextureState(texture, AllSubresources, state);
        compositeCommands->commitBarriers();
        submit(graphics, compositeCommands);
        require(graphics->waitForIdle(), "Screenshot GPU completion failed");
        size_t pitch = 0;
        const auto *data =
            static_cast<const uint8_t *>(graphics->mapStagingTexture(staging, {}, CpuAccessMode::Read, pitch));
        require(data && pitch >= size_t(width) * 4, "Invalid screenshot map");
        std::vector<uint8_t> result(size_t(width) * height * 4);
        for (uint32_t y = 0; y < height; ++y)
            std::memcpy(result.data() + size_t(y) * width * 4, data + size_t(y) * pitch, size_t(width) * 4);
        graphics->unmapStagingTexture(staging);
        return result;
    }
    Images read()
    {
        require(width && height && !first, "Render before readback");
        Images result;
        result.width = width;
        result.height = height;
        result.color = readTexture(finalColor, ResourceStates::RenderTarget);
        result.polygon = readTexture(meshColor, ResourceStates::RenderTarget);
        auto depth = readTexture(rasterDepth, ResourceStates::RenderTarget),
             packed = readTexture(volumeColor, ResourceStates::ShaderResource);
        result.depth.resize(size_t(width) * height);
        result.volume.resize(size_t(width) * height);
        std::memcpy(result.depth.data(), depth.data(), depth.size());
        std::memcpy(result.volume.data(), packed.data(), packed.size());
        return result;
    }
};
Renderer::Renderer(IDevice *graphics, IDevice *compute, const Scene &scene)
    : m_impl(std::make_unique<Impl>(graphics, compute, scene))
{
    m_impl->setMesh(scene.mesh);
}
Renderer::~Renderer() = default;
void Renderer::setMesh(const Mesh &mesh)
{
    m_impl->setMesh(mesh);
}
void Renderer::render(uint32_t width, uint32_t height, const View &view, IFramebuffer *target)
{
    m_impl->render(width, height, view, target);
}
Images Renderer::read()
{
    return m_impl->read();
}
} // namespace depth_map
