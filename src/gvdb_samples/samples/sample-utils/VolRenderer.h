#ifndef SAMPLE_UTILS_VOLRENDERER_H
#define SAMPLE_UTILS_VOLRENDERER_H
// CUDA raycaster for the volume instances of a GVDBSceneGraph (the Render /
// RenderKernel / Raytrace paths of the reference VolumeGVDB). Kernel module:
// kernels/GVDBRaycast.cu (served as "ptx/GVDBRaycast.ptx").
#include <nvrhi/core/foundation.h>
#include <nvrhi/core/autoptr.h>
#include <donut/core/vfs/VFS.h>
#include <donut/core/math/math.h>
#include <donut/engine/SceneGraph.h>
#include <gvdb/GVDBScene.h>
#include <cmath>
#include <memory>

namespace SampleUtils {

// Scene-wide settings of a volume render (per-volume settings are the
// VolumeRenderAttributes of the instances).
struct VolumeViewParams {
    dm::float4 backgroundColor = {0.f, 0.f, 0.f, 1.f};
    dm::float3 shadowParams = {0.8f, 1.f, 0.f};    // shadow amount, shadow bias
    dm::float3 sectionPoint = dm::float3::zero();  // cross-section plane (Section2D / Section3D)
    dm::float3 sectionNormal = {0.f, 1.f, 0.f};
    int filterMode = 0;
    float rayNormalBias = 0.f;                     // depth-buffer ray bias
    int frame = 0;
    int sample = 0;
};

// The view a frame is rendered from, in world space: eye position and the
// (non-normalised) directions through the top-left pixel corner plus the
// per-screen increments. Build with makeRenderView().
struct RenderView {
    dm::float3 eye = dm::float3::zero();
    dm::float3 rayTopLeft = {0.f, 0.f, 1.f};   // direction through the top-left corner of the image
    dm::float3 rayU = {1.f, 0.f, 0.f};         // rayTopRight - rayTopLeft
    dm::float3 rayV = {0.f, -1.f, 0.f};        // rayBottomLeft - rayTopLeft
    float zNear = 0.1f;
    float zFar = 1000.f;
    int width = 1;
    int height = 1;
    dm::float3 lightPos = dm::float3::zero();   // world space
    dm::float4x4 viewProj = dm::float4x4::identity();   // for overlays / depth buffer compositing
};

// The view of a Donut camera leaf (node transform = view to world) and a light leaf.
//
// Convention: Donut's SceneCamera looks along its node's -Z axis with +Y up and
// +X right (GetViewToWorldMatrix() flips view +Z onto node -Z); the projection
// is perspProjD3DStyle(verticalFov, aspect, zNear, zFar), so NDC (-1, +1) is the
// top-left image corner. The corner rays are built from the camera basis at the
// near plane: rayTopLeft = fwd*zNear - right*tx + up*ty, rayU = 2*tx*right,
// rayV = -2*ty*up (tx = ty*aspect, ty = tan(fov/2)*zNear). Pixel (x, y) of a
// width x height image, (0,0) top-left, looks along
// rayTopLeft + (x+0.5)/width*rayU + (y+0.5)/height*rayV, which is what the
// GVDB kernels compute from scn.cams / camu / camv.
RenderView makeRenderView(const donut::engine::PerspectiveCamera *camera, const donut::engine::Light *light,
                          int width, int height);
// Same from explicit data: viewToWorld with +X right, +Y up, +Z forward (Donut view space).
RenderView makeRenderView(const dm::affine3 &viewToWorld, float verticalFov, float zNear, float zFar,
                          dm::float3 lightPos, int width, int height);

// The reference Camera3D::setFov() takes the horizontal field of view; Donut's
// PerspectiveCamera stores the vertical one. Both in radians.
inline float verticalFovFromHorizontal(float horizontalFov, float aspect) {
    return 2.f * atanf(tanf(horizontalFov * 0.5f) / aspect);
}

// Ray record of VolRenderer::raytrace (ScnRay in the kernels).
struct GVDB_ALIGN(16) ScnRay {
    dm::float3 hit;      // hit point (index space), hit.z == NOHIT when missed
    dm::float3 normal;
    dm::float3 orig;     // index space
    dm::float3 dir;
    uint32_t clr;        // RGBA8
    uint32_t pnode;
    uint32_t pndx;
};
constexpr float GVDB_NOHIT = 1.0e10f;

class VolRenderer {
 public:
    // vfs must serve "ptx/GVDBRaycast.ptx".
    VolRenderer(donut::gp::IDevice *device, donut::gp::IDeviceQueue *queue, donut::vfs::IFileSystem *vfs);
    ~VolRenderer();

    VolumeViewParams &getViewParams() { return m_viewParams; }
    void setViewParams(const VolumeViewParams &params) { m_viewParams = params; }

    // Optional depth buffer (one float per pixel, post-projection depth in
    // [0,1], width*height of the view) that terminates volume rays.
    void setDepthBuffer(donut::gp::IBuffer *depthBuffer);

    // Renders every volume instance of the graph into outRGBA8 (width*height*4
    // bytes): the first instance clears to the background colour, further
    // instances are composited over with their coverage / opacity.
    //
    // Compositing: an instance after the first is rendered into a scratch
    // buffer with a transparent black background, so the kernels leave
    // premultiplied colour and the opacity in alpha (RayDeep writes
    // colour*opacity / opacity; the surface kernels write an opaque hit or
    // nothing), and the kernel gvdbCompositeOver adds it over the output
    // (dst = src + dst*(1 - src.a)). Section2D/Section3D/EmptySkip always write
    // alpha 255 and therefore replace what is below them.
    void render(gvdb::GVDBSceneGraph *scene, const RenderView &view, donut::gp::IBuffer *outRGBA8);
    // Renders one instance (shading from its attributes unless overridden).
    void renderInstance(gvdb::GVDBVolumeInstance *instance, const RenderView &view, donut::gp::IBuffer *outRGBA8,
                        bool compositeOver, gvdb::VolumeShading overrideShading = gvdb::VolumeShading::Off);
    // Renders one instance with a user kernel `k(VDBInfo*, uchar chan, uchar4* out)`
    // from a module that includes GVDBScene.cuh (its `scn` constant is filled).
    void renderCustom(donut::gp::IKernel *kernel, gvdb::GVDBVolumeInstance *instance, const RenderView &view,
                      donut::gp::IBuffer *outRGBA8);
    // Traces numRays ScnRay records (orig/dir in index space of the instance),
    // filling hit and normal; hits are pulled back by bias along the ray.
    void raytrace(gvdb::GVDBVolumeInstance *instance, donut::gp::IBuffer *rays, uint32_t numRays, float bias,
                  const RenderView &view);

    donut::gp::IDeviceQueue *getQueue() const { return m_queue.Get(); }

 private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    nvrhi::AutoPtr<donut::gp::IDevice> m_device;
    nvrhi::AutoPtr<donut::gp::IDeviceQueue> m_queue;
    nvrhi::AutoPtr<donut::gp::IBuffer> m_depthBuffer;
    VolumeViewParams m_viewParams;
};

}  // namespace SampleUtils

#endif /* SAMPLE_UTILS_VOLRENDERER_H */
