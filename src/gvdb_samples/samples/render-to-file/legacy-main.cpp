// render-to-file (gRenderToFile): load explosion.vbx, volume-render it with the
// CUDA raycaster at 1024x768 and write out_rendtofile.png. Console sample.
#include <sample-utils/HeadlessContext.h>
#include <sample-utils/GVDBApp.h>
#include <sample-utils/VolRenderer.h>
#include <sample-utils/ImageIO.h>
#include <sample-utils/SampleTypes.h>
#include <donut/core/log.h>
#include <donut/core/vfs/VFS.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace donut;
using namespace SampleUtils;

int main(int argc, const char **argv) {
    const int w = 1024, h = 768;
    // Optional: --shade <0..7> overrides the shading mode (VolumeShading values) for debugging.
    int shadeOverride = -1;
    for (int i = 1; i + 1 < argc; ++i)
        if (strcmp(argv[i], "--shade") == 0) shadeOverride = atoi(argv[i + 1]);

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
    volume->logMeasure();

    auto instance = attachVolumeInstance(ctx.sceneGraph, volume, vbxTransformToAffine(vbxTransform), "explosion");
    gvdb::VolumeRenderAttributes &attrs = instance->GetRenderAttributes();
    attrs.channel = 0;
    attrs.shading = shadeOverride >= 0 ? gvdb::VolumeShading(shadeOverride) : gvdb::VolumeShading::Volume;
    attrs.steps = {0.25f, 16.f, 0.25f};        // raycasting steps
    attrs.extinct = {-1.0f, 1.5f, 0.0f};       // volume extinction
    attrs.threshold = {0.1f, 0.0f, 0.1f};      // volume value range
    attrs.cutoff = {0.005f, 0.01f, 0.0f};
    attrs.transferFunction = MAKE_RC_OBJ_PTR(gvdb::VolumeTransferFunction, ctx.gpDevice.Get());
    attrs.transferFunction->setLinear(0.00f, 0.25f, {0.f, 0.f, 0.f, 0.f}, {1.f, 1.f, 0.f, 0.1f});
    attrs.transferFunction->setLinear(0.25f, 0.50f, {1.f, 1.f, 0.f, 0.4f}, {1.f, 0.f, 0.f, 0.3f});
    attrs.transferFunction->setLinear(0.50f, 0.75f, {1.f, 0.f, 0.f, 0.3f}, {0.2f, 0.2f, 0.2f, 0.1f});
    attrs.transferFunction->setLinear(0.75f, 1.00f, {0.2f, 0.2f, 0.2f, 0.1f}, {0.f, 0.f, 0.f, 0.f});
    attrs.transferFunction->commit(ctx.gpQueue);

    // ---- camera and light (the reference setOrbit values; its FOV is horizontal) ----
    ctx.camera->verticalFov = verticalFovFromHorizontal(dm::radians(50.f), float(w) / float(h));
    OrbitController cam;
    cam.setOrbit({20.f, 30.f, 0.f}, {125.f, 160.f, 125.f}, 500.f);
    cam.apply(ctx.cameraNode);
    OrbitController light;
    light.setOrbit({299.f, 57.3f, 0.f}, {132.f, -20.f, 50.f}, 200.f);
    light.apply(ctx.lightNode);
    ctx.sceneGraph->Refresh(0);

    // ---- render ----
    printf("Creating screen buffer. %d x %d\n", w, h);
    nvrhi::AutoPtr<gp::IBuffer> renderBuffer;
    {
        gp::BufferDesc desc;
        desc.byteSize = size_t(w) * h * 4;
        UT_V_GP(ctx.gpDevice->createBuffer(desc, &renderBuffer));
        UT_V_GP(ctx.gpQueue->clearBufferUint(renderBuffer, 0xFF808080u));   // grey: visible when a kernel does not write
    }
    VolRenderer renderer(ctx.gpDevice, ctx.gpQueue, ctx.vfs);
    renderer.getViewParams().backgroundColor = {0.1f, 0.2f, 0.4f, 1.0f};
    RenderView view = makeRenderView(ctx.camera, ctx.light, w, h);

    auto t0 = std::chrono::high_resolution_clock::now();
    renderer.render(ctx.sceneGraph, view, renderBuffer);
    ctx.sync();
    float ms = std::chrono::duration<float, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
    printf("Render volume. %6.3f ms\n", ms);

    std::filesystem::path outPath = "out_rendtofile.png";
    printf("Writing %s\n", outPath.string().c_str());
    if (!saveRenderBufferPNG(outPath, ctx.gpDevice, ctx.gpQueue, renderBuffer, w, h)) return 1;

    printf("Done.\n");
    return 0;
}
