// Devices and file system of the console samples.
#include "HeadlessContext.h"
#include "SampleTypes.h"
#include <donut/app/ApplicationBase.h>
#include <donut/core/log.h>
#include <cstdio>

namespace SampleUtils {

using namespace donut;

std::filesystem::path HeadlessContext::getAssetPath(const char *name) const {
    return std::filesystem::path(GVDB_SAMPLES_ASSETS_DIR) / name;
}

nvrhi::AutoPtr<gvdb::IGVDBVolume> HeadlessContext::createVolume() const {
    nvrhi::AutoPtr<gvdb::IGVDBVolume> volume;
    if (NVRHI_FAILED(gvdb::createGVDBVolume(gpQueue, vfs, &volume))) {
        log::error("createGVDBVolume failed");
        return nullptr;
    }
    return volume;
}

void HeadlessContext::sync() const {
    syncQueue(gpDevice, gpQueue);
}

bool createHeadlessContext(HeadlessContext &ctx, bool wantOptix) {
    log::EnableOutputToConsole(true);
    log::EnableOutputToMessageBox(false);

    ctx.vfs = MAKE_RC_OBJ_PTR(vfs::RootFileSystem);
    std::filesystem::path exeDir = app::GetDirectoryWithExecutable();
    ctx.vfs->mount("assets", std::filesystem::path(GVDB_SAMPLES_ASSETS_DIR));
    ctx.vfs->mount("ptx", exeDir / "ptx");

    gp::CUDADeviceDesc desc;
    auto callback = MAKE_RC_OBJ_PTR(GPDeviceMessageCallback);
    desc.messageCallback = callback;
    if (NVRHI_FAILED(gp::createCUDADevice(desc, &ctx.gpDevice)) || !ctx.gpDevice) {
        log::error("Cannot create the CUDA device");
        return false;
    }
    gp::DeviceQueueDesc queueDesc;
    queueDesc.priority = gp::DeviceQueuePriority::Normal;
    if (NVRHI_FAILED(ctx.gpDevice->createDeviceQueue(queueDesc, &ctx.gpQueue)) || !ctx.gpQueue) {
        log::error("Cannot create the CUDA queue");
        return false;
    }

    if (wantOptix) {
        gp::RTDeviceDesc rtDesc;
        if (NVRHI_FAILED(gp::createOptiXDevice(rtDesc, ctx.gpDevice, &ctx.rtDevice)) || !ctx.rtDevice) {
            log::warning("OptiX device not available; continuing without it");
            ctx.rtDevice = nullptr;
        }
    }

    ctx.sceneGraph = MAKE_RC_OBJ_PTR(gvdb::GVDBSceneGraph);
    auto root = MAKE_RC_OBJ_PTR(engine::SceneGraphNode);
    root->SetName("root");
    ctx.sceneGraph->SetRootNode(root);
    ctx.camera = MAKE_RC_OBJ_PTR(engine::PerspectiveCamera);
    ctx.camera->zNear = 0.1f;
    ctx.camera->zFar = 5000.f;
    ctx.camera->verticalFov = dm::radians(50.f);
    ctx.cameraNode = ctx.sceneGraph->AttachLeafNode(root, ctx.camera);
    ctx.cameraNode->SetName("camera");
    auto pointLight = MAKE_RC_OBJ_PTR(engine::PointLight);
    ctx.light = pointLight;
    ctx.lightNode = ctx.sceneGraph->AttachLeafNode(root, ctx.light);
    ctx.lightNode->SetName("light");
    ctx.sceneGraph->Refresh(0);
    return true;
}

}  // namespace SampleUtils
