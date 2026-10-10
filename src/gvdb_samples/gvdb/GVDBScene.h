#ifndef GVDB_GVDBSCENE_H
#define GVDB_GVDBSCENE_H
// GVDB volumes in a Donut scene graph.
//
// Mirrors Donut's mesh design: IGVDBVolume is shared data (as MeshInfo is),
// GVDBVolumeInstance is the SceneGraphLeaf that places one volume in the
// world (as MeshInstance does), and GVDBSceneGraph is the SceneGraph that
// tracks volume instances next to mesh instances, lights and cameras. A scene
// may hold any number of volumes, and several instances may share one volume.
//
// The node transform of a volume instance maps the volume's index space
// (voxel units) to world space. Renderers iterate GVDBSceneGraph::
// GetVolumeInstances() and read the per-instance VolumeRenderAttributes.
#include <gvdb/GVDB.h>
#include <donut/engine/SceneGraph.h>

namespace gvdb {

// Content flag of volume leaves. Outside the bits Donut defines (0x01..0x20);
// propagated through the node hierarchy like the other flags.
constexpr donut::engine::SceneContentFlags SceneContentFlags_Volumes = donut::engine::SceneContentFlags(0x100);

// Shading modes shared by the CUDA raycaster and the OptiX renderer
// (SHADE_* in the reference).
enum class VolumeShading : uint8_t {
    Voxel = 0,
    Section2D = 1,
    Section3D = 2,
    EmptySkip = 3,
    Trilinear = 4,
    Tricubic = 5,
    LevelSet = 6,
    Volume = 7,
    Off = 100,
};

// A transfer function: TRANSFER_FUNC_SIZE float4 entries over the value range
// [threshold.y, threshold.z] of the instance that uses it, mirrored to the GPU.
class VolumeTransferFunction : public nvrhi::ObjectImpl<nvrhi::IObject> {
 public:
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(VolumeTransferFunction)
    NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IObject)
    NVRHI_END_INTERFACE_TABLE()

    explicit VolumeTransferFunction(donut::gp::IDevice *device);

    // Linear ramp from value a at t0 to value b at t1, t in [0, 1].
    void setLinear(float t0, float t1, dm::float4 a, dm::float4 b);
    const dm::float4 *getData() const { return m_data.data(); }
    dm::float4 *getData() { return m_data.data(); }
    // Uploads the host data; getGPU() is valid afterwards.
    void commit(donut::gp::IDeviceQueue *queue);
    donut::gp::IBuffer *getGPU() const { return m_gpu.Get(); }

 private:
    std::vector<dm::float4> m_data;
    nvrhi::AutoPtr<donut::gp::IDevice> m_device;
    nvrhi::AutoPtr<donut::gp::IBuffer> m_gpu;
};

// Per-instance rendering attributes (the scene-wide volume settings of the
// reference Scene, made per volume so that a scene can mix volumes).
struct VolumeRenderAttributes {
    int channel = 0;                        // channel that is rendered / traced
    int colorChannel = CHAN_UNDEF;          // RGBA8 channel modulating the colour, or CHAN_UNDEF
    VolumeShading shading = VolumeShading::Trilinear;
    dm::float3 threshold = {0.1f, 0.f, 1.f};   // iso value, value min, value max
    dm::float3 steps = {1.f, 16.f, 0.1f};      // direct step, shadow step, fine step (voxels)
    dm::float3 extinct = {-1.1f, 1.5f, 0.f};   // extinction, albedo
    dm::float3 cutoff = {0.005f, 0.01f, 0.f};  // minimum value, alpha cut-off
    float epsilon = 1e-3f;                     // ray / brick boundary epsilon
    int materialId = 0;                        // OptiX material index (OptixRenderer)
    nvrhi::AutoPtr<VolumeTransferFunction> transferFunction;
};

NVRHI_CLASS_CLSID(GVDBVolumeInstance, "bcc94df8-5e6e-40dc-ac4c-334a6736420e")
class GVDBVolumeInstance : public donut::engine::SceneGraphLeaf {
 public:
    NVRHI_DECLARE_UUID_TRAITS(GVDBVolumeInstance)
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(GVDBVolumeInstance)
    NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IWeakReferenceSource)
    NVRHI_IMPLEMENTS_CLASS(GVDBVolumeInstance)
    NVRHI_IMPLEMENTS_ROUTE_PARENT(donut::engine::SceneGraphLeaf)
    NVRHI_END_INTERFACE_TABLE()

    explicit GVDBVolumeInstance(IGVDBVolume *volume) : m_volume(volume) {}

    [[nodiscard]] IGVDBVolume *GetVolume() const { return m_volume.Get(); }
    [[nodiscard]] VolumeRenderAttributes &GetRenderAttributes() { return m_attributes; }
    [[nodiscard]] const VolumeRenderAttributes &GetRenderAttributes() const { return m_attributes; }

    // Voxel bounds of the active bricks (index space).
    [[nodiscard]] dm::box3 GetLocalBoundingBox() override;
    [[nodiscard]] nvrhi::AutoPtr<SceneGraphLeaf> Clone() override;
    [[nodiscard]] donut::engine::SceneContentFlags GetContentFlags() const override {
        return SceneContentFlags_Volumes;
    }

    // Node transform (identity when the instance is not attached).
    [[nodiscard]] dm::affine3 GetIndexToWorld() const;
    [[nodiscard]] dm::affine3 GetWorldToIndex() const;

 private:
    nvrhi::AutoPtr<IGVDBVolume> m_volume;
    VolumeRenderAttributes m_attributes;
};

class GVDBSceneGraph : public donut::engine::SceneGraph {
    NVRHI_INHERIT_INTERFACE_TABLE()
 public:
    using SceneGraph::SceneGraph;

    [[nodiscard]] const std::vector<nvrhi::AutoPtr<GVDBVolumeInstance>> &GetVolumeInstances() const {
        return m_volumeInstances;
    }
    // Distinct volumes referenced by the instances.
    [[nodiscard]] const donut::engine::ResourceTracker<IGVDBVolume> &GetVolumes() const { return m_volumes; }

    donut::engine::SceneResourceCallback<IGVDBVolume> OnVolumeAdded;
    donut::engine::SceneResourceCallback<IGVDBVolume> OnVolumeRemoved;

 protected:
    void RegisterLeaf(donut::engine::SceneGraphLeaf *leaf) override;
    void UnregisterLeaf(donut::engine::SceneGraphLeaf *leaf) override;

 private:
    std::vector<nvrhi::AutoPtr<GVDBVolumeInstance>> m_volumeInstances;
    donut::engine::ResourceTracker<IGVDBVolume> m_volumes;
};

}  // namespace gvdb

#endif /* GVDB_GVDBSCENE_H */
