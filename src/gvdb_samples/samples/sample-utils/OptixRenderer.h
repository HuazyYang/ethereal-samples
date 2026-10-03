#ifndef SAMPLE_UTILS_OPTIXRENDERER_H
#define SAMPLE_UTILS_OPTIXRENDERER_H
// OptiX renderer for a GVDBSceneGraph (port of the reference OptixScene onto
// the gp OptiX interfaces, GPDeviceOptiX.h). Volume instances become custom
// AABB primitives whose intersection programs run the GVDB raycaster; mesh
// instances become triangle geometry; everything is instanced in one IAS so
// the node transforms of the scene graph apply directly. Device programs:
// kernels/optix/OptixPrograms.cu ("ptx/OptixPrograms.ptx").
#include <nvrhi/core/foundation.h>
#include <nvrhi/core/autoptr.h>
#include <donut/core/vfs/VFS.h>
#include <donut/core/math/math.h>
#include <gvdb/GPDeviceOptiX.h>
#include <gvdb/GVDBScene.h>
#include <sample-utils/VolRenderer.h>
#include <memory>

namespace SampleUtils {

// Surface material of the OptiX closest-hit programs (MaterialParams of the reference).
struct OptixMaterialParams {
    float lightWidth = 0.f;      // light scatter
    dm::float3 ambColor = dm::float3::zero();
    dm::float3 envColor = dm::float3::zero();    // x == 1: checkerboard with cell size y and dark value z
    dm::float3 diffColor = {.6f, .7f, .7f};
    dm::float3 specColor = {3.f, 3.f, 3.f};
    float specPower = 400.f;
    float shadowWidth = 0.f;     // shadow scatter
    float shadowBias = 0.f;
    float reflWidth = 0.f;       // reflect scatter
    dm::float3 reflColor = {1.f, 1.f, 1.f};
    float reflBias = 0.f;
    float refrWidth = 0.f;       // refract scatter
    float refrIor = 1.2f;
    dm::float3 refrColor = {.35f, .4f, .4f};
    float refrAmount = 10.f;
    float refrOffset = 15.f;
    float refrBias = 0.f;
};

// How a volume instance is intersected ('S', 'D', 'L', 'E' of the reference).
enum class OptixVolumeIsect : uint8_t { Surface, Deep, LevelSet, EmptySkip };

class OptixRenderer {
 public:
    // vfs must serve "ptx/OptixPrograms.ptx".
    OptixRenderer(donut::gp::IDevice *cudaDevice, donut::gp::IRTDevice *rtDevice, donut::gp::IDeviceQueue *queue,
                  donut::vfs::IFileSystem *vfs);
    ~OptixRenderer();

    // Output: RGBA8 (width*height*4) for presentation plus an internal float
    // accumulation buffer for progressive sampling.
    void resizeOutput(uint32_t width, uint32_t height);
    donut::gp::IBuffer *getOutputBuffer() const;
    uint32_t getWidth() const;
    uint32_t getHeight() const;
    // Reads the RGBA8 output back to the host.
    void readOutput(std::vector<uint8_t> &rgba8);

    // Materials, referenced by VolumeRenderAttributes::materialId and by addMesh / mesh instances.
    int addMaterial(const OptixMaterialParams &params);
    void setMaterialParams(int id, const OptixMaterialParams &params);
    int getNumMaterials() const;

    // Environment map (RGBA8, equirect-like as in the reference); nullptr = white.
    void setEnvmap(const uint8_t *rgba8, uint32_t width, uint32_t height);

    VolumeViewParams &getViewParams();
    void setViewParams(const VolumeViewParams &params);

    // Builds the acceleration structures, SBT and per-instance tables from the
    // scene graph: one AABB primitive per volume instance (bounds from the
    // volume, isect from the shading of its attributes: LevelSet / Volume /
    // EmptySkip / else Surface) and one triangle GAS per mesh (MeshInfo CPU
    // buffers), instanced per MeshInstance with the material from
    // getMeshMaterial(). Call again after instances were added / removed.
    void buildScene(gvdb::GVDBSceneGraph *scene);
    // Material used for mesh instances (default 0); per-instance override by name.
    void setMeshMaterial(int materialId);
    void setMeshMaterial(const donut::engine::MeshInstance *instance, int materialId);
    // After a volume's topology or data changed (re-prepares VDBInfo, refits the AABBs).
    void updateVolumes(gvdb::GVDBSceneGraph *scene);
    // After node transforms changed (rebuilds the IAS).
    void updateTransforms(gvdb::GVDBSceneGraph *scene);
    // After mesh vertex data changed (rebuilds the mesh GAS).
    void updateMeshes(gvdb::GVDBSceneGraph *scene);

    // Progressive sampling: sample 0 restarts the accumulation.
    void setSample(int frame, int sample);
    // Renders the scene; the volume shading comes from each instance's
    // attributes (overrideShading != Off applies to every volume).
    void render(gvdb::GVDBSceneGraph *scene, const RenderView &view,
                gvdb::VolumeShading overrideShading = gvdb::VolumeShading::Off);

    donut::gp::IDeviceQueue *getQueue() const;

 private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace SampleUtils

#endif /* SAMPLE_UTILS_OPTIXRENDERER_H */
