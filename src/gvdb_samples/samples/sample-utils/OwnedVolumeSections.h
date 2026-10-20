// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "OwnedVolumeRaycast.h"
namespace gvdb::sample {
enum class SectionMode : uint32_t {
    Slice2D,
    Slice3D
};
struct SectionOptions {
    SectionMode mode = SectionMode::Slice3D;
    slang::Float4 point{}, normal{1, 0, 0, 0};
    // In 2D, normal.x/z scale the legacy XZ image coordinates; normal.y is unused.
    float epsilon = .01f;
    uint32_t maxLeafSteps = 256, maxLeafVisits = 65536;
};
// Explicit legacy section equations with a 16,384-entry transfer table. Transfer
// index is trunc(saturate((scalar-isovalue)/(maximum-minimum))*16300).
// 2D uses integer pixel corners, ignores camera/color and composites over black.
// 3D traces beyond a forward plane intersection and blends section over surface.
// Parallel/behind-camera planes leave tracing at the camera (legacy sentinel repair).
// Alpha is always one; RGB is saturated and truncated to RGBA8 precision.
slang::RaycastOutput renderOwnedSection(ethereal::slang::ComputeRuntime&, portable::SnapshotHandle,
                                        uint32_t scalarChannel, uint32_t colorChannel, slang::RaycastParameters,
                                        const std::vector<slang::Float4>& transfer, SectionOptions = {});
// Packed RGBA8, low byte R. Inputs are read-only structured uint buffers; result
// is a fresh buffer. No hidden wait. Count zero returns null. Same-buffer inputs
// are allowed; partial ranges must be uint-aligned and large enough for count.
nvrhi::BufferHandle compositeOverRGBA8(ethereal::slang::ComputeRuntime&, portable::BorrowedBufferBinding source,
                                       portable::BorrowedBufferBinding destination, uint32_t count);
}  // namespace gvdb::sample
