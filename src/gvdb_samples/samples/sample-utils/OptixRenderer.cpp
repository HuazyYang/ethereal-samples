// OptiX renderer for a GVDBSceneGraph (OptixRenderer.h). Port of the reference
// OptixScene (optix_scene.cpp) onto the gp OptiX interfaces.
//
// Scene organisation
//   * one custom-primitive GAS (a single AABB = the voxel bounds in index
//     space) per volume instance, built with allowUpdate so updateVolumes()
//     can refit it;
//   * one triangle GAS per distinct MeshInfo, from the CPU buffers of the
//     mesh (positions, decoded normals, indices) uploaded to gp buffers;
//   * one IAS holding one instance per volume instance and per mesh instance,
//     with the node's local-to-world transform (index -> world for volumes),
//     rebuilt by updateTransforms().
//
// Materials and SBT
//   Materials live in a device table (params.materials); every hit group
//   record carries an OptixHitRecordData with the material id (and, for
//   volumes, the index into the per-instance table params.volumes; for
//   meshes, the vertex / index pointers). Hit group records are laid out per
//   instance in IAS order, GVDB_RT_RAY_TYPE_COUNT (2: radiance, shadow)
//   records each: the instance's sbtOffset is 2 * instanceIndex. The program
//   group of a volume's records is chosen by its intersection mode (surface /
//   deep / level set / empty skip), so a change of the shading override
//   repacks the SBT; changes of material parameters only re-upload the table.
#include "OptixRenderer.h"
#include "SampleTypes.h"
#include <sample-utils/kernels/optix/OptixLaunchParams.h>
#include <donut/core/log.h>
#include <donut/engine/SceneTypes.h>
#include <nvrhi/core/datablob.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

namespace SampleUtils {

using namespace donut;
using namespace donut::gp;
using namespace donut::engine;

namespace {

constexpr uint32_t c_NumRayTypes = GVDB_RT_RAY_TYPE_COUNT;
constexpr uint32_t c_SeedsDim = GVDB_RT_SEEDS_DIM;
constexpr uint32_t c_NumSeeds = c_SeedsDim * c_SeedsDim;

inline gvdb_lp::float3 lp3(const dm::float3 &v) {
    gvdb_lp::float3 r;
    r.x = v.x;
    r.y = v.y;
    r.z = v.z;
    return r;
}

inline gvdb_lp::float4 lp4(const dm::float4 &v) {
    gvdb_lp::float4 r;
    r.x = v.x;
    r.y = v.y;
    r.z = v.z;
    r.w = v.w;
    return r;
}

// Donut affine (row vectors, p' = p * M) -> OptiX row-major 3x4 (column vectors).
void affineToOptix(const dm::affine3 &m, float out[12]) {
    const float t[3] = {m.m_translation.x, m.m_translation.y, m.m_translation.z};
    for (int j = 0; j < 3; ++j) {
        for (int i = 0; i < 3; ++i) out[j * 4 + i] = m.m_linear.m_data[i * 3 + j];
        out[j * 4 + 3] = t[j];
    }
}

void identityToOptix(float out[12]) {
    const float id[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    memcpy(out, id, sizeof(id));
}

void setIdentity(float m[16]) {
    memset(m, 0, sizeof(float) * 16);
    m[0] = m[5] = m[10] = m[15] = 1.f;
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
    m.ambColor = lp3(p.ambColor);
    m.lightWidth = p.lightWidth;
    m.envColor = lp3(p.envColor);
    m.specPower = p.specPower;
    m.diffColor = lp3(p.diffColor);
    m.shadowWidth = p.shadowWidth;
    m.specColor = lp3(p.specColor);
    m.shadowBias = p.shadowBias;
    m.reflColor = lp3(p.reflColor);
    m.reflWidth = p.reflWidth;
    m.refrColor = lp3(p.refrColor);
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
    // ---- devices ----
    nvrhi::AutoPtr<IDevice> device;
    nvrhi::AutoPtr<IRTDevice> rtDevice;
    nvrhi::AutoPtr<IDeviceQueue> queue;
    nvrhi::AutoPtr<vfs::IFileSystem> vfs;

    // ---- pipeline ----
    RTPipelineCompileOptions compileOptions;
    nvrhi::AutoPtr<IRTModule> module;
    nvrhi::AutoPtr<IRTProgramGroup> pgRaygen;
    nvrhi::AutoPtr<IRTProgramGroup> pgException;
    nvrhi::AutoPtr<IRTProgramGroup> pgMissRadiance;
    nvrhi::AutoPtr<IRTProgramGroup> pgMissShadow;
    nvrhi::AutoPtr<IRTProgramGroup> pgVolumeRadiance[GVDB_RT_ISECT_COUNT];
    nvrhi::AutoPtr<IRTProgramGroup> pgVolumeShadow[GVDB_RT_ISECT_COUNT];
    nvrhi::AutoPtr<IRTProgramGroup> pgMeshRadiance;
    nvrhi::AutoPtr<IRTProgramGroup> pgMeshShadow;
    nvrhi::AutoPtr<IRTPipeline> pipeline;
    bool pipelineOk = false;

    // ---- output ----
    uint32_t width = 0, height = 0;
    nvrhi::AutoPtr<IBuffer> accumBuffer;    // float4
    nvrhi::AutoPtr<IBuffer> outputBuffer;   // RGBA8

    // ---- scene-wide ----
    VolumeViewParams viewParams;
    std::vector<OptixMaterialParams> materials;
    nvrhi::AutoPtr<IBuffer> materialBuffer;
    bool materialsDirty = true;
    nvrhi::AutoPtr<IBuffer> envmap;   // float4 texels (see OptixLaunchParams::envmap)
    uint32_t envWidth = 0, envHeight = 0;
    nvrhi::AutoPtr<IBuffer> seedBuffer;
    nvrhi::AutoPtr<IBuffer> defaultTransfer;
    nvrhi::AutoPtr<IBuffer> paramsBuffer;
    int meshMaterial = 0;
    std::map<const MeshInstance *, int> meshMaterialOverrides;

    // ---- scene ----
    struct VolumeEntry {
        nvrhi::AutoPtr<gvdb::GVDBVolumeInstance> instance;
        nvrhi::AutoPtr<IBuffer> aabbBuffer;
        nvrhi::AutoPtr<IRTAccelStruct> gas;
        int isect = GVDB_RT_ISECT_SURFACE;   // intersection mode of the SBT records
        int materialId = 0;                  // material of the SBT records
    };
    struct MeshEntry {
        nvrhi::AutoPtr<MeshInfo> mesh;
        nvrhi::AutoPtr<IBuffer> positions;
        nvrhi::AutoPtr<IBuffer> normals;
        nvrhi::AutoPtr<IBuffer> indices;
        uint32_t numVertices = 0;
        uint32_t numTriangles = 0;
        nvrhi::AutoPtr<IRTAccelStruct> gas;
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
    nvrhi::AutoPtr<IBuffer> volumeTableBuffer;
    nvrhi::AutoPtr<IRTAccelStruct> ias;
    nvrhi::AutoPtr<IRTShaderBindingTable> sbt;
    gvdb::VolumeShading builtOverride = gvdb::VolumeShading::Off;
    bool sbtDirty = false;   // mesh materials changed

    // ---- helpers ----
    nvrhi::AutoPtr<IBuffer> createBuffer(size_t bytes) {
        nvrhi::AutoPtr<IBuffer> buffer;
        BufferDesc desc;
        desc.byteSize = std::max<size_t>(bytes, 1);
        UT_V_GP(device->createBuffer(desc, &buffer));
        return buffer;
    }

    nvrhi::AutoPtr<IRTProgramGroup> createProgramGroup(const RTProgramGroupDesc &desc) {
        nvrhi::AutoPtr<IRTProgramGroup> group;
        UT_V_GP(rtDevice->createProgramGroup(desc, &group));
        return group;
    }

    RTProgramEntry entry(const char *name) { return RTProgramEntry{module.Get(), name}; }

    bool createPipeline();
    void createDefaults();
    void uploadMaterials();
    void uploadVolumeTable();
    int effectiveIsect(const VolumeEntry &e, gvdb::VolumeShading overrideShading) const;
    void writeVolumeAabb(VolumeEntry &e);
    bool uploadMesh(MeshEntry &e, bool recreate);
    RTAccelStructDesc meshGasDesc(const MeshEntry &e, RTTrianglesInput &input) const;
    void buildInstances(bool rebuildOnly);
    void buildSBT(gvdb::VolumeShading overrideShading);
    int meshMaterialOf(const MeshInstance *instance) const;
    void fillVolumeTable(gvdb::VolumeShading overrideShading);
};

// ---------------------------------------------------------------------------
// Pipeline
// ---------------------------------------------------------------------------

bool OptixRenderer::Impl::createPipeline() {
    // PTX of the programs
    nvrhi::AutoPtr<nvrhi::IDataBlob> blob;
    if (NVRHI_FAILED(vfs->readFile("ptx/OptixPrograms.ptx", &blob)) || !blob) {
        log::error("OptixRenderer: cannot read ptx/OptixPrograms.ptx");
        return false;
    }
    const size_t len = blob->GetSize();
    blob->Resize(len + 1);
    static_cast<char *>(blob->GetDataPtr())[len] = 0;

    compileOptions = RTPipelineCompileOptions();
    compileOptions.usesMotionBlur = false;
    compileOptions.traversableGraphFlags = RT_TRAVERSABLE_GRAPH_SINGLE_LEVEL_INSTANCING;
    compileOptions.numPayloadValues = GVDB_RT_NUM_PAYLOAD_VALUES;
    compileOptions.numAttributeValues = GVDB_RT_NUM_ATTRIBUTES;
    compileOptions.exceptionFlags = RT_EXCEPTION_NONE;
    compileOptions.launchParamsVariableName = "params";
    compileOptions.primitiveTypeFlags = RT_PRIMITIVE_TYPE_CUSTOM | RT_PRIMITIVE_TYPE_TRIANGLE;

    RTModuleDesc moduleDesc;
    moduleDesc.pipelineOptions = compileOptions;
    moduleDesc.optLevel = RTCompileOptLevel::Default;
    moduleDesc.debugLevel = RTCompileDebugLevel::Default;
    if (NVRHI_FAILED(rtDevice->createModule(moduleDesc, blob->GetDataPtr(), len + 1, &module))) {
        log::error("OptixRenderer: OptiX module creation failed");
        return false;
    }

    // program groups
    {
        RTProgramGroupDesc d;
        d.kind = RTProgramGroupKind::RayGen;
        d.raygen = entry("__raygen__main");
        pgRaygen = createProgramGroup(d);
    }
    {
        RTProgramGroupDesc d;
        d.kind = RTProgramGroupKind::Exception;
        d.exception = entry("__exception__main");
        pgException = createProgramGroup(d);
    }
    {
        RTProgramGroupDesc d;
        d.kind = RTProgramGroupKind::Miss;
        d.miss = entry("__miss__radiance");
        pgMissRadiance = createProgramGroup(d);
        d.miss = entry("__miss__shadow");
        pgMissShadow = createProgramGroup(d);
    }
    const char *isectNames[GVDB_RT_ISECT_COUNT] = {};
    isectNames[GVDB_RT_ISECT_SURFACE] = "__intersection__volume_surface";
    isectNames[GVDB_RT_ISECT_DEEP] = "__intersection__volume_deep";
    isectNames[GVDB_RT_ISECT_LEVELSET] = "__intersection__volume_levelset";
    isectNames[GVDB_RT_ISECT_EMPTYSKIP] = "__intersection__volume_emptyskip";
    for (int i = 0; i < GVDB_RT_ISECT_COUNT; ++i) {
        RTProgramGroupDesc d;
        d.kind = RTProgramGroupKind::HitGroup;
        d.hitgroup.intersection = entry(isectNames[i]);
        d.hitgroup.closestHit = entry(i == GVDB_RT_ISECT_DEEP ? "__closesthit__deep" : "__closesthit__surface");
        pgVolumeRadiance[i] = createProgramGroup(d);

        RTProgramGroupDesc s;
        s.kind = RTProgramGroupKind::HitGroup;
        s.hitgroup.intersection = entry(isectNames[i]);
        s.hitgroup.anyHit = entry("__anyhit__shadow");
        pgVolumeShadow[i] = createProgramGroup(s);
    }
    {
        RTProgramGroupDesc d;
        d.kind = RTProgramGroupKind::HitGroup;
        d.hitgroup.closestHit = entry("__closesthit__mesh");
        pgMeshRadiance = createProgramGroup(d);

        RTProgramGroupDesc s;
        s.kind = RTProgramGroupKind::HitGroup;
        s.hitgroup.anyHit = entry("__anyhit__shadow");
        pgMeshShadow = createProgramGroup(s);
    }

    std::vector<IRTProgramGroup *> groups = {pgRaygen.Get(), pgException.Get(), pgMissRadiance.Get(),
                                             pgMissShadow.Get(), pgMeshRadiance.Get(), pgMeshShadow.Get()};
    for (int i = 0; i < GVDB_RT_ISECT_COUNT; ++i) {
        groups.push_back(pgVolumeRadiance[i].Get());
        groups.push_back(pgVolumeShadow[i].Get());
    }
    for (IRTProgramGroup *g : groups) {
        if (!g) {
            log::error("OptixRenderer: OptiX program group creation failed");
            return false;
        }
    }

    RTPipelineDesc pipelineDesc;
    pipelineDesc.compileOptions = compileOptions;
    // primary -> deep (mesh ray) -> surface (reflection) -> deep (mesh ray) ->
    // surface (refraction) -> deep (mesh ray) -> surface (shadow)
    pipelineDesc.maxTraceDepth = 8;
    pipelineDesc.maxTraversableGraphDepth = 2;   // IAS -> GAS
    pipelineDesc.programGroups = groups.data();
    pipelineDesc.numProgramGroups = (uint32_t)groups.size();
    if (NVRHI_FAILED(rtDevice->createPipeline(pipelineDesc, &pipeline))) {
        log::error("OptixRenderer: OptiX pipeline creation failed");
        return false;
    }
    return true;
}

void OptixRenderer::Impl::createDefaults() {
    // random seeds
    seedBuffer = createBuffer(c_NumSeeds * sizeof(uint32_t));
    // launch parameters
    paramsBuffer = createBuffer(sizeof(OptixLaunchParams));
    // default transfer function: white, opacity ramp 0..1 over the value range
    {
        std::vector<dm::float4> tf(gvdb::TRANSFER_FUNC_SIZE);
        for (int i = 0; i < gvdb::TRANSFER_FUNC_SIZE; ++i)
            tf[i] = dm::float4(1.f, 1.f, 1.f, float(i) / float(gvdb::TRANSFER_FUNC_SIZE - 1));
        defaultTransfer = createBuffer(tf.size() * sizeof(dm::float4));
        UT_V_GP(queue->writeBuffer(defaultTransfer, tf.data(), tf.size() * sizeof(dm::float4), 0));
    }
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
    if (!materialBuffer || materialBuffer->getDesc()->byteSize < bytes) materialBuffer = createBuffer(bytes);
    UT_V_GP(queue->writeBuffer(materialBuffer, table.data(), bytes, 0));
    materialsDirty = false;
}

int OptixRenderer::Impl::effectiveIsect(const VolumeEntry &e, gvdb::VolumeShading overrideShading) const {
    const gvdb::VolumeShading shading =
        overrideShading != gvdb::VolumeShading::Off ? overrideShading : e.instance->GetRenderAttributes().shading;
    return shadingToIsect(shading);
}

void OptixRenderer::Impl::fillVolumeTable(gvdb::VolumeShading overrideShading) {
    volumeTable.resize(volumes.size());
    for (size_t i = 0; i < volumes.size(); ++i) {
        VolumeEntry &e = volumes[i];
        const gvdb::VolumeRenderAttributes &a = e.instance->GetRenderAttributes();
        gvdb::IGVDBVolume *vol = e.instance->GetVolume();
        OptixVolumeInstance &v = volumeTable[i];
        memset(&v, 0, sizeof(v));
        IBuffer *info = vol->getVDBInfoGPU();
        v.gvdb = info ? (unsigned long long)info->getNativeHandle() : 0;
        IBuffer *tf = a.transferFunction ? a.transferFunction->getGPU() : nullptr;
        v.transfer = (unsigned long long)((tf ? tf : defaultTransfer.Get())->getNativeHandle());
        v.thresh = lp3(a.threshold);
        v.channel = a.channel;
        v.steps = lp3(a.steps);
        v.colorChannel = a.colorChannel;
        v.extinct = lp3(a.extinct);
        v.shading = int(overrideShading != gvdb::VolumeShading::Off ? overrideShading : a.shading);
        v.cutoff = lp3(a.cutoff);
        e.isect = effectiveIsect(e, overrideShading);
        v.isect = e.isect;
        v.epsilon = a.epsilon;
        v.materialId = a.materialId;
        const dm::affine3 indexToWorld = e.instance->GetIndexToWorld();
        affineToOptix(indexToWorld, v.indexToWorld);
        affineToOptix(dm::inverse(indexToWorld), v.worldToIndex);
    }
}

void OptixRenderer::Impl::uploadVolumeTable() {
    const size_t bytes = std::max<size_t>(1, volumeTable.size()) * sizeof(OptixVolumeInstance);
    if (!volumeTableBuffer || volumeTableBuffer->getDesc()->byteSize < bytes) volumeTableBuffer = createBuffer(bytes);
    if (!volumeTable.empty())
        UT_V_GP(queue->writeBuffer(volumeTableBuffer, volumeTable.data(), volumeTable.size() * sizeof(OptixVolumeInstance), 0));
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

void OptixRenderer::Impl::writeVolumeAabb(VolumeEntry &e) {
    const dm::box3 bounds = e.instance->GetLocalBoundingBox();
    float aabb[6] = {bounds.m_mins.x, bounds.m_mins.y, bounds.m_mins.z,
                     bounds.m_maxs.x, bounds.m_maxs.y, bounds.m_maxs.z};
    // an empty volume gets a degenerate but valid box
    for (int i = 0; i < 3; ++i)
        if (!(aabb[i + 3] > aabb[i])) aabb[i + 3] = aabb[i] + 1e-3f;
    if (!e.aabbBuffer) e.aabbBuffer = createBuffer(sizeof(aabb));
    UT_V_GP(queue->writeBuffer(e.aabbBuffer, aabb, sizeof(aabb), 0));
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
        e.positions = createBuffer(size_t(nv) * sizeof(dm::float3));
        e.normals = nullptr;
        e.indices = createBuffer(size_t(numTriangles) * 3 * sizeof(uint32_t));
        e.numVertices = nv;
        e.numTriangles = numTriangles;
    }

    UT_V_GP(queue->writeBuffer(e.positions, b.positionData.data() + v0, size_t(nv) * sizeof(dm::float3), 0));
    UT_V_GP(queue->writeBuffer(e.indices, b.indexData.data() + i0, size_t(numTriangles) * 3 * sizeof(uint32_t), 0));
    if (b.normalData.size() >= size_t(v0) + nv) {
        std::vector<dm::float3> normals(nv);
        for (uint32_t i = 0; i < nv; ++i) normals[i] = decodeSnorm8(b.normalData[v0 + i]);
        if (!e.normals) e.normals = createBuffer(size_t(nv) * sizeof(dm::float3));
        UT_V_GP(queue->writeBuffer(e.normals, normals.data(), normals.size() * sizeof(dm::float3), 0));
    } else {
        e.normals = nullptr;
    }
    return true;
}

RTAccelStructDesc OptixRenderer::Impl::meshGasDesc(const MeshEntry &e, RTTrianglesInput &input) const {
    input = RTTrianglesInput();
    input.vertexBuffer = e.positions.Get();
    input.numVertices = e.numVertices;
    input.vertexStride = sizeof(float) * 3;
    input.indexBuffer = e.indices.Get();
    input.numTriangles = e.numTriangles;
    input.indexFormat = RTIndexFormat::UInt32x3;
    input.geometryFlags = RT_GEOMETRY_FLAG_NONE;
    input.numSbtRecords = 1;
    RTAccelStructDesc desc;
    desc.type = RTGeometryType::Triangles;
    desc.triangles = &input;
    desc.numTriangleInputs = 1;
    desc.allowUpdate = false;
    desc.allowCompaction = true;
    desc.preferFastTrace = true;
    return desc;
}

int OptixRenderer::Impl::meshMaterialOf(const MeshInstance *instance) const {
    auto it = meshMaterialOverrides.find(instance);
    return it != meshMaterialOverrides.end() ? it->second : meshMaterial;
}

void OptixRenderer::Impl::buildInstances(bool rebuildOnly) {
    std::vector<RTInstance> rtInstances(instances.size());
    for (size_t i = 0; i < instances.size(); ++i) {
        const InstanceEntry &inst = instances[i];
        RTInstance &r = rtInstances[i];
        r.instanceId = (uint32_t)i;
        r.sbtOffset = (uint32_t)i * c_NumRayTypes;
        r.visibilityMask = 255;
        if (inst.kind == InstanceKind::Volume) {
            const VolumeEntry &e = volumes[inst.index];
            affineToOptix(e.instance->GetIndexToWorld(), r.transform);
            r.flags = RT_INSTANCE_FLAG_NONE;
            r.accel = e.gas.Get();
        } else {
            const MeshEntry &e = meshes[inst.index];
            SceneGraphNode *node = inst.meshInstance->GetNode();
            if (node)
                affineToOptix(node->GetLocalToWorldTransformFloat(), r.transform);
            else
                identityToOptix(r.transform);
            r.flags = RT_INSTANCE_FLAG_DISABLE_TRIANGLE_FACE_CULLING;
            r.accel = e.gas.Get();
        }
    }

    RTAccelStructDesc desc;
    desc.type = RTGeometryType::Instances;
    desc.instances = rtInstances.empty() ? nullptr : rtInstances.data();
    desc.numInstances = (uint32_t)rtInstances.size();
    desc.allowUpdate = false;
    desc.allowCompaction = false;
    desc.preferFastTrace = true;
    if (rebuildOnly && ias) {
        UT_V_GP(ias->rebuild(queue, desc, false));
    } else {
        ias = nullptr;
        UT_V_GP(rtDevice->createAccelStruct(queue, desc, &ias));
    }
}

void OptixRenderer::Impl::buildSBT(gvdb::VolumeShading overrideShading) {
    const size_t numRecords = instances.size() * c_NumRayTypes;
    std::vector<OptixHitRecordData> data(numRecords);
    std::vector<RTSbtRecordDesc> hitgroups(numRecords);
    for (size_t i = 0; i < instances.size(); ++i) {
        const InstanceEntry &inst = instances[i];
        OptixHitRecordData rec = {};
        IRTProgramGroup *radiance = nullptr;
        IRTProgramGroup *shadow = nullptr;
        if (inst.kind == InstanceKind::Volume) {
            VolumeEntry &e = volumes[inst.index];
            e.isect = effectiveIsect(e, overrideShading);
            e.materialId = e.instance->GetRenderAttributes().materialId;
            rec.kind = GVDB_RT_HIT_VOLUME;
            rec.volumeIndex = (uint32_t)inst.index;
            rec.materialId = e.materialId;
            radiance = pgVolumeRadiance[e.isect].Get();
            shadow = pgVolumeShadow[e.isect].Get();
        } else {
            const MeshEntry &e = meshes[inst.index];
            rec.kind = GVDB_RT_HIT_MESH;
            rec.volumeIndex = 0;
            rec.materialId = meshMaterialOf(inst.meshInstance.Get());
            rec.numTriangles = e.numTriangles;
            rec.positions = (unsigned long long)e.positions->getNativeHandle();
            rec.normals = e.normals ? (unsigned long long)e.normals->getNativeHandle() : 0;
            rec.indices = (unsigned long long)e.indices->getNativeHandle();
            radiance = pgMeshRadiance.Get();
            shadow = pgMeshShadow.Get();
        }
        for (uint32_t r = 0; r < c_NumRayTypes; ++r) {
            const size_t k = i * c_NumRayTypes + r;
            data[k] = rec;
            hitgroups[k].programGroup = (r == GVDB_RT_RAY_SHADOW) ? shadow : radiance;
            hitgroups[k].data = &data[k];
            hitgroups[k].dataSize = sizeof(OptixHitRecordData);
        }
    }

    RTSbtRecordDesc miss[c_NumRayTypes] = {};
    miss[GVDB_RT_RAY_RADIANCE].programGroup = pgMissRadiance.Get();
    miss[GVDB_RT_RAY_SHADOW].programGroup = pgMissShadow.Get();

    RTShaderBindingTableDesc desc;
    desc.raygen.programGroup = pgRaygen.Get();
    desc.exception.programGroup = pgException.Get();
    desc.miss = miss;
    desc.numMiss = c_NumRayTypes;
    desc.hitgroups = hitgroups.empty() ? nullptr : hitgroups.data();
    desc.numHitgroups = (uint32_t)hitgroups.size();
    sbt = nullptr;
    UT_V_GP(rtDevice->createShaderBindingTable(queue, desc, &sbt));
    builtOverride = overrideShading;
    sbtDirty = false;
}

// ---------------------------------------------------------------------------
// OptixRenderer
// ---------------------------------------------------------------------------

OptixRenderer::OptixRenderer(IDevice *cudaDevice, IRTDevice *rtDevice, IDeviceQueue *queue, vfs::IFileSystem *vfs)
    : m_impl(std::make_unique<Impl>()) {
    m_impl->device = cudaDevice;
    m_impl->rtDevice = rtDevice;
    m_impl->queue = queue;
    m_impl->vfs = vfs;
    NVRHI_ASSERT(cudaDevice && rtDevice && queue && vfs);

    m_impl->createDefaults();
    m_impl->pipelineOk = m_impl->createPipeline();
    setEnvmap(nullptr, 0, 0);
    setSample(0, 0);
}

OptixRenderer::~OptixRenderer() {
    if (m_impl && m_impl->device && m_impl->queue) m_impl->device->waitForQueue(m_impl->queue);
}

void OptixRenderer::resizeOutput(uint32_t width, uint32_t height) {
    Impl &im = *m_impl;
    if (width == 0 || height == 0) return;
    if (im.width == width && im.height == height && im.outputBuffer) return;
    im.width = width;
    im.height = height;
    im.accumBuffer = im.createBuffer(size_t(width) * height * sizeof(float) * 4);
    im.outputBuffer = im.createBuffer(size_t(width) * height * 4);
    UT_V_GP(im.queue->clearBufferUint(im.accumBuffer, 0));
    UT_V_GP(im.queue->clearBufferUint(im.outputBuffer, 0xFF000000u));
}

IBuffer *OptixRenderer::getOutputBuffer() const { return m_impl->outputBuffer.Get(); }
uint32_t OptixRenderer::getWidth() const { return m_impl->width; }
uint32_t OptixRenderer::getHeight() const { return m_impl->height; }

void OptixRenderer::readOutput(std::vector<uint8_t> &rgba8) {
    Impl &im = *m_impl;
    const size_t bytes = size_t(im.width) * im.height * 4;
    rgba8.assign(bytes, 0);
    if (bytes == 0 || !im.outputBuffer) return;

    nvrhi::AutoPtr<IBuffer> staging;
    BufferDesc desc;
    desc.byteSize = bytes;
    desc.isStaging = true;
    UT_V_GP(im.device->createBuffer(desc, &staging));
    UT_V_GP(im.queue->copyBufferRegion(staging, 0, im.outputBuffer, 0, bytes));
    UT_V_GP(im.device->waitForQueue(im.queue));
    void *mapped = nullptr;
    UT_V_GP(im.device->mapBuffer(staging, &mapped));
    if (mapped) memcpy(rgba8.data(), mapped, bytes);
    im.device->unmapBuffer(staging);
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
    if (!im.envmap || im.envmap->getDesc()->byteSize < bytes) im.envmap = im.createBuffer(bytes);
    UT_V_GP(im.queue->writeBuffer(im.envmap, texels.data(), bytes, 0));
    im.envWidth = width;
    im.envHeight = height;
}

VolumeViewParams &OptixRenderer::getViewParams() { return m_impl->viewParams; }
void OptixRenderer::setViewParams(const VolumeViewParams &params) { m_impl->viewParams = params; }

void OptixRenderer::buildScene(gvdb::GVDBSceneGraph *scene) {
    Impl &im = *m_impl;
    im.volumes.clear();
    im.meshes.clear();
    im.instances.clear();
    im.ias = nullptr;
    im.sbt = nullptr;
    if (!scene || !im.pipelineOk) return;

    // volumes: one AABB primitive per instance
    for (const auto &inst : scene->GetVolumeInstances()) {
        if (!inst || !inst->GetVolume()) continue;
        Impl::VolumeEntry e;
        e.instance = inst;
        inst->GetVolume()->prepareVDB();
        im.writeVolumeAabb(e);

        RTAabbsInput input;
        input.aabbBuffer = e.aabbBuffer.Get();
        input.numPrimitives = 1;
        input.stride = 0;
        input.geometryFlags = RT_GEOMETRY_FLAG_NONE;
        input.numSbtRecords = 1;
        RTAccelStructDesc desc;
        desc.type = RTGeometryType::CustomPrimitives;
        desc.aabbs = &input;
        desc.numAabbInputs = 1;
        desc.allowUpdate = true;   // refit in updateVolumes()
        desc.allowCompaction = false;
        desc.preferFastTrace = true;
        UT_V_GP(im.rtDevice->createAccelStruct(im.queue, desc, &e.gas));

        Impl::InstanceEntry ie;
        ie.kind = Impl::InstanceKind::Volume;
        ie.index = int(im.volumes.size());
        im.volumes.push_back(std::move(e));
        im.instances.push_back(std::move(ie));
    }

    // meshes: one triangle GAS per distinct MeshInfo
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
            RTTrianglesInput input;
            const RTAccelStructDesc desc = im.meshGasDesc(e, input);
            UT_V_GP(im.rtDevice->createAccelStruct(im.queue, desc, &e.gas));
            it = meshIndex.emplace(mesh, int(im.meshes.size())).first;
            im.meshes.push_back(std::move(e));
        }
        Impl::InstanceEntry ie;
        ie.kind = Impl::InstanceKind::Mesh;
        ie.index = it->second;
        ie.meshInstance = mi;
        im.instances.push_back(std::move(ie));
    }

    im.buildInstances(false);
    im.fillVolumeTable(im.builtOverride);
    im.uploadVolumeTable();
    im.buildSBT(im.builtOverride);
}

void OptixRenderer::setMeshMaterial(int materialId) {
    m_impl->meshMaterial = materialId;
    m_impl->sbtDirty = true;
}

void OptixRenderer::setMeshMaterial(const MeshInstance *instance, int materialId) {
    if (!instance) return;
    m_impl->meshMaterialOverrides[instance] = materialId;
    m_impl->sbtDirty = true;
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
        RTAabbsInput input;
        input.aabbBuffer = e.aabbBuffer.Get();
        input.numPrimitives = 1;
        input.numSbtRecords = 1;
        RTAccelStructDesc desc;
        desc.type = RTGeometryType::CustomPrimitives;
        desc.aabbs = &input;
        desc.numAabbInputs = 1;
        desc.allowUpdate = true;
        desc.allowCompaction = false;
        // Full rebuild, not a refit: a volume that was empty (degenerate AABB)
        // when the scene was built, or whose bounds grew a lot, must get a new
        // BVH. A one-primitive GAS is cheap to build.
        UT_V_GP(e.gas->rebuild(im.queue, desc, false));
    }
    // The IAS caches the bounds (and handles) of its children.
    if (!im.volumes.empty()) im.buildInstances(true);
    im.fillVolumeTable(im.builtOverride);
    im.uploadVolumeTable();
}

void OptixRenderer::updateTransforms(gvdb::GVDBSceneGraph *scene) {
    Impl &im = *m_impl;
    if (!im.pipelineOk || !im.ias) return;
    if (scene && (scene->GetVolumeInstances().size() != im.volumes.size() ||
                  scene->GetMeshInstances().size() + im.volumes.size() < im.instances.size())) {
        buildScene(scene);
        return;
    }
    im.buildInstances(true);
    im.fillVolumeTable(im.builtOverride);
    im.uploadVolumeTable();
}

void OptixRenderer::updateMeshes(gvdb::GVDBSceneGraph *scene) {
    Impl &im = *m_impl;
    if (!im.pipelineOk) return;
    bool topologyChanged = false;
    for (Impl::MeshEntry &e : im.meshes) {
        const uint32_t nv = e.numVertices, nt = e.numTriangles;
        if (!im.uploadMesh(e, false)) continue;
        RTTrianglesInput input;
        const RTAccelStructDesc desc = im.meshGasDesc(e, input);
        if (nv != e.numVertices || nt != e.numTriangles) topologyChanged = true;
        UT_V_GP(e.gas->rebuild(im.queue, desc, false));
    }
    if (topologyChanged) {
        // buffer addresses changed: hit records and the IAS refer to them
        im.buildInstances(true);
        im.buildSBT(im.builtOverride);
    } else if (im.ias) {
        im.buildInstances(true);   // the IAS references the rebuilt GAS handles
    }
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
    UT_V_GP(im.queue->writeBuffer(im.seedBuffer, seeds.data(), seeds.size() * sizeof(uint32_t), 0));
}

void OptixRenderer::render(gvdb::GVDBSceneGraph *scene, const RenderView &view, gvdb::VolumeShading overrideShading) {
    Impl &im = *m_impl;
    if (!im.pipelineOk || !im.outputBuffer || im.width == 0 || im.height == 0) return;
    if (!im.ias || !im.sbt) {
        if (scene) buildScene(scene);
        if (!im.ias || !im.sbt) return;
    }

    // The hit records bind the intersection program (by shading) and the
    // material: repack the SBT when the override, an instance's shading or a
    // material assignment changed.
    bool repack = im.sbtDirty || overrideShading != im.builtOverride;
    for (const Impl::VolumeEntry &e : im.volumes) {
        if (im.effectiveIsect(e, overrideShading) != e.isect ||
            e.instance->GetRenderAttributes().materialId != e.materialId)
            repack = true;
    }
    if (repack) im.buildSBT(overrideShading);
    // per-instance values may change every frame (cheap: a few records)
    im.fillVolumeTable(overrideShading);
    im.uploadVolumeTable();
    if (im.materialsDirty) im.uploadMaterials();

    // ---- launch parameters ----
    OptixLaunchParams p;
    memset(&p, 0, sizeof(p));
    const VolumeViewParams &vp = im.viewParams;

    // ScnInfo as the CUDA VolRenderer fills it (PrepareRender of the reference):
    // scene-wide values; the per-volume values default to the first instance
    // (the programs read them from the instance table).
    ScnInfo &s = p.scn;
    s.width = int(im.width);
    s.height = int(im.height);
    s.camnear = view.zNear;
    s.camfar = view.zFar;
    s.campos = lp3(view.eye);
    s.cams = lp3(view.rayTopLeft);
    s.camu = lp3(view.rayU);
    s.camv = lp3(view.rayV);
    s.light_pos = lp3(view.lightPos);
    s.slice_pnt = lp3(vp.sectionPoint);
    s.slice_norm = lp3(vp.sectionNormal);
    s.shadow_params = lp3(vp.shadowParams);
    s.backclr = lp4(vp.backgroundColor);
    setIdentity(s.xform);
    setIdentity(s.invxform);
    setIdentity(s.invxrot);
    s.bias = vp.rayNormalBias;
    s.filtering = char(vp.filterMode);
    s.frame = vp.frame;
    s.samples = vp.sample;
    if (!im.volumes.empty()) {
        const gvdb::VolumeRenderAttributes &a = im.volumes[0].instance->GetRenderAttributes();
        s.shading = char(overrideShading != gvdb::VolumeShading::Off ? overrideShading : a.shading);
        s.extinct = lp3(a.extinct);
        s.steps = lp3(a.steps);
        s.cutoff = lp3(a.cutoff);
        s.thresh = lp3(a.threshold);
        s.epsilon = a.epsilon;
        s.gvdb_channel = a.colorChannel;
        s.transfer = reinterpret_cast<gvdb_lp::float4 *>(im.volumeTable[0].transfer);
    } else {
        const gvdb::VolumeRenderAttributes a;
        s.shading = char(a.shading);
        s.extinct = lp3(a.extinct);
        s.steps = lp3(a.steps);
        s.cutoff = lp3(a.cutoff);
        s.thresh = lp3(a.threshold);
        s.epsilon = a.epsilon;
        s.gvdb_channel = a.colorChannel;
        s.transfer = reinterpret_cast<gvdb_lp::float4 *>(im.defaultTransfer->getNativeHandle());
    }
    s.outbuf = nullptr;
    s.dbuf = nullptr;

    p.volumes = reinterpret_cast<const OptixVolumeInstance *>(im.volumeTableBuffer->getNativeHandle());
    p.numVolumes = (unsigned int)im.volumeTable.size();
    p.materials = reinterpret_cast<const OptixMaterial *>(im.materialBuffer->getNativeHandle());
    p.numMaterials = (unsigned int)std::max<size_t>(1, im.materials.size());
    p.eye = lp3(view.eye);
    p.rayTL = lp3(view.rayTopLeft);
    p.rayU = lp3(view.rayU);
    p.rayV = lp3(view.rayV);
    p.lightPos = lp3(view.lightPos);
    p.accum = reinterpret_cast<gvdb_lp::float4 *>(im.accumBuffer->getNativeHandle());
    p.output = reinterpret_cast<gvdb_lp::uchar4 *>(im.outputBuffer->getNativeHandle());
    p.width = im.width;
    p.height = im.height;
    p.seeds = reinterpret_cast<unsigned int *>(im.seedBuffer->getNativeHandle());
    p.frame = (unsigned int)std::max(0, vp.frame);
    p.sample = (unsigned int)std::max(0, vp.sample);
    p.envmap = reinterpret_cast<const gvdb_lp::float4 *>(im.envmap->getNativeHandle());
    p.envWidth = im.envWidth;
    p.envHeight = im.envHeight;
    p.traversable = im.ias->getTraversableHandle();
    p.sceneEpsilon = 1.0e-6f;

    UT_V_GP(im.queue->writeBuffer(im.paramsBuffer, &p, sizeof(p), 0));
    UT_V_GP(im.rtDevice->launch(im.queue, im.pipeline, im.sbt, im.paramsBuffer, 0, sizeof(p), im.width, im.height, 1));
}

IDeviceQueue *OptixRenderer::getQueue() const { return m_impl->queue.Get(); }

}  // namespace SampleUtils
