// OptiX renderer for a GVDBSceneGraph (OptixRenderer.h). Port of the reference OptixScene (optix_scene.cpp) onto
// the ray tracing interfaces of the nvrhi CUDA device.
//
// Scene organisation
//   * one custom-primitive BLAS (a single AABB = the voxel bounds in index space) per volume instance, rebuilt by
//     updateVolumes();
//   * one triangle BLAS per distinct MeshInfo, from the CPU buffers of the mesh (positions, decoded normals,
//     indices) uploaded to device buffers;
//   * one TLAS holding one instance per volume instance and per mesh instance, with the node's local-to-world
//     transform (index -> world for volumes), rebuilt by updateTransforms().
//
// Launch parameters, tables and shader table
//   The programs read everything from the launch parameters (a push-constant block): the TLAS, the device tables
//   of the volumes, the materials and the TLAS instances (OptixInstanceData: kind, volume, material, mesh
//   buffers; a hit finds its entry by InstanceID()). The shader records carry no data, so the shader table only
//   picks the hit groups: GVDB_RT_RAY_TYPE_COUNT (2: radiance, shadow) per instance in TLAS order (the instance's
//   hit group offset is 2 * instance index), a volume's by its intersection mode (surface / deep / level set /
//   empty skip). render() refills the tables and the hit groups, so shading and material changes apply at once.
//
//   The TLAS handle in the launch parameters is the one of its last build, which the CUDA device knows once the
//   build is submitted: render() flushes the context after a TLAS build before it reads the handle.
#include "OptixRenderer.h"
#include "SampleTypes.h"
#include <sample-utils/kernels/optix/OptixConstants.h>
#include <donut/core/log.h>
#include <donut/core/vfs/VFS.h>
#include <donut/engine/SceneTypes.h>
#include <nvrhi/core/datablob.h>
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

namespace SampleUtils {

using namespace donut;
using namespace donut::engine;

namespace {

constexpr uint32_t c_NumRayTypes = GVDB_RT_RAY_TYPE_COUNT;
constexpr uint32_t c_NumSeeds = GVDB_RT_SEEDS_DIM * GVDB_RT_SEEDS_DIM;
constexpr nvrhi::rt::AccelStructBuildFlags c_BuildFlags = nvrhi::rt::AccelStructBuildFlags::PreferFastTrace;

// ---- host mirrors of the structs of OptixPrograms.slang (CUDA layouts) ----

struct OptixVolumeInstance {
    uint64_t gvdb;        // VDBInfo*
    uint64_t transfer;    // float4*
    dm::float3 thresh;
    int32_t channel;
    dm::float3 steps;
    int32_t colorChannel;
    dm::float3 extinct;
    float epsilon;
    dm::float3 cutoff;
    int32_t pad;
};
static_assert(sizeof(OptixVolumeInstance) == 80 && offsetof(OptixVolumeInstance, cutoff) == 64,
              "OptixVolumeInstance differs from OptixPrograms.slang");

struct OptixMaterial {
    dm::float3 ambColor;
    float lightWidth;
    dm::float3 envColor;
    float specPower;
    dm::float3 diffColor;
    float shadowWidth;
    dm::float3 specColor;
    float shadowBias;
    dm::float3 reflColor;
    float reflWidth;
    dm::float3 refrColor;
    float reflBias;
    float refrWidth;
    float refrIor;
    float refrAmount;
    float refrOffset;
    float refrBias;
    float pad[3];
};
static_assert(sizeof(OptixMaterial) == 128, "OptixMaterial differs from OptixPrograms.slang");

struct OptixInstanceData {
    uint32_t kind;
    uint32_t volumeIndex;
    int32_t materialId;
    uint32_t numTriangles;
    uint64_t positions;   // float3*
    uint64_t normals;     // float3* or 0
    uint64_t indices;     // uint3*
};
static_assert(sizeof(OptixInstanceData) == 40, "OptixInstanceData differs from OptixPrograms.slang");

struct OptixLaunchParams {
    uint64_t scene;       // OptixTraversableHandle
    uint64_t volumes;
    uint64_t materials;
    uint64_t instances;
    uint64_t accum;
    uint64_t output;
    uint64_t seeds;
    uint64_t envmap;
    alignas(16) dm::float4 backclr;
    dm::float3 eye;
    uint32_t numMaterials;
    dm::float3 rayTL;
    uint32_t width;
    dm::float3 rayU;
    uint32_t height;
    dm::float3 rayV;
    uint32_t sample;
    dm::float3 lightPos;
    uint32_t envWidth;
    uint32_t envHeight;
    uint32_t pad[3];
};
static_assert(sizeof(OptixLaunchParams) == 176 && offsetof(OptixLaunchParams, backclr) == 64 &&
                  offsetof(OptixLaunchParams, lightPos) == 144,
              "OptixLaunchParams differs from OptixPrograms.slang");

// Hit group export names: per intersection mode (GVDB_RT_ISECT_*) for volumes, radiance and shadow.
const char *const c_VolumeRadianceGroups[GVDB_RT_ISECT_COUNT] = {"volumeSurface", "volumeDeep", "volumeLevelSet",
                                                                   "volumeEmptySkip"};
const char *const c_VolumeShadowGroups[GVDB_RT_ISECT_COUNT] = {"volumeSurfaceShadow", "volumeDeepShadow",
                                                                 "volumeLevelSetShadow", "volumeEmptySkipShadow"};
const char *const c_IntersectionPrograms[GVDB_RT_ISECT_COUNT] = {"intersectSurface", "intersectDeep",
                                                                   "intersectLevelSet", "intersectEmptySkip"};

// Donut affine (row vectors, p' = p * M) -> instance transform (row-major 3x4, column vectors).
void affineToInstance(const dm::affine3 &m, float out[12]) {
    const float t[3] = {m.m_translation.x, m.m_translation.y, m.m_translation.z};
    for (int j = 0; j < 3; ++j) {
        for (int i = 0; i < 3; ++i) out[j * 4 + i] = m.m_linear.m_data[i * 3 + j];
        out[j * 4 + 3] = t[j];
    }
}

// Donut packs vertex normals as 8-bit snorm (vectorToSnorm8).
dm::float3 decodeSnorm8(uint32_t v) {
    auto comp = [](uint32_t bits) {
        const int8_t s = int8_t(bits & 0xFF);
        return std::max(-1.f, float(s) / 127.f);
    };
    return dm::float3(comp(v), comp(v >> 8), comp(v >> 16));
}

OptixMaterial toDevice(const OptixMaterialParams &p) {
    OptixMaterial m = {};
    m.ambColor = p.ambColor;
    m.lightWidth = p.lightWidth;
    m.envColor = p.envColor;
    m.specPower = p.specPower;
    m.diffColor = p.diffColor;
    m.shadowWidth = p.shadowWidth;
    m.specColor = p.specColor;
    m.shadowBias = p.shadowBias;
    m.reflColor = p.reflColor;
    m.reflWidth = p.reflWidth;
    m.refrColor = p.refrColor;
    m.reflBias = p.reflBias;
    m.refrWidth = p.refrWidth;
    m.refrIor = p.refrIor;
    m.refrAmount = p.refrAmount;
    m.refrOffset = p.refrOffset;
    m.refrBias = p.refrBias;
    return m;
}

int shadingToIsect(gvdb::VolumeShading shading) {
    switch (shading) {
        case gvdb::VolumeShading::LevelSet:
            return GVDB_RT_ISECT_LEVELSET;
        case gvdb::VolumeShading::Volume:
            return GVDB_RT_ISECT_DEEP;
        case gvdb::VolumeShading::EmptySkip:
            return GVDB_RT_ISECT_EMPTYSKIP;
        default:
            return GVDB_RT_ISECT_SURFACE;
    }
}

}  // namespace

struct OptixRenderer::Impl {
    nvrhi::AutoPtr<gvdb::IComputeContext> context;
    nvrhi::cuda::IDevice *device = nullptr;   // the context's

    // ---- pipeline ----
    nvrhi::ShaderLibraryHandle library;
    nvrhi::rt::PipelineHandle pipeline;
    nvrhi::rt::ShaderTableHandle shaderTable;
    bool pipelineOk = false;

    // ---- output ----
    uint32_t width = 0, height = 0;
    nvrhi::BufferHandle accumBuffer;    // float4
    nvrhi::BufferHandle outputBuffer;   // RGBA8

    // ---- scene-wide ----
    VolumeViewParams viewParams;
    std::vector<OptixMaterialParams> materials;
    nvrhi::BufferHandle materialBuffer;
    bool materialsDirty = true;
    nvrhi::BufferHandle envmap;   // float4 texels, sampled bilinearly by the programs
    uint32_t envWidth = 0, envHeight = 0;
    nvrhi::BufferHandle seedBuffer;
    nvrhi::AutoPtr<gvdb::VolumeTransferFunction> defaultTransfer;
    int meshMaterial = 0;
    std::map<const MeshInstance *, int> meshMaterialOverrides;

    // ---- scene ----
    struct VolumeEntry {
        nvrhi::AutoPtr<gvdb::GVDBVolumeInstance> instance;
        nvrhi::BufferHandle aabbBuffer;
        nvrhi::rt::AccelStructHandle blas;
    };
    struct MeshEntry {
        nvrhi::AutoPtr<MeshInfo> mesh;
        nvrhi::BufferHandle positions;
        nvrhi::BufferHandle normals;
        nvrhi::BufferHandle indices;
        uint32_t numVertices = 0;
        uint32_t numTriangles = 0;
        nvrhi::rt::AccelStructHandle blas;
    };
    enum class InstanceKind { Volume, Mesh };
    struct InstanceEntry {
        InstanceKind kind = InstanceKind::Volume;
        int index = 0;   // into volumes / meshes
        nvrhi::AutoPtr<MeshInstance> meshInstance;
    };
    std::vector<VolumeEntry> volumes;
    std::vector<MeshEntry> meshes;
    std::vector<InstanceEntry> instances;
    std::vector<OptixVolumeInstance> volumeTable;
    std::vector<OptixInstanceData> instanceTable;
    nvrhi::BufferHandle volumeTableBuffer;
    nvrhi::BufferHandle instanceTableBuffer;
    nvrhi::rt::AccelStructHandle tlas;
    uint64_t traversable = 0;
    bool traversablePending = false;   // a TLAS build was recorded since the handle was read

    nvrhi::ICommandList *commandList() { return context->getCommandList(); }

    nvrhi::BufferHandle createBuffer(size_t bytes, const char *name, bool buildInput = false) {
        nvrhi::BufferDesc desc;
        desc.byteSize = std::max<size_t>(bytes, 4);
        desc.debugName = name;
        desc.isAccelStructBuildInput = buildInput;
        nvrhi::BufferHandle buffer;
        UT_CHECK(device->createBuffer(desc, &buffer));
        return buffer;
    }

    // Before an object is dropped or replaced: the programs reach buffers through device addresses and the TLAS
    // through its handle, so it lives until the recorded work is done.
    void retire(nvrhi::IObject *object) { context->releaseAfterUse(object); }

    // `buffer` when it holds at least `bytes`, else a new buffer (the old one retired).
    nvrhi::BufferHandle grow(nvrhi::IBuffer *buffer, size_t bytes, const char *name) {
        if (buffer && buffer->getDesc().byteSize >= bytes) return buffer;
        retire(buffer);
        return createBuffer(bytes, name);
    }

    bool createPipeline();
    void uploadMaterials();
    int effectiveIsect(const VolumeEntry &e, gvdb::VolumeShading overrideShading) const;
    uint64_t transferOf(const gvdb::VolumeRenderAttributes &a);
    void fillTables(gvdb::VolumeShading overrideShading);
    void writeVolumeAabb(VolumeEntry &e);
    nvrhi::rt::GeometryDesc volumeGeometry(const VolumeEntry &e) const;
    bool uploadMesh(MeshEntry &e, bool recreate);
    nvrhi::rt::GeometryDesc meshGeometry(const MeshEntry &e) const;
    nvrhi::rt::AccelStructHandle buildBlas(const nvrhi::rt::GeometryDesc &geometry, const char *name);
    void buildInstances();
    int meshMaterialOf(const MeshInstance *instance) const;
};

// ---------------------------------------------------------------------------
// Pipeline
// ---------------------------------------------------------------------------

bool OptixRenderer::Impl::createPipeline() {
    if (!device->isOptixAvailable()) {
        log::error("OptixRenderer: the CUDA device has no OptiX");
        return false;
    }
    nvrhi::AutoPtr<nvrhi::IDataBlob> blob;
    vfs::IFileSystem *fs = context->getFileSystem();
    if (!fs || NVRHI_FAILED(fs->readFile(GVDB_SHADERS_OPTIX, &blob)) || !blob) {
        log::error("OptixRenderer: cannot read %s", GVDB_SHADERS_OPTIX);
        return false;
    }
    if (NVRHI_FAILED(device->createShaderLibrary(blob->GetDataPtr(), blob->GetSize(), &library)) || !library) {
        log::error("OptixRenderer: cannot load %s", GVDB_SHADERS_OPTIX);
        return false;
    }

    bool ok = true;
    auto program = [&](const char *name, nvrhi::ShaderType type) {
        nvrhi::ShaderHandle shader;
        if (NVRHI_FAILED(library->getShader(name, type, &shader)) || !shader) {
            log::error("OptixRenderer: %s has no program %s", GVDB_SHADERS_OPTIX, name);
            ok = false;
        }
        return shader;
    };
    nvrhi::ShaderHandle rayGeneration = program("rayGeneration", nvrhi::ShaderType::RayGeneration);
    nvrhi::ShaderHandle missRadiance = program("missRadiance", nvrhi::ShaderType::Miss);
    nvrhi::ShaderHandle missShadow = program("missShadow", nvrhi::ShaderType::Miss);
    nvrhi::ShaderHandle surfaceHit = program("surfaceHit", nvrhi::ShaderType::ClosestHit);
    nvrhi::ShaderHandle deepHit = program("deepHit", nvrhi::ShaderType::ClosestHit);
    nvrhi::ShaderHandle meshHit = program("meshHit", nvrhi::ShaderType::ClosestHit);
    nvrhi::ShaderHandle shadowHit = program("shadowHit", nvrhi::ShaderType::AnyHit);
    nvrhi::ShaderHandle intersection[GVDB_RT_ISECT_COUNT];
    for (int i = 0; i < GVDB_RT_ISECT_COUNT; ++i)
        intersection[i] = program(c_IntersectionPrograms[i], nvrhi::ShaderType::Intersection);
    if (!ok) return false;

    using namespace nvrhi::rt;
    PipelineDesc desc;
    desc.addShader(PipelineShaderDesc().setExportName("rayGeneration").setShader(rayGeneration))
        .addShader(PipelineShaderDesc().setExportName("missRadiance").setShader(missRadiance))
        .addShader(PipelineShaderDesc().setExportName("missShadow").setShader(missShadow));
    for (int i = 0; i < GVDB_RT_ISECT_COUNT; ++i) {
        desc.addHitGroup(PipelineHitGroupDesc()
                             .setExportName(c_VolumeRadianceGroups[i])
                             .setIntersectionShader(intersection[i])
                             .setClosestHitShader(i == GVDB_RT_ISECT_DEEP ? deepHit : surfaceHit)
                             .setIsProceduralPrimitive(true));
        desc.addHitGroup(PipelineHitGroupDesc()
                             .setExportName(c_VolumeShadowGroups[i])
                             .setIntersectionShader(intersection[i])
                             .setAnyHitShader(shadowHit)
                             .setIsProceduralPrimitive(true));
    }
    desc.addHitGroup(PipelineHitGroupDesc().setExportName("mesh").setClosestHitShader(meshHit))
        .addHitGroup(PipelineHitGroupDesc().setExportName("meshShadow").setAnyHitShader(shadowHit))
        .setMaxPayloadSize(GVDB_RT_PAYLOAD_SIZE)
        .setMaxAttributeSize(GVDB_RT_ATTRIBUTE_SIZE)
        .setMaxRecursionDepth(GVDB_RT_MAX_TRACE_DEPTH);
    if (NVRHI_FAILED(device->createRayTracingPipeline(desc, &pipeline)) || !pipeline) {
        log::error("OptixRenderer: OptiX pipeline creation failed");
        return false;
    }
    if (NVRHI_FAILED(pipeline->createShaderTable(ShaderTableDesc(), &shaderTable)) || !shaderTable) {
        log::error("OptixRenderer: shader table creation failed");
        return false;
    }
    shaderTable->setRayGenerationShader("rayGeneration");
    shaderTable->addMissShader("missRadiance");   // GVDB_RT_RAY_RADIANCE
    shaderTable->addMissShader("missShadow");     // GVDB_RT_RAY_SHADOW
    return true;
}

// ---------------------------------------------------------------------------
// Materials / tables
// ---------------------------------------------------------------------------

void OptixRenderer::Impl::uploadMaterials() {
    std::vector<OptixMaterial> table;
    if (materials.empty())
        table.push_back(toDevice(OptixMaterialParams()));
    else
        for (const auto &m : materials) table.push_back(toDevice(m));

    const size_t bytes = table.size() * sizeof(OptixMaterial);
    materialBuffer = grow(materialBuffer, bytes, "OptixRenderer materials");
    commandList()->writeBuffer(materialBuffer, table.data(), bytes, 0);
    materialsDirty = false;
}

int OptixRenderer::Impl::effectiveIsect(const VolumeEntry &e, gvdb::VolumeShading overrideShading) const {
    const gvdb::VolumeShading shading =
        overrideShading != gvdb::VolumeShading::Off ? overrideShading : e.instance->GetRenderAttributes().shading;
    return shadingToIsect(shading);
}

uint64_t OptixRenderer::Impl::transferOf(const gvdb::VolumeRenderAttributes &a) {
    gvdb::VolumeTransferFunction *tf = a.transferFunction.Get();
    if (!tf) {
        if (!defaultTransfer) {
            // white, opacity ramp 0..1 over the value range
            defaultTransfer = MAKE_RC_OBJ_PTR(gvdb::VolumeTransferFunction, context.Get());
            defaultTransfer->setLinear(0.f, 1.f, dm::float4(1.f, 1.f, 1.f, 0.f), dm::float4(1.f, 1.f, 1.f, 1.f));
            defaultTransfer->commit();
        }
        tf = defaultTransfer.Get();
    }
    if (!tf->getGPU()) tf->commit();
    return gvdb::deviceAddress(tf->getGPU());
}

// The volume and instance tables of the programs and the hit groups of the shader table, from the current
// attributes of the instances.
void OptixRenderer::Impl::fillTables(gvdb::VolumeShading overrideShading) {
    volumeTable.assign(volumes.size(), OptixVolumeInstance{});
    for (size_t i = 0; i < volumes.size(); ++i) {
        const gvdb::VolumeRenderAttributes &a = volumes[i].instance->GetRenderAttributes();
        OptixVolumeInstance &v = volumeTable[i];
        v.gvdb = gvdb::deviceAddress(volumes[i].instance->GetVolume()->getVDBInfoGPU());
        v.transfer = transferOf(a);
        v.thresh = a.threshold;
        v.channel = a.channel;
        v.steps = a.steps;
        v.colorChannel = a.colorChannel;
        v.extinct = a.extinct;
        v.epsilon = a.epsilon;
        v.cutoff = a.cutoff;
    }

    instanceTable.assign(instances.size(), OptixInstanceData{});
    shaderTable->clearHitShaders();
    for (size_t i = 0; i < instances.size(); ++i) {
        const InstanceEntry &inst = instances[i];
        OptixInstanceData &rec = instanceTable[i];
        if (inst.kind == InstanceKind::Volume) {
            const VolumeEntry &e = volumes[inst.index];
            const int isect = effectiveIsect(e, overrideShading);
            rec.kind = GVDB_RT_HIT_VOLUME;
            rec.volumeIndex = uint32_t(inst.index);
            rec.materialId = e.instance->GetRenderAttributes().materialId;
            shaderTable->addHitGroup(c_VolumeRadianceGroups[isect]);   // GVDB_RT_RAY_RADIANCE
            shaderTable->addHitGroup(c_VolumeShadowGroups[isect]);     // GVDB_RT_RAY_SHADOW
        } else {
            const MeshEntry &e = meshes[inst.index];
            rec.kind = GVDB_RT_HIT_MESH;
            rec.materialId = meshMaterialOf(inst.meshInstance.Get());
            rec.numTriangles = e.numTriangles;
            rec.positions = gvdb::deviceAddress(e.positions);
            rec.normals = gvdb::deviceAddress(e.normals);
            rec.indices = gvdb::deviceAddress(e.indices);
            shaderTable->addHitGroup("mesh");
            shaderTable->addHitGroup("meshShadow");
        }
    }

    nvrhi::ICommandList *cmd = commandList();
    volumeTableBuffer = grow(volumeTableBuffer, std::max<size_t>(1, volumeTable.size()) * sizeof(OptixVolumeInstance),
                             "OptixRenderer volumes");
    if (!volumeTable.empty())
        cmd->writeBuffer(volumeTableBuffer, volumeTable.data(), volumeTable.size() * sizeof(OptixVolumeInstance), 0);
    instanceTableBuffer = grow(instanceTableBuffer, std::max<size_t>(1, instanceTable.size()) * sizeof(OptixInstanceData),
                               "OptixRenderer instances");
    if (!instanceTable.empty())
        cmd->writeBuffer(instanceTableBuffer, instanceTable.data(), instanceTable.size() * sizeof(OptixInstanceData),
                         0);
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

void OptixRenderer::Impl::writeVolumeAabb(VolumeEntry &e) {
    const dm::box3 bounds = e.instance->GetLocalBoundingBox();
    nvrhi::rt::GeometryAABB aabb = {bounds.m_mins.x, bounds.m_mins.y, bounds.m_mins.z,
                                    bounds.m_maxs.x, bounds.m_maxs.y, bounds.m_maxs.z};
    // an empty volume gets a degenerate but valid box
    if (!(aabb.maxX > aabb.minX)) aabb.maxX = aabb.minX + 1e-3f;
    if (!(aabb.maxY > aabb.minY)) aabb.maxY = aabb.minY + 1e-3f;
    if (!(aabb.maxZ > aabb.minZ)) aabb.maxZ = aabb.minZ + 1e-3f;
    if (!e.aabbBuffer) e.aabbBuffer = createBuffer(sizeof(aabb), "OptixRenderer volume AABB", true);
    commandList()->writeBuffer(e.aabbBuffer, &aabb, sizeof(aabb), 0);
}

nvrhi::rt::GeometryDesc OptixRenderer::Impl::volumeGeometry(const VolumeEntry &e) const {
    nvrhi::rt::GeometryDesc geometry;
    geometry.setAABBs(nvrhi::rt::GeometryAABBs().setBuffer(e.aabbBuffer).setCount(1).setStride(
        sizeof(nvrhi::rt::GeometryAABB)));
    geometry.flags = nvrhi::rt::GeometryFlags::None;   // the shadow rays run the any-hit program
    return geometry;
}

bool OptixRenderer::Impl::uploadMesh(MeshEntry &e, bool recreate) {
    const MeshInfo *mesh = e.mesh.Get();
    if (!mesh || !mesh->buffers) return false;
    const BufferGroup &b = *mesh->buffers;

    const uint32_t v0 = mesh->vertexOffset;
    const uint32_t nv = mesh->totalVertices ? mesh->totalVertices : uint32_t(b.positionData.size());
    const uint32_t i0 = mesh->indexOffset;
    const uint32_t ni = mesh->totalIndices ? mesh->totalIndices : uint32_t(b.indexData.size());
    if (nv == 0 || ni < 3 || v0 + nv > b.positionData.size() || i0 + ni > b.indexData.size()) return false;

    const uint32_t numTriangles = ni / 3;
    if (recreate || e.numVertices != nv || e.numTriangles != numTriangles) {
        for (nvrhi::IBuffer *old : {e.positions.Get(), e.normals.Get(), e.indices.Get()}) retire(old);
        e.positions = createBuffer(size_t(nv) * sizeof(dm::float3), "OptixRenderer positions", true);
        e.normals = nullptr;
        e.indices = createBuffer(size_t(numTriangles) * 3 * sizeof(uint32_t), "OptixRenderer indices", true);
        e.numVertices = nv;
        e.numTriangles = numTriangles;
    }

    nvrhi::ICommandList *cmd = commandList();
    cmd->writeBuffer(e.positions, b.positionData.data() + v0, size_t(nv) * sizeof(dm::float3), 0);
    cmd->writeBuffer(e.indices, b.indexData.data() + i0, size_t(numTriangles) * 3 * sizeof(uint32_t), 0);
    if (b.normalData.size() >= size_t(v0) + nv) {
        std::vector<dm::float3> normals(nv);
        for (uint32_t i = 0; i < nv; ++i) normals[i] = decodeSnorm8(b.normalData[v0 + i]);
        if (!e.normals) e.normals = createBuffer(size_t(nv) * sizeof(dm::float3), "OptixRenderer normals");
        cmd->writeBuffer(e.normals, normals.data(), normals.size() * sizeof(dm::float3), 0);
    } else {
        retire(e.normals);
        e.normals = nullptr;
    }
    return true;
}

nvrhi::rt::GeometryDesc OptixRenderer::Impl::meshGeometry(const MeshEntry &e) const {
    nvrhi::rt::GeometryDesc geometry;
    geometry.setTriangles(nvrhi::rt::GeometryTriangles()
                              .setVertexBuffer(e.positions)
                              .setVertexFormat(nvrhi::Format::RGB32_FLOAT)
                              .setVertexCount(e.numVertices)
                              .setVertexStride(sizeof(dm::float3))
                              .setIndexBuffer(e.indices)
                              .setIndexFormat(nvrhi::Format::R32_UINT)
                              .setIndexCount(e.numTriangles * 3));
    geometry.flags = nvrhi::rt::GeometryFlags::None;
    return geometry;
}

// Creates a BLAS for the geometry and records its build.
nvrhi::rt::AccelStructHandle OptixRenderer::Impl::buildBlas(const nvrhi::rt::GeometryDesc &geometry, const char *name) {
    nvrhi::rt::AccelStructHandle blas;
    UT_CHECK(device->createAccelStruct(
        nvrhi::rt::AccelStructDesc().addBottomLevelGeometry(geometry).setBuildFlags(c_BuildFlags).setDebugName(name),
        &blas));
    if (blas) commandList()->buildBottomLevelAccelStruct(blas, &geometry, 1, c_BuildFlags);
    return blas;
}

int OptixRenderer::Impl::meshMaterialOf(const MeshInstance *instance) const {
    auto it = meshMaterialOverrides.find(instance);
    return it != meshMaterialOverrides.end() ? it->second : meshMaterial;
}

void OptixRenderer::Impl::buildInstances() {
    std::vector<nvrhi::rt::InstanceDesc> descs(instances.size());
    for (size_t i = 0; i < instances.size(); ++i) {
        const InstanceEntry &inst = instances[i];
        nvrhi::rt::InstanceDesc &d = descs[i];
        d.setInstanceID(uint32_t(i)).setInstanceContributionToHitGroupIndex(uint32_t(i) * c_NumRayTypes).setInstanceMask(0xFF);
        if (inst.kind == InstanceKind::Volume) {
            const VolumeEntry &e = volumes[inst.index];
            affineToInstance(e.instance->GetIndexToWorld(), d.transform);
            d.setBLAS(e.blas);
        } else {
            const MeshEntry &e = meshes[inst.index];
            if (SceneGraphNode *node = inst.meshInstance->GetNode())
                affineToInstance(node->GetLocalToWorldTransformFloat(), d.transform);
            d.setFlags(nvrhi::rt::InstanceFlags::TriangleCullDisable);
            d.setBLAS(e.blas);
        }
    }

    if (!tlas || tlas->getDesc().topLevelMaxInstances < std::max<size_t>(1, descs.size())) {
        nvrhi::rt::AccelStructHandle created;
        UT_CHECK(device->createAccelStruct(nvrhi::rt::AccelStructDesc()
                                               .setIsTopLevel(true)
                                               .setTopLevelMaxInstances(std::max<size_t>(1, descs.size()))
                                               .setBuildFlags(c_BuildFlags)
                                               .setDebugName("OptixRenderer TLAS"),
                                           &created));
        retire(tlas);
        tlas = std::move(created);
        if (!tlas) return;
    }
    commandList()->buildTopLevelAccelStruct(tlas, descs.data(), descs.size(), c_BuildFlags);
    traversablePending = true;
}

// ---------------------------------------------------------------------------
// OptixRenderer
// ---------------------------------------------------------------------------

OptixRenderer::OptixRenderer(gvdb::IComputeContext *context) : m_impl(std::make_unique<Impl>()) {
    NVRHI_ASSERT(context);
    m_impl->context = context;
    m_impl->device = context->getDevice();
    m_impl->seedBuffer = m_impl->createBuffer(c_NumSeeds * sizeof(uint32_t), "OptixRenderer seeds");
    m_impl->pipelineOk = m_impl->createPipeline();
    setEnvmap(nullptr, 0, 0);
    setSample(0, 0);
}

OptixRenderer::~OptixRenderer() {
    // The programs reach every buffer and the TLAS through addresses and handles.
    if (m_impl && m_impl->context) m_impl->context->finish();
}

bool OptixRenderer::isValid() const { return m_impl->pipelineOk; }

void OptixRenderer::resizeOutput(uint32_t width, uint32_t height) {
    Impl &im = *m_impl;
    if (width == 0 || height == 0) return;
    if (im.width == width && im.height == height && im.outputBuffer) return;
    im.width = width;
    im.height = height;
    im.retire(im.accumBuffer);
    im.retire(im.outputBuffer);
    im.accumBuffer = im.createBuffer(size_t(width) * height * sizeof(float) * 4, "OptixRenderer accumulation");
    im.outputBuffer = im.createBuffer(size_t(width) * height * 4, "OptixRenderer output");
    im.commandList()->clearBufferUInt(im.accumBuffer, 0);
    im.commandList()->clearBufferUInt(im.outputBuffer, 0xFF000000u);
}

nvrhi::IBuffer *OptixRenderer::getOutputBuffer() const { return m_impl->outputBuffer.Get(); }
uint32_t OptixRenderer::getWidth() const { return m_impl->width; }
uint32_t OptixRenderer::getHeight() const { return m_impl->height; }

void OptixRenderer::readOutput(std::vector<uint8_t> &rgba8) {
    Impl &im = *m_impl;
    const size_t bytes = size_t(im.width) * im.height * 4;
    rgba8.assign(bytes, 0);
    if (bytes == 0 || !im.outputBuffer) return;
    im.context->readBuffer(im.outputBuffer, 0, rgba8.data(), bytes);
}

int OptixRenderer::addMaterial(const OptixMaterialParams &params) {
    m_impl->materials.push_back(params);
    m_impl->materialsDirty = true;
    return int(m_impl->materials.size()) - 1;
}

void OptixRenderer::setMaterialParams(int id, const OptixMaterialParams &params) {
    if (id < 0 || id >= int(m_impl->materials.size())) return;
    m_impl->materials[id] = params;
    m_impl->materialsDirty = true;
}

int OptixRenderer::getNumMaterials() const { return int(m_impl->materials.size()); }

void OptixRenderer::setEnvmap(const uint8_t *rgba8, uint32_t width, uint32_t height) {
    Impl &im = *m_impl;
    if (!rgba8 || width == 0 || height == 0) {
        // the reference default: a white 64x64 map
        width = 64;
        height = 64;
        rgba8 = nullptr;
    }
    // float4 texels in a device buffer (the programs sample it bilinearly)
    std::vector<dm::float4> texels(size_t(width) * height, dm::float4(1.f));
    if (rgba8) {
        for (size_t i = 0; i < texels.size(); ++i)
            texels[i] = dm::float4(rgba8[i * 4 + 0], rgba8[i * 4 + 1], rgba8[i * 4 + 2], rgba8[i * 4 + 3]) / 255.f;
    }
    const size_t bytes = texels.size() * sizeof(dm::float4);
    im.envmap = im.grow(im.envmap, bytes, "OptixRenderer environment");
    im.commandList()->writeBuffer(im.envmap, texels.data(), bytes, 0);
    im.envWidth = width;
    im.envHeight = height;
}

VolumeViewParams &OptixRenderer::getViewParams() { return m_impl->viewParams; }
void OptixRenderer::setViewParams(const VolumeViewParams &params) { m_impl->viewParams = params; }

void OptixRenderer::buildScene(gvdb::GVDBSceneGraph *scene) {
    Impl &im = *m_impl;
    // (the previous scene's objects may still be in use by recorded launches)
    for (Impl::VolumeEntry &e : im.volumes) {
        im.retire(e.aabbBuffer);
        im.retire(e.blas);
    }
    for (Impl::MeshEntry &e : im.meshes) {
        for (nvrhi::IBuffer *b : {e.positions.Get(), e.normals.Get(), e.indices.Get()}) im.retire(b);
        im.retire(e.blas);
    }
    im.volumes.clear();
    im.meshes.clear();
    im.instances.clear();
    if (!scene || !im.pipelineOk) return;

    // volumes: one AABB primitive per instance
    for (const auto &inst : scene->GetVolumeInstances()) {
        if (!inst || !inst->GetVolume()) continue;
        Impl::VolumeEntry e;
        e.instance = inst;
        inst->GetVolume()->prepareVDB();
        im.writeVolumeAabb(e);
        e.blas = im.buildBlas(im.volumeGeometry(e), "OptixRenderer volume BLAS");

        Impl::InstanceEntry ie;
        ie.kind = Impl::InstanceKind::Volume;
        ie.index = int(im.volumes.size());
        im.volumes.push_back(std::move(e));
        im.instances.push_back(std::move(ie));
    }

    // meshes: one triangle BLAS per distinct MeshInfo
    std::map<const MeshInfo *, int> meshIndex;
    for (const auto &mi : scene->GetMeshInstances()) {
        if (!mi) continue;
        MeshInfo *mesh = mi->GetMesh();
        if (!mesh || !mesh->buffers || mesh->type != MeshType::Triangles) continue;
        auto it = meshIndex.find(mesh);
        if (it == meshIndex.end()) {
            Impl::MeshEntry e;
            e.mesh = mesh;
            if (!im.uploadMesh(e, true)) {
                log::warning("OptixRenderer: mesh '%s' has no usable CPU buffers, skipped", mesh->name.c_str());
                continue;
            }
            e.blas = im.buildBlas(im.meshGeometry(e), "OptixRenderer mesh BLAS");
            it = meshIndex.emplace(mesh, int(im.meshes.size())).first;
            im.meshes.push_back(std::move(e));
        }
        Impl::InstanceEntry ie;
        ie.kind = Impl::InstanceKind::Mesh;
        ie.index = it->second;
        ie.meshInstance = mi;
        im.instances.push_back(std::move(ie));
    }

    im.buildInstances();
}

void OptixRenderer::setMeshMaterial(int materialId) { m_impl->meshMaterial = materialId; }

void OptixRenderer::setMeshMaterial(const MeshInstance *instance, int materialId) {
    if (instance) m_impl->meshMaterialOverrides[instance] = materialId;
}

void OptixRenderer::updateVolumes(gvdb::GVDBSceneGraph *scene) {
    Impl &im = *m_impl;
    if (!im.pipelineOk) return;
    if (scene && scene->GetVolumeInstances().size() != im.volumes.size()) {
        buildScene(scene);
        return;
    }
    for (Impl::VolumeEntry &e : im.volumes) {
        e.instance->GetVolume()->prepareVDB();
        im.writeVolumeAabb(e);
        // A full build, not a refit: a volume that was empty (degenerate AABB) when the scene was built, or whose
        // bounds grew a lot, needs a new BVH. A one-primitive BLAS is cheap to build.
        const nvrhi::rt::GeometryDesc geometry = im.volumeGeometry(e);
        im.commandList()->buildBottomLevelAccelStruct(e.blas, &geometry, 1, c_BuildFlags);
    }
    // The TLAS caches the bounds of its children.
    if (!im.volumes.empty()) im.buildInstances();
}

void OptixRenderer::updateTransforms(gvdb::GVDBSceneGraph *scene) {
    Impl &im = *m_impl;
    if (!im.pipelineOk || !im.tlas) return;
    if (scene && (scene->GetVolumeInstances().size() != im.volumes.size() ||
                  scene->GetMeshInstances().size() + im.volumes.size() < im.instances.size())) {
        buildScene(scene);
        return;
    }
    im.buildInstances();
}

void OptixRenderer::updateMeshes(gvdb::GVDBSceneGraph *scene) {
    Impl &im = *m_impl;
    if (!im.pipelineOk) return;
    for (Impl::MeshEntry &e : im.meshes) {
        const uint32_t nv = e.numVertices, nt = e.numTriangles;
        if (!im.uploadMesh(e, false)) continue;
        const nvrhi::rt::GeometryDesc geometry = im.meshGeometry(e);
        if (nv != e.numVertices || nt != e.numTriangles) {
            im.retire(e.blas);   // the BLAS size changes
            e.blas = im.buildBlas(geometry, "OptixRenderer mesh BLAS");
        } else {
            im.commandList()->buildBottomLevelAccelStruct(e.blas, &geometry, 1, c_BuildFlags);
        }
    }
    if (im.tlas) im.buildInstances();   // the TLAS references the rebuilt BLAS
    (void)scene;
}

void OptixRenderer::setSample(int frame, int sample) {
    Impl &im = *m_impl;
    im.viewParams.frame = frame;
    im.viewParams.sample = sample;

    // the seed buffer of the reference: srand per (frame, sample)
    std::vector<uint32_t> seeds(c_NumSeeds);
    srand(unsigned(sample * 17 + 4732 + frame));
    for (uint32_t i = 0; i < c_NumSeeds; ++i) seeds[i] = uint32_t((long(rand()) * 0xffffL) / RAND_MAX);
    im.commandList()->writeBuffer(im.seedBuffer, seeds.data(), seeds.size() * sizeof(uint32_t), 0);
}

void OptixRenderer::render(gvdb::GVDBSceneGraph *scene, const RenderView &view, gvdb::VolumeShading overrideShading) {
    Impl &im = *m_impl;
    if (!im.pipelineOk || !im.outputBuffer || im.width == 0 || im.height == 0) return;
    if (!im.tlas) {
        if (scene) buildScene(scene);
        if (!im.tlas) return;
    }

    // per-instance values and intersection modes may change every frame (cheap: a few records)
    im.fillTables(overrideShading);
    if (im.materialsDirty) im.uploadMaterials();
    if (im.traversablePending) {
        // the handle of the TLAS build: known once the build is submitted
        im.context->flush();
        im.traversable = uint64_t(uintptr_t(im.tlas->getNativeObject(nvrhi::ObjectTypes::OptiX_TraversableHandle)));
        im.traversablePending = false;
    }

    // ---- launch parameters ----
    OptixLaunchParams p = {};
    p.scene = im.traversable;
    p.volumes = gvdb::deviceAddress(im.volumeTableBuffer);
    p.materials = gvdb::deviceAddress(im.materialBuffer);
    p.instances = gvdb::deviceAddress(im.instanceTableBuffer);
    p.accum = gvdb::deviceAddress(im.accumBuffer);
    p.output = gvdb::deviceAddress(im.outputBuffer);
    p.seeds = gvdb::deviceAddress(im.seedBuffer);
    p.envmap = gvdb::deviceAddress(im.envmap);
    p.backclr = im.viewParams.backgroundColor;
    p.eye = view.eye;
    p.numMaterials = uint32_t(std::max<size_t>(1, im.materials.size()));
    p.rayTL = view.rayTopLeft;
    p.width = im.width;
    p.rayU = view.rayU;
    p.height = im.height;
    p.rayV = view.rayV;
    p.sample = uint32_t(std::max(0, im.viewParams.sample));
    p.lightPos = view.lightPos;
    p.envWidth = im.envWidth;
    p.envHeight = im.envHeight;

    nvrhi::ICommandList *cmd = im.commandList();
    cmd->setRayTracingState(nvrhi::rt::State().setShaderTable(im.shaderTable));
    cmd->setPushConstants(&p, sizeof(p));
    cmd->dispatchRays(nvrhi::rt::DispatchRaysArguments().setWidth(im.width).setHeight(im.height));
}

}  // namespace SampleUtils
