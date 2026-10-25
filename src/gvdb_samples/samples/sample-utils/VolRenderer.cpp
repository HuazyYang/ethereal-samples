// CUDA raycaster over the volume instances of a GVDBSceneGraph.
//
// Per instance the kernels' ScnInfo (see kernels/GVDBScene.slangh) is filled
// from the view, the instance's VolumeRenderAttributes and its node transform,
// exactly as the reference VolumeGVDB::PrepareRender did:
//   xform    = index -> world (the node transform)
//   invxform = world -> index
//   invxrot  = inverse of the linear (scale * rotation) part: world directions -> index directions
// The matrices are dm::float4x4 (row-major, row-vector convention), which is
// the layout the reference Matrix4F / the kernel mmult() expect. The block is
// written into a buffer before each launch (same command list, so every launch
// reads its own version) and passed by address.
#include "VolRenderer.h"
#include "SampleTypes.h"
#include <donut/core/log.h>
#include <cstddef>
#include <cstring>

namespace SampleUtils {

using namespace donut;

namespace {

// Host mirror of ScnInfo (kernels/GVDBScene.slangh). Pointers are device addresses.
struct ScnInfo {
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
    int8_t shading;
    int8_t filtering;
    int frame;
    int samples;
    dm::float3 extinct;
    dm::float3 steps;
    dm::float3 cutoff;
    dm::float3 thresh;
    float epsilon;
    int gvdb_channel;
    uint64_t transfer;
    uint64_t outbuf;
    uint64_t dbuf;
};
static_assert(offsetof(ScnInfo, backclr) == 112, "ScnInfo layout differs from the device struct");
static_assert(offsetof(ScnInfo, bias) == 320, "ScnInfo layout differs from the device struct");
static_assert(offsetof(ScnInfo, frame) == 328, "ScnInfo layout differs from the device struct");
static_assert(offsetof(ScnInfo, transfer) == 392, "ScnInfo layout differs from the device struct");
static_assert(offsetof(ScnInfo, dbuf) == 408, "ScnInfo layout differs from the device struct");

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
    gvdb::IComputeKernel *kernels[8] = {};
    gvdb::IComputeKernel *raytraceKernel = nullptr;
    gvdb::IComputeKernel *compositeKernel = nullptr;
    nvrhi::BufferHandle scratch;   // layer buffer for compositing
    nvrhi::BufferHandle scnBuffer;   // the ScnInfo of the next launch
    nvrhi::AutoPtr<gvdb::VolumeTransferFunction> defaultTransfer;

    gvdb::IComputeKernel *kernelFor(gvdb::VolumeShading shading) {
        int i = int(shading);
        if (i < 0 || i >= 8) return nullptr;
        return kernels[i];
    }

    // Device address of the instance's transfer function (committed on demand), or
    // of a default white ramp when the instance has none.
    uint64_t getTransfer(gvdb::IComputeContext *context, gvdb::GVDBVolumeInstance *instance) {
        gvdb::VolumeTransferFunction *tf = instance->GetRenderAttributes().transferFunction.Get();
        if (!tf) {
            if (!defaultTransfer) {
                defaultTransfer = MAKE_RC_OBJ_PTR(gvdb::VolumeTransferFunction, context);
                defaultTransfer->setLinear(0.f, 1.f, dm::float4(0.f, 0.f, 0.f, 0.f), dm::float4(1.f, 1.f, 1.f, 1.f));
                defaultTransfer->commit();
            }
            tf = defaultTransfer.Get();
        }
        if (!tf->getGPU()) tf->commit();
        return gvdb::deviceAddress(tf->getGPU());
    }

    // Records the upload of the block of the next launch; returns its address.
    uint64_t uploadScn(gvdb::IComputeContext *context, const ScnInfo &scn) {
        if (!scnBuffer) {
            nvrhi::BufferDesc desc;
            desc.byteSize = sizeof(ScnInfo);
            desc.debugName = "VolRenderer ScnInfo";
            UT_CHECK(context->getDevice()->createBuffer(desc, &scnBuffer));
        }
        context->getCommandList()->writeBuffer(scnBuffer, &scn, sizeof(scn), 0);
        return gvdb::deviceAddress(scnBuffer);
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

VolRenderer::VolRenderer(gvdb::IComputeContext *context) : m_impl(std::make_unique<Impl>()), m_context(context) {
    for (int i = 0; i < 8; ++i)
        m_impl->kernels[i] = m_context->getKernel(GVDB_SHADERS_RAYCAST, kernelNameForShading(gvdb::VolumeShading(i)));
    m_impl->raytraceKernel = m_context->getKernel(GVDB_SHADERS_RAYCAST, "gvdbRaytrace");
    m_impl->compositeKernel = m_context->getKernel(GVDB_SHADERS_RAYCAST, "gvdbCompositeOver");
}

VolRenderer::~VolRenderer() {
    if (m_context) m_context->finish();
}

void VolRenderer::setDepthBuffer(nvrhi::IBuffer *depthBuffer) {
    m_context->releaseAfterUse(m_depthBuffer.Get());
    m_depthBuffer = depthBuffer;
}

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

// Fills `scn` for one instance. transfer: the device address of the transfer function.
static void prepareScnInfo(ScnInfo &scn, const VolumeViewParams &params, const RenderView &view,
                           gvdb::GVDBVolumeInstance *instance, gvdb::VolumeShading shading, dm::float4 background,
                           uint64_t transfer, uint64_t depthBuffer, int width, int height) {
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
    scn.shading = int8_t(shading);
    scn.filtering = int8_t(params.filterMode);
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

void VolRenderer::render(gvdb::GVDBSceneGraph *scene, const RenderView &view, nvrhi::IBuffer *outRGBA8) {
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
        m_context->getCommandList()->clearBufferUInt(outRGBA8, packed);
    }
}

void VolRenderer::renderInstance(gvdb::GVDBVolumeInstance *instance, const RenderView &view, nvrhi::IBuffer *outRGBA8,
                                 bool compositeOver, gvdb::VolumeShading overrideShading) {
    if (!instance || !outRGBA8) return;
    gvdb::IGVDBVolume *volume = instance->GetVolume();
    if (!volume) return;

    gvdb::VolumeShading shading =
        overrideShading != gvdb::VolumeShading::Off ? overrideShading : instance->GetRenderAttributes().shading;
    const int width = view.width, height = view.height;
    const size_t bytes = size_t(width) * size_t(height) * 4;

    if (shading == gvdb::VolumeShading::Off) {
        if (!compositeOver) m_context->getCommandList()->clearBufferUInt(outRGBA8, 0u);
        return;
    }
    gvdb::IComputeKernel *kernel = m_impl->kernelFor(shading);
    if (!kernel) {
        log::error("VolRenderer: no kernel for shading mode %d", int(shading));
        return;
    }

    nvrhi::IBuffer *target = outRGBA8;
    dm::float4 background = m_viewParams.backgroundColor;
    if (compositeOver) {
        if (!m_impl->scratch || m_impl->scratch->getDesc().byteSize < bytes) {
            m_context->releaseAfterUse(m_impl->scratch.Get());
            nvrhi::BufferDesc desc;
            desc.byteSize = bytes;
            desc.debugName = "VolRenderer layer";
            m_impl->scratch = nullptr;
            UT_CHECK(m_context->getDevice()->createBuffer(desc, &m_impl->scratch));
        }
        target = m_impl->scratch.Get();
        background = dm::float4::zero();   // transparent: kernels leave premultiplied colour + opacity
    }

    volume->prepareVDB();

    ScnInfo scn;
    prepareScnInfo(scn, m_viewParams, view, instance, shading, background, m_impl->getTransfer(m_context, instance),
                   gvdb::deviceAddress(m_depthBuffer), width, height);
    const uint64_t scnAddress = m_impl->uploadScn(m_context, scn);

    nvrhi::ICommandList *commandList = m_context->getCommandList();
    const uint8_t chan = uint8_t(instance->GetRenderAttributes().channel);
    gvdb::launchKernel(commandList, kernel, gvdb::groupsFor(kernel, dm::uint3(uint32_t(width), uint32_t(height), 1u)),
                       scnAddress, gvdb::deviceAddress(volume->getVDBInfoGPU()), chan, gvdb::deviceAddress(target));

    if (compositeOver) {
        const int numPixels = width * height;
        gvdb::launchKernel(commandList, m_impl->compositeKernel, gvdb::groupsFor(m_impl->compositeKernel, uint64_t(numPixels)),
                           gvdb::deviceAddress(outRGBA8), gvdb::deviceAddress(target), numPixels);
    }
}

void VolRenderer::renderCustom(gvdb::IComputeKernel *kernel, gvdb::GVDBVolumeInstance *instance, const RenderView &view,
                               nvrhi::IBuffer *outRGBA8) {
    if (!kernel || !instance || !outRGBA8) return;
    gvdb::IGVDBVolume *volume = instance->GetVolume();
    if (!volume) return;
    volume->prepareVDB();

    ScnInfo scn;
    prepareScnInfo(scn, m_viewParams, view, instance, instance->GetRenderAttributes().shading,
                   m_viewParams.backgroundColor, m_impl->getTransfer(m_context, instance),
                   gvdb::deviceAddress(m_depthBuffer), view.width, view.height);
    const uint64_t scnAddress = m_impl->uploadScn(m_context, scn);

    const uint8_t chan = uint8_t(instance->GetRenderAttributes().channel);
    gvdb::launchKernel(m_context->getCommandList(), kernel,
                       gvdb::groupsFor(kernel, dm::uint3(uint32_t(view.width), uint32_t(view.height), 1u)), scnAddress,
                       gvdb::deviceAddress(volume->getVDBInfoGPU()), chan, gvdb::deviceAddress(outRGBA8));
}

void VolRenderer::raytrace(gvdb::GVDBVolumeInstance *instance, nvrhi::IBuffer *rays, uint32_t numRays, float bias,
                           const RenderView &view) {
    if (!instance || !rays || numRays == 0 || !m_impl->raytraceKernel) return;
    gvdb::IGVDBVolume *volume = instance->GetVolume();
    if (!volume) return;
    volume->prepareVDB();

    // As the reference Raytrace: PrepareRender(1, 1); no depth buffer (the
    // rays are not screen rays).
    ScnInfo scn;
    prepareScnInfo(scn, m_viewParams, view, instance, instance->GetRenderAttributes().shading,
                   m_viewParams.backgroundColor, m_impl->getTransfer(m_context, instance), 0, 1, 1);
    const uint64_t scnAddress = m_impl->uploadScn(m_context, scn);

    const uint8_t chan = uint8_t(instance->GetRenderAttributes().channel);
    gvdb::launchKernel(m_context->getCommandList(), m_impl->raytraceKernel,
                       gvdb::groupsFor(m_impl->raytraceKernel, uint64_t(numRays)), scnAddress,
                       gvdb::deviceAddress(volume->getVDBInfoGPU()), chan, int(numRays), gvdb::deviceAddress(rays), bias);
}

}  // namespace SampleUtils
