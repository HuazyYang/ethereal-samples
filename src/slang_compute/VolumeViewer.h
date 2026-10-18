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
    uint32_t frameLimit = 0;           // Zero keeps an interactive window open.
    std::filesystem::path screenshot;  // Optional final compute image at frameLimit.
    float step = .5f, extinction = 1.5f, isovalue = .1f, valueMin = 0, valueMax = 1;
    float yaw = .52f, pitch = .52f, distanceScale = 2.8f, sectionHeight = .5f;
    VolumeViewMode mode = VolumeViewMode::Volume;
    bool topology = false, sectionInset = false;
    bool explicitCamera = false;
    bool requestRayTracing = false;
    bool showUI = false;  // Windowed Inspector only; captures contain the rendered scene.
    // Explicit perspective near/far are view-space Z planes, in index units.
    float horizontalFov = 50, cameraNear = 1, cameraFar = 100000;
    float scalarBackground = 0;
    slang::Float4 cameraEye{0, 0, 1, 0}, cameraForward{0, 0, -1, 0}, cameraUp{0, 1, 0, 0};
    slang::Float4 background{.1f, .2f, .4f, 1};
    slang::Float4 targetOffset{0, 0, 0, 0}, lightDirection{-1, 2, -1, 0};
};
// Primitives composite in vector order with source-alpha blending (alpha clamped to [0,1]).
// Lines are drawn before points; these screen overlays do not test scene depth.
struct VolumeOverlayLine {
    slang::Float4 start, end, color;
};
struct VolumeOverlayPoint {
    slang::Float4 position, color;
};
class VolumeScene {
 public:
    virtual ~VolumeScene() = default;
    virtual slang::SlangVolume& volume() = 0;
    virtual const slang::PackedVolume& packed() const = 0;
    virtual std::vector<slang::Float4> transfer() const = 0;
    virtual void animate(float, VolumeViewOptions&) {}
    // Called inside the windowed Inspector with an active ImGui context.
    // Controls affect the next frame; headless rendering never calls this hook.
    virtual void buildUI(VolumeViewOptions&) {}
    // Optional replacement for volume/surface rendering. Empty output selects the
    // standard renderer; a replacement must match the requested dimensions and
    // provide float4 color plus float depth buffers. Presentation/overlays stay shared.
    virtual slang::RaycastOutput renderCustom(const slang::RaycastParameters&, const VolumeViewOptions&) {
        return {};
    }
    virtual nvrhi::IBuffer* colorAtlas() const { return nullptr; }
    // Section2D projects these debug primitives orthographically onto X/Z.
    virtual std::vector<VolumeOverlayLine> overlayLines() const { return {}; }
    virtual std::vector<VolumeOverlayPoint> overlayPoints() const { return {}; }
    // Return true when a scene consumes an input before the viewer defaults.
    virtual bool mouseDrag(int, float, float, int, VolumeViewOptions&) { return false; }
    virtual bool mouseScroll(float, VolumeViewOptions&) { return false; }
    virtual bool key(int) { return false; }
    virtual bool key(int value, VolumeViewOptions&) { return key(value); }
    // Actions are press-only unless a scene opts into repeating a continuous control.
    virtual bool acceptsKeyRepeat(int) const { return false; }
    bool keyEvent(int value, bool repeat, VolumeViewOptions& options) {
        return (!repeat || acceptsKeyRepeat(value)) && key(value, options);
    }
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
