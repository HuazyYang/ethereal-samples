// Shared portable GVDB display and controls. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ComputeRuntime.h"
#include "../gvdb_samples/gvdb/GVDBSlangVolume.h"
#include <functional>
#include <memory>
#include <string>

namespace gvdb::sample {
enum class VolumeViewMode : uint32_t {
    Volume,
    Surface,
    Voxel,
    Section,
    Section2D
};
struct VolumeViewOptions {
    uint32_t width = 1024, height = 768;
    float step = .5f, extinction = 1.5f, isovalue = .1f, valueMin = 0, valueMax = 1;
    float yaw = .52f, pitch = .52f, distanceScale = 2.8f, sectionHeight = .5f;
    VolumeViewMode mode = VolumeViewMode::Volume;
    bool topology = false, sectionInset = false;
    slang::Float4 background{.1f, .2f, .4f, 1};
    slang::Float4 targetOffset{0, 0, 0, 0}, lightDirection{-1, 2, -1, 0};
};
class VolumeScene {
 public:
    virtual ~VolumeScene() = default;
    virtual slang::SlangVolume& volume() = 0;
    virtual const slang::PackedVolume& packed() const = 0;
    virtual std::vector<slang::Float4> transfer() const = 0;
    virtual bool key(int) { return false; }
    virtual bool key(int value, VolumeViewOptions&) { return key(value); }
    virtual std::string describe() const { return {}; }
};
using SceneFactory = std::function<std::unique_ptr<VolumeScene>(ethereal::slang::ComputeRuntime&)>;
slang::RaycastOutput renderVolumeView(ethereal::slang::ComputeRuntime& runtime, VolumeScene& scene,
                                      const VolumeViewOptions& options);
void writeVolumeImage(ethereal::slang::ComputeRuntime& runtime, const slang::RaycastOutput& output,
                      const std::filesystem::path& path);
// Exercises the same direct-buffer graphics presenter without opening a window.
std::vector<uint8_t> renderVolumeOffscreen(int argc, const char** argv, const SceneFactory& factory,
                                           VolumeViewOptions options = {});
int runVolumeWindow(int argc, const char** argv, const char* title, const SceneFactory& factory,
                    VolumeViewOptions options = {});
}  // namespace gvdb::sample
