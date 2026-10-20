// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "OwnedVolumeModes.h"
#include "OwnedVolumeSections.h"
#include "OwnedVolumeTricubic.h"
namespace gvdb::sample {
enum class LayerShading : uint32_t {
    Off,
    Voxel,
    Section2D,
    Section3D,
    EmptySkip,
    Trilinear,
    Tricubic,
    LevelSet,
    Deep
};
struct OwnedVolumeLayer {
    portable::SnapshotHandle snapshot;
    uint32_t scalarChannel = 0, colorChannel = UINT32_MAX;
    LayerShading shading = LayerShading::Deep;
    // Caller transforms camera/light/section into this layer's index space.
    // Output width/height and background override the corresponding camera fields.
    slang::RaycastParameters camera{};
    std::vector<slang::Float4> transfer;
    VolumeModeOptions modes;
    SectionOptions section;
    TricubicOptions tricubic;
};
struct LayeredVolumeOutput {
    // Low byte R, then G/B/A. diagnostics is one uint: bit0 nonfinite color,
    // bit1 NaN depth, bit2 negative depth (traversal exhaustion); zero is success.
    nvrhi::BufferHandle rgba8, diagnostics;
    uint32_t width = 0, height = 0;
};
// Scene order, later layers over earlier ones; no depth sorting. Only the first
// layer sees background. Subsequent layers render over transparent black.
// Off first layer clears transparent black; Off later layers are ignored.
// Empty scenes clear the supplied background. Retains legacy per-mode alpha,
// including opaque section/empty-skip misses. No readback or hidden completion.
LayeredVolumeOutput renderOwnedLayers(ethereal::slang::ComputeRuntime&, uint32_t width, uint32_t height,
                                      slang::Float4 background, const std::vector<OwnedVolumeLayer>&);
}  // namespace gvdb::sample
