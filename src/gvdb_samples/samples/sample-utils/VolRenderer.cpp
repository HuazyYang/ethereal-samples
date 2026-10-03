// CUDA raycaster over the volume instances of a GVDBSceneGraph.
//
// Per instance the kernels' `scn` constant (ScnInfo, see kernels/GVDBScene.cuh)
// is filled from the view, the instance's VolumeRenderAttributes and its node
// transform, exactly as the reference VolumeGVDB::PrepareRender did:
//   xform    = index -> world (the node transform)
//   invxform = world -> index
//   invxrot  = inverse of the linear (scale * rotation) part: world directions -> index directions
// The matrices are dm::float4x4 (row-major, row-vector convention), which is
// the layout the reference Matrix4F / the kernel mmult() expect.
#include "VolRenderer.h"
#include "SampleTypes.h"
#include <donut/core/log.h>
#include <nvrhi/core/datablob.h>
#include <cstring>
#include <iterator>

namespace SampleUtils {

using namespace donut;

namespace {

// Host mirror of the device ScnInfo (kernels/GVDBScene.cuh). Pointers are
// NativeHandle (CUdeviceptr) sized members so the layout matches on x64.
struct GVDB_ALIGN(16) ScnInfo {
    int width;
    int height;
    float camnear;
    float camfar;
    dm::float3 campos;
    dm::float3 cams;
    dm::float3 camu;
    dm::float3 camv;
    dm::float3 light_pos;
    dm::float3 slice_pnt;
    dm::float3 slice_norm;
    dm::float3 shadow_params;
    dm::float4 backclr;
    float xform[16];
    float invxform[16];
    float invxrot[16];
    float bias;
    char shading;
    char filtering;
    int frame;
    int samples;
    dm::float3 extinct;
    dm::float3 steps;
    dm::float3 cutoff;
    dm::float3 thresh;
    float epsilon;
    int gvdb_channel;
    gp::NativeHandle transfer;
    gp::NativeHandle outbuf;
    gp::NativeHandle dbuf;
};
static_assert(offsetof(ScnInfo, backclr) == 112, "ScnInfo layout differs from the device struct");
static_assert(offsetof(ScnInfo, bias) == 320, "ScnInfo layout differs from the device struct");
static_assert(offsetof(ScnInfo, frame) == 328, "ScnInfo layout differs from the device struct");
static_assert(offsetof(ScnInfo, transfer) == 392, "ScnInfo layout differs from the device struct");
static_assert(sizeof(ScnInfo) == 416, "ScnInfo layout differs from the device struct");

const char *kernelNameForShading(gvdb::VolumeShading shading) {
    switch (shading) {
        case gvdb::VolumeShading::Voxel: return "gvdbRaySurfaceVoxel";
        case gvdb::VolumeShading::Section2D: return "gvdbSection2D";
        case gvdb::VolumeShading::Section3D: return "gvdbSection3D";
        case gvdb::VolumeShading::EmptySkip: return "gvdbRayEmptySkip";
        case gvdb::VolumeShading::Trilinear: return "gvdbRaySurfaceTrilinear";
        case gvdb::VolumeShading::Tricubic: return "gvdbRaySurfaceTricubic";
        case gvdb::VolumeShading::LevelSet: return "gvdbRayLevelSet";
        case gvdb::VolumeShading::Volume: return "gvdbRayDeep";
        default: return nullptr;
    }
}

}  // namespace

struct VolRenderer::Impl {
    nvrhi::AutoPtr<gp::IModule> module;
    nvrhi::AutoPtr<gp::IKernel> kernels[8];
    nvrhi::AutoPtr<gp::IKernel> raytraceKernel;
    nvrhi::AutoPtr<gp::IKernel> compositeKernel;
    nvrhi::AutoPtr<gp::IBuffer> scratch;   // layer buffer for compositing
    size_t scratchBytes = 0;
    nvrhi::AutoPtr<gvdb::VolumeTransferFunction> defaultTransfer;

    gp::IKernel *kernelFor(gvdb::VolumeShading shading) {
        int i = int(shading);
        if (i < 0 || i >= 8) return nullptr;
        return kernels[i].Get();
    }

    // GPU handle of the instance's transfer function (committed on demand), or
    // of a default white ramp when the instance has none.
    gp::NativeHandle getTransfer(gp::IDevice *device, gp::IDeviceQueue *queue, gvdb::GVDBVolumeInstance *instance) {
        gvdb::VolumeTransferFunction *tf = instance->GetRenderAttributes().transferFunction.Get();
        if (!tf) {
            if (!defaultTransfer) {
                defaultTransfer = MAKE_RC_OBJ_PTR(gvdb::VolumeTransferFunction, device);
                defaultTransfer->setLinear(0.f, 1.f, dm::float4(0.f, 0.f, 0.f, 0.f), dm::float4(1.f, 1.f, 1.f, 1.f));
                defaultTransfer->commit(queue);
            }
            tf = defaultTransfer.Get();
        }
        if (!tf->getGPU()) tf->commit(queue);
        return tf->getGPU() ? tf->getGPU()->getNativeHandle() : 0;
    }
};

RenderView makeRenderView(const dm::affine3 &viewToWorld, float verticalFov, float zNear, float zFar,
                          dm::float3 lightPos, int width, int height) {
    RenderView view;
    view.width = std::max(width, 1);
    view.height = std::max(height, 1);
    view.zNear = zNear;
    view.zFar = zFar;
    view.lightPos = lightPos;
    view.eye = viewToWorld.m_translation;

    const dm::float3 right = viewToWorld.m_linear.row0;
    const dm::float3 up = viewToWorld.m_linear.row1;
    const dm::float3 fwd = viewToWorld.m_linear.row2;
    const float aspect = float(view.width) / float(view.height);
    const float ty = tanf(verticalFov * 0.5f) * zNear;
    const float tx = ty * aspect;
    view.rayTopLeft = fwd * zNear - right * tx + up * ty;
    view.rayU = right * (2.f * tx);
    view.rayV = -up * (2.f * ty);

    dm::affine3 worldToView = dm::inverse(viewToWorld);
    view.viewProj = dm::affineToHomogeneous(worldToView) * dm::perspProjD3DStyle(verticalFov, aspect, zNear, zFar);
    return view;
}

RenderView makeRenderView(const engine::PerspectiveCamera *camera, const engine::Light *light, int width,
                          int height) {
    dm::affine3 viewToWorld = camera ? camera->GetViewToWorldMatrix() : dm::affine3::identity();
    float fov = camera ? camera->verticalFov : dm::radians(50.f);
    float zNear = camera ? camera->zNear : 0.1f;
    float zFar = (camera && camera->zFar.has_value()) ? *camera->zFar : 1000.f;
    if (camera && camera->aspectRatio.has_value() && *camera->aspectRatio > 0.f && height > 0) {
        // Respect an explicit aspect ratio by adjusting the image the rays cover.
        width = int(float(height) * *camera->aspectRatio + 0.5f);
    }
    dm::float3 lightPos = light ? dm::float3(light->GetPosition()) : viewToWorld.m_translation;
    return makeRenderView(viewToWorld, fov, zNear, zFar, lightPos, width, height);
}

VolRenderer::VolRenderer(gp::IDevice *device, gp::IDeviceQueue *queue, vfs::IFileSystem *vfs)
    : m_impl(std::make_unique<Impl>()), m_device(device), m_queue(queue) {
    nvrhi::AutoPtr<nvrhi::IDataBlob> blob;
    if (!vfs || NVRHI_FAILED(vfs->readFile("ptx/GVDBRaycast.ptx", &blob)) || !blob) {
        log::error("VolRenderer: cannot read ptx/GVDBRaycast.ptx");
        return;
    }
    size_t len = blob->GetSize();
    blob->Resize(len + 1);
    static_cast<char *>(blob->GetDataPtr())[len] = 0;
    UT_V_GP(m_device->createModule({}, blob->GetDataPtr(), len + 1, &m_impl->module));
    if (!m_impl->module) return;

    for (int i = 0; i < 8; ++i) {
        const char *name = kernelNameForShading(gvdb::VolumeShading(i));
        if (NVRHI_FAILED(m_impl->module->getKernel(name, &m_impl->kernels[i])))
            log::error("VolRenderer: kernel %s not found in GVDBRaycast.ptx", name);
    }
    UT_V_GP(m_impl->module->getKernel("gvdbRaytrace", &m_impl->raytraceKernel));
    UT_V_GP(m_impl->module->getKernel("gvdbCompositeOver", &m_impl->compositeKernel));
}

VolRenderer::~VolRenderer() {}

void VolRenderer::setDepthBuffer(gp::IBuffer *depthBuffer) { m_depthBuffer = depthBuffer; }

namespace {

void fillMatrices(ScnInfo &scn, const dm::affine3 &indexToWorld) {
    dm::float4x4 xform = dm::affineToHomogeneous(indexToWorld);
    dm::float4x4 invxform = dm::inverse(xform);
    dm::float3x3 invLinear = dm::inverse(indexToWorld.m_linear);
    dm::float4x4 invxrot = dm::affineToHomogeneous(dm::affine3(invLinear, dm::float3::zero()));
    static_assert(sizeof(xform) == sizeof(scn.xform), "matrix size");
    memcpy(scn.xform, &xform, sizeof(scn.xform));
    memcpy(scn.invxform, &invxform, sizeof(scn.invxform));
    memcpy(scn.invxrot, &invxrot, sizeof(scn.invxrot));
}

}  // namespace

// Fills `scn` for one instance. transfer: the GPU transfer function handle.
static void prepareScnInfo(ScnInfo &scn, const VolumeViewParams &params, const RenderView &view,
                           gvdb::GVDBVolumeInstance *instance, gvdb::VolumeShading shading, dm::float4 background,
                           gp::NativeHandle transfer, gp::NativeHandle depthBuffer, int width, int height) {
    const gvdb::VolumeRenderAttributes &attrs = instance->GetRenderAttributes();
    dm::affine3 indexToWorld = instance->GetIndexToWorld();
    dm::affine3 worldToIndex = dm::inverse(indexToWorld);

    memset(&scn, 0, sizeof(scn));
    scn.width = width;
    scn.height = height;
    scn.camnear = view.zNear;
    scn.camfar = view.zFar;
    scn.campos = view.eye;
    scn.cams = view.rayTopLeft;
    scn.camu = view.rayU;
    scn.camv = view.rayV;
    scn.light_pos = worldToIndex.transformPoint(view.lightPos);
    scn.slice_pnt = params.sectionPoint;
    scn.slice_norm = params.sectionNormal;
    scn.shadow_params = params.shadowParams;
    scn.backclr = background;
    fillMatrices(scn, indexToWorld);
    scn.bias = params.rayNormalBias;
    scn.shading = char(shading);
    scn.filtering = char(params.filterMode);
    scn.frame = params.frame;
    scn.samples = params.sample;
    scn.extinct = attrs.extinct;
    scn.steps = attrs.steps;
    scn.cutoff = attrs.cutoff;
    scn.thresh = attrs.threshold;
    scn.epsilon = attrs.epsilon;
    scn.gvdb_channel = attrs.colorChannel;
    scn.transfer = transfer;
    scn.outbuf = 0;
    scn.dbuf = depthBuffer;
}

void VolRenderer::render(gvdb::GVDBSceneGraph *scene, const RenderView &view, gp::IBuffer *outRGBA8) {
    if (!scene || !outRGBA8) return;
    bool first = true;
    for (const auto &instance : scene->GetVolumeInstances()) {
        renderInstance(instance.Get(), view, outRGBA8, !first);
        first = false;
    }
    if (first) {
        // No volumes: clear to the background colour.
        dm::float4 c = dm::saturate(m_viewParams.backgroundColor) * 255.f;
        uint32_t packed = uint32_t(c.x) | (uint32_t(c.y) << 8) | (uint32_t(c.z) << 16) | (uint32_t(c.w) << 24);
        UT_V_GP(m_queue->clearBufferUint(outRGBA8, packed));
    }
}

void VolRenderer::renderInstance(gvdb::GVDBVolumeInstance *instance, const RenderView &view, gp::IBuffer *outRGBA8,
                                 bool compositeOver, gvdb::VolumeShading overrideShading) {
    if (!instance || !outRGBA8 || !m_impl->module) return;
    gvdb::IGVDBVolume *volume = instance->GetVolume();
    if (!volume) return;

    gvdb::VolumeShading shading =
        overrideShading != gvdb::VolumeShading::Off ? overrideShading : instance->GetRenderAttributes().shading;
    const int width = view.width, height = view.height;
    const size_t bytes = size_t(width) * size_t(height) * 4;

    if (shading == gvdb::VolumeShading::Off) {
        if (!compositeOver) UT_V_GP(m_queue->clearBufferUint(outRGBA8, 0u));
        return;
    }
    gp::IKernel *kernel = m_impl->kernelFor(shading);
    if (!kernel) {
        log::error("VolRenderer: no kernel for shading mode %d", int(shading));
        return;
    }

    gp::IBuffer *target = outRGBA8;
    dm::float4 background = m_viewParams.backgroundColor;
    if (compositeOver) {
        if (!m_impl->scratch || m_impl->scratchBytes < bytes) {
            gp::BufferDesc desc;
            desc.byteSize = bytes;
            m_impl->scratch = nullptr;
            UT_V_GP(m_device->createBuffer(desc, &m_impl->scratch));
            m_impl->scratchBytes = bytes;
        }
        target = m_impl->scratch.Get();
        background = dm::float4::zero();   // transparent: kernels leave premultiplied colour + opacity
    }

    volume->prepareVDB();

    ScnInfo scn;
    prepareScnInfo(scn, m_viewParams, view, instance, shading, background,
                   m_impl->getTransfer(m_device, m_queue, instance),
                   m_depthBuffer ? m_depthBuffer->getNativeHandle() : 0, width, height);
    UT_V_GP(m_queue->setConstantBuffer(kernel, "scn", &scn, sizeof(scn)));

    gvdb::uchar chan = gvdb::uchar(instance->GetRenderAttributes().channel);
    gp::KernelArg args[] = {gp::KernelArg::Buffer(volume->getVDBInfoGPU()), gp::KernelArg::Scalar(chan),
                            gp::KernelArg::Buffer(target)};
    gp::dim3 block{16, 16, 1};
    gp::dim3 grid{(width + block.x - 1) / block.x, (height + block.y - 1) / block.y, 1};
    UT_V_GP(m_queue->launch(kernel, grid, block, args, std::size(args)));

    if (compositeOver) {
        int numPixels = width * height;
        gp::KernelArg cargs[] = {gp::KernelArg::Buffer(outRGBA8), gp::KernelArg::Buffer(target),
                                 gp::KernelArg::Scalar(numPixels)};
        gp::dim3 cblock{256, 1, 1};
        gp::dim3 cgrid{(numPixels + 255) / 256, 1, 1};
        UT_V_GP(m_queue->launch(m_impl->compositeKernel, cgrid, cblock, cargs, std::size(cargs)));
    }
}

void VolRenderer::renderCustom(gp::IKernel *kernel, gvdb::GVDBVolumeInstance *instance, const RenderView &view,
                               gp::IBuffer *outRGBA8) {
    if (!kernel || !instance || !outRGBA8) return;
    gvdb::IGVDBVolume *volume = instance->GetVolume();
    if (!volume) return;
    volume->prepareVDB();

    ScnInfo scn;
    prepareScnInfo(scn, m_viewParams, view, instance, instance->GetRenderAttributes().shading,
                   m_viewParams.backgroundColor, m_impl->getTransfer(m_device, m_queue, instance),
                   m_depthBuffer ? m_depthBuffer->getNativeHandle() : 0, view.width, view.height);
    // The user module has its own `scn` symbol (it includes GVDBScene.cuh).
    UT_V_GP(m_queue->setConstantBuffer(kernel, "scn", &scn, sizeof(scn)));

    gvdb::uchar chan = gvdb::uchar(instance->GetRenderAttributes().channel);
    gp::KernelArg args[] = {gp::KernelArg::Buffer(volume->getVDBInfoGPU()), gp::KernelArg::Scalar(chan),
                            gp::KernelArg::Buffer(outRGBA8)};
    gp::dim3 block{8, 8, 1};
    gp::dim3 grid{(view.width + block.x - 1) / block.x, (view.height + block.y - 1) / block.y, 1};
    UT_V_GP(m_queue->launch(kernel, grid, block, args, std::size(args)));
}

void VolRenderer::raytrace(gvdb::GVDBVolumeInstance *instance, gp::IBuffer *rays, uint32_t numRays, float bias,
                           const RenderView &view) {
    if (!instance || !rays || numRays == 0 || !m_impl->raytraceKernel) return;
    gvdb::IGVDBVolume *volume = instance->GetVolume();
    if (!volume) return;
    volume->prepareVDB();

    // As the reference Raytrace: PrepareRender(1, 1); no depth buffer (the
    // rays are not screen rays).
    ScnInfo scn;
    prepareScnInfo(scn, m_viewParams, view, instance, instance->GetRenderAttributes().shading,
                   m_viewParams.backgroundColor, m_impl->getTransfer(m_device, m_queue, instance),
                   0, 1, 1);
    UT_V_GP(m_queue->setConstantBuffer(m_impl->raytraceKernel, "scn", &scn, sizeof(scn)));

    gvdb::uchar chan = gvdb::uchar(instance->GetRenderAttributes().channel);
    int count = int(numRays);
    gp::KernelArg args[] = {gp::KernelArg::Buffer(volume->getVDBInfoGPU()), gp::KernelArg::Scalar(chan),
                            gp::KernelArg::Scalar(count), gp::KernelArg::Buffer(rays), gp::KernelArg::Scalar(bias)};
    gp::dim3 block{64, 1, 1};
    gp::dim3 grid{(count + block.x - 1) / block.x, 1, 1};
    UT_V_GP(m_queue->launch(m_impl->raytraceKernel, grid, block, args, std::size(args)));
}

}  // namespace SampleUtils
