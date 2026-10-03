#ifndef SAMPLE_UTILS_HEADLESSCONTEXT_H
#define SAMPLE_UTILS_HEADLESSCONTEXT_H
// Devices and file system for the console samples (render-to-file,
// render-kernel): no window, no nvrhi device. Same mounts as GVDBApp
// ("assets", "ptx").
#include <nvrhi/core/autoptr.h>
#include <donut/core/vfs/VFS.h>
#include <gvdb/GPDevice.h>
#include <gvdb/GPDeviceCUDA.h>
#include <gvdb/GPDeviceOptiX.h>
#include <gvdb/GVDBScene.h>
#include <filesystem>

namespace SampleUtils {

struct HeadlessContext {
    nvrhi::AutoPtr<donut::vfs::RootFileSystem> vfs;
    nvrhi::AutoPtr<donut::gp::IDevice> gpDevice;
    nvrhi::AutoPtr<donut::gp::IDeviceQueue> gpQueue;
    nvrhi::AutoPtr<donut::gp::IRTDevice> rtDevice;   // only with wantOptix and OptiX available
    nvrhi::AutoPtr<gvdb::GVDBSceneGraph> sceneGraph;
    // Camera and light leaves under their own nodes of the root (drive them
    // with OrbitController::apply() from GVDBApp.h, then Refresh the graph).
    nvrhi::AutoPtr<donut::engine::PerspectiveCamera> camera;
    nvrhi::AutoPtr<donut::engine::SceneGraphNode> cameraNode;
    nvrhi::AutoPtr<donut::engine::Light> light;
    nvrhi::AutoPtr<donut::engine::SceneGraphNode> lightNode;

    std::filesystem::path getAssetPath(const char *name) const;
    nvrhi::AutoPtr<gvdb::IGVDBVolume> createVolume() const;
    // Waits for the gp queue to finish.
    void sync() const;
};

// Logs to stdout. Returns false when the CUDA device cannot be created.
bool createHeadlessContext(HeadlessContext &ctx, bool wantOptix = false);

}  // namespace SampleUtils

#endif /* SAMPLE_UTILS_HEADLESSCONTEXT_H */
