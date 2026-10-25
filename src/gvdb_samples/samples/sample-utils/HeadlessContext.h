#ifndef SAMPLE_UTILS_HEADLESSCONTEXT_H
#define SAMPLE_UTILS_HEADLESSCONTEXT_H
// Devices and file system for the console samples (render-to-file,
// render-kernel): no window, no graphics device; an nvrhi CUDA device created
// through a graphics factory of its own. Same mounts as GVDBApp ("assets",
// "shaders/gvdb").
#include <nvrhi/core/autoptr.h>
#include <nvrhi/cuda.h>
#include <nvrhi/factory.h>
#include <donut/core/vfs/VFS.h>
#include <gvdb/GVDBCompute.h>
#include <gvdb/GVDBScene.h>
#include <filesystem>

namespace SampleUtils {

struct HeadlessContext {
    nvrhi::AutoPtr<donut::vfs::RootFileSystem> vfs;
    nvrhi::GraphicsFactoryHandle factory;
    nvrhi::cuda::DeviceHandle cudaDevice;            // OptiX enabled with wantOptix (isOptixAvailable())
    nvrhi::AutoPtr<gvdb::IComputeContext> compute;
    nvrhi::AutoPtr<gvdb::GVDBSceneGraph> sceneGraph;
    // Camera and light leaves under their own nodes of the root (drive them
    // with OrbitController::apply() from GVDBApp.h, then Refresh the graph).
    nvrhi::AutoPtr<donut::engine::PerspectiveCamera> camera;
    nvrhi::AutoPtr<donut::engine::SceneGraphNode> cameraNode;
    nvrhi::AutoPtr<donut::engine::Light> light;
    nvrhi::AutoPtr<donut::engine::SceneGraphNode> lightNode;

    std::filesystem::path getAssetPath(const char *name) const;
    nvrhi::AutoPtr<gvdb::IGVDBVolume> createVolume() const;
    // Submits the recorded work and waits for it.
    void sync() const;
};

// Logs to stdout. Returns false when the CUDA device cannot be created.
bool createHeadlessContext(HeadlessContext &ctx, bool wantOptix = false);

}  // namespace SampleUtils

#endif /* SAMPLE_UTILS_HEADLESSCONTEXT_H */
