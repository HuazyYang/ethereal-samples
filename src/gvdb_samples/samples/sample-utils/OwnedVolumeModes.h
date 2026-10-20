// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "OwnedVolumeRaycast.h"
namespace gvdb::sample {
enum class VolumeMode : uint32_t {
    Off,
    Voxel,
    Trilinear,
    SurfaceDepth,
    Deep,
    LevelSet,
    EmptySkip
};
struct VolumeModeOptions {
    VolumeMode mode = VolumeMode::Trilinear;
    uint32_t maxLeafSteps = 256, maxLeafVisits = 65536;
    float epsilon = .01f;
    float shadowAmount = .8f, shadowBias = 1;
    float extinction = 1, albedo = 1.2f, minimumValue = .005f, alphaCutoff = .01f;
};
// Retained kernel equations, with normalized index-space rays, RGBA8-precision
// color and a separate distance output (infinity miss, -1 traversal exhaustion).
// SurfaceDepth is the original trilinear alias. LevelSet uses strict value<threshold
// and outward gradients; EmptySkip visualizes the biased active-leaf entry position.
// EmptySkip preserves positive coordinate low-byte wrap, with negative values zero.
// Deep uses camera.march.x for sampling, camera.march.z for maximum ray distance,
// and threshold-offset 16384-entry transfer lookup. No implicit GPU completion.
slang::RaycastOutput renderOwnedMode(ethereal::slang::ComputeRuntime&, portable::SnapshotHandle,
                                     uint32_t scalarChannel, uint32_t colorChannel, slang::RaycastParameters,
                                     const std::vector<slang::Float4>& transfer, VolumeModeOptions = {});
}  // namespace gvdb::sample
