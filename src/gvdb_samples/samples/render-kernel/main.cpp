// render-kernel (gRenderKernel): load explosion.vbx and render it with the user
// raycast kernel of RenderCustom.cu (ptx/RenderCustom.ptx) through
// VolRenderer::renderCustom; writes out_rendkernel.png. Console sample.
#include <sample-utils/HeadlessContext.h>
#include <sample-utils/GVDBApp.h>
#include <sample-utils/VolRenderer.h>
#include <sample-utils/ImageIO.h>
#include <sample-utils/SampleTypes.h>
#include <donut/core/log.h>
#include <donut/core/vfs/VFS.h>
#include <nvrhi/core/datablob.h>
#include <chrono>
#include <cstdio>

using namespace donut;
using namespace SampleUtils;

int main(int argc, const char **argv) {
    const int w = 1024, h = 768;

    printf("Starting GVDB.\n");
    HeadlessContext ctx;
    if (!createHeadlessContext(ctx)) {
        printf("Cannot create the CUDA device.\n");
        return 1;
    }

    // ---- volume from VBX ----
    std::filesystem::path vbxPath = ctx.getAssetPath("explosion.vbx");
    printf("Loading VBX. %s\n", vbxPath.string().c_str());
    nvrhi::AutoPtr<nvrhi::IDataBlob> vbx;
    {
        auto fs = MAKE_RC_OBJ_PTR(vfs::NativeFileSystem);
        if (NVRHI_FAILED(fs->readFile(vbxPath, &vbx)) || !vbx) {
            printf("Cannot find vbx file.\n");
            return 1;
        }
    }
    auto volume = ctx.createVolume();
    if (!volume) return 1;
    nvrhi::AutoPtr<gvdb::IGVDBSerializer> serializer;
    if (NVRHI_FAILED(gvdb::createGVDBSerializer(volume, &serializer))) return 1;
    gvdb::VBXTransform vbxTransform;
    if (NVRHI_FAILED(serializer->loadVBX(vbx, &vbxTransform))) {
        printf("Failed to load the VBX file.\n");
        return 1;
    }

    auto instance = attachVolumeInstance(ctx.sceneGraph, volume, vbxTransformToAffine(vbxTransform), "explosion");
    gvdb::VolumeRenderAttributes &attrs = instance->GetRenderAttributes();
    attrs.channel = 0;
    attrs.shading = gvdb::VolumeShading::Trilinear;
    attrs.steps = {0.25f, 16.f, 0.25f};
    attrs.threshold = {0.1f, 0.0f, 1.0f};

    // ---- camera and light ----
    ctx.camera->verticalFov = verticalFovFromHorizontal(dm::radians(50.f), float(w) / float(h));
    OrbitController cam;
    cam.setOrbit({20.f, 30.f, 0.f}, {125.f, 160.f, 125.f}, 800.f);
    cam.apply(ctx.cameraNode);
    OrbitController light;
    light.setOrbit({50.f, 65.f, 0.f}, {125.f, 140.f, 125.f}, 200.f);
    light.apply(ctx.lightNode);
    ctx.sceneGraph->Refresh(0);

    // ---- user kernel ----
    printf("Loading module: RenderCustom.ptx\n");
    nvrhi::AutoPtr<gp::IModule> module;
    nvrhi::AutoPtr<gp::IKernel> kernel;
    {
        nvrhi::AutoPtr<nvrhi::IDataBlob> ptx;
        if (NVRHI_FAILED(ctx.vfs->readFile("ptx/RenderCustom.ptx", &ptx)) || !ptx) {
            printf("Cannot read ptx/RenderCustom.ptx\n");
            return 1;
        }
        size_t len = ptx->GetSize();
        ptx->Resize(len + 1);
        static_cast<char *>(ptx->GetDataPtr())[len] = 0;
        UT_V_GP(ctx.gpDevice->createModule({}, ptx->GetDataPtr(), len + 1, &module));
        UT_V_GP(module->getKernel("raycast_kernel", &kernel));
    }

    // ---- render ----
    printf("Creating screen buffer. %d x %d\n", w, h);
    nvrhi::AutoPtr<gp::IBuffer> renderBuffer;
    {
        gp::BufferDesc desc;
        desc.byteSize = size_t(w) * h * 4;
        UT_V_GP(ctx.gpDevice->createBuffer(desc, &renderBuffer));
    }
    VolRenderer renderer(ctx.gpDevice, ctx.gpQueue, ctx.vfs);
    RenderView view = makeRenderView(ctx.camera, ctx.light, w, h);

    printf("Render custom kernel.\n");
    auto t0 = std::chrono::high_resolution_clock::now();
    renderer.renderCustom(kernel, instance, view, renderBuffer);
    ctx.sync();
    float ms = std::chrono::duration<float, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
    printf("Render custom. %6.3f ms\n", ms);

    std::filesystem::path outPath = "out_rendkernel.png";
    printf("Writing %s\n", outPath.string().c_str());
    if (!saveRenderBufferPNG(outPath, ctx.gpDevice, ctx.gpQueue, renderBuffer, w, h)) return 1;

    printf("Done.\n");
    return 0;
}
