// Devices and file system of the console samples.
#include "HeadlessContext.h"
#include "SampleTypes.h"
#include <donut/app/ApplicationBase.h>
#include <donut/app/DeviceManager.h>
#include <donut/core/log.h>
#include <cstdio>

namespace SampleUtils {

using namespace donut;

std::filesystem::path HeadlessContext::getAssetPath(const char *name) const {
    return std::filesystem::path(GVDB_SAMPLES_ASSETS_DIR) / name;
}

nvrhi::AutoPtr<gvdb::IGVDBVolume> HeadlessContext::createVolume() const {
    nvrhi::AutoPtr<gvdb::IGVDBVolume> volume;
    if (NVRHI_FAILED(gvdb::createGVDBVolume(compute, &volume))) {
        log::error("createGVDBVolume failed");
        return nullptr;
    }
    return volume;
}

void HeadlessContext::sync() const {
    syncCompute(compute);
}

bool createHeadlessContext(HeadlessContext &ctx, bool wantOptix) {
    log::EnableOutputToConsole(true);
    log::EnableOutputToMessageBox(false);

    ctx.vfs = MAKE_RC_OBJ_PTR(vfs::RootFileSystem);
    std::filesystem::path exeDir = app::GetDirectoryWithExecutable();
    ctx.vfs->mount("assets", std::filesystem::path(GVDB_SAMPLES_ASSETS_DIR));
    ctx.vfs->mount("shaders/gvdb", exeDir / "shaders" / "gvdb");

    nvrhi::cuda::GraphicsFactoryDesc factoryDesc;
    factoryDesc.messageCallback = app::DefaultMessageCallback::GetInstance();
    ctx.factory = nvrhi::cuda::createGraphicsFactory(factoryDesc);
    nvrhi::cuda::DeviceDesc desc;
    desc.messageCallback = app::DefaultMessageCallback::GetInstance();
    desc.enableOptix = wantOptix;
    if (!ctx.factory || NVRHI_FAILED(ctx.factory->createDevice(desc, &ctx.cudaDevice)) || !ctx.cudaDevice) {
        log::error("Cannot create the CUDA device");
        return false;
    }
    if (wantOptix && !ctx.cudaDevice->isOptixAvailable()) log::warning("OptiX not available; continuing without it");
    if (NVRHI_FAILED(gvdb::createComputeContext(ctx.cudaDevice, ctx.vfs, &ctx.compute))) {
        log::error("Cannot create the compute context");
        return false;
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
