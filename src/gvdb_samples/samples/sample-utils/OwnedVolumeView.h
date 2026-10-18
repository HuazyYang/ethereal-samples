// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "OwnedVolumeRaycast.h"
namespace gvdb::sample {
enum class OwnedVolumeViewMode : uint32_t { Voxel, Section, Section2D };
// Record into the app ComputeRuntime, with no submit/wait. Canonical raw channel
// and topology bindings come from one snapshot and remain alive through execution.
slang::RaycastOutput renderOwnedVolumeView(ethereal::slang::ComputeRuntime&,
    portable::SnapshotHandle, uint32_t channel, slang::RaycastParameters,
    const std::vector<slang::Float4>& transfer, OwnedVolumeViewMode, float sectionY);
// Composite a section into an existing same-device color/depth output. Only the
// bounded rectangle is written. The caller owns submission and output lifetime.
void drawOwnedVolumeSection(ethereal::slang::ComputeRuntime&, portable::SnapshotHandle,
    uint32_t channel, slang::RaycastParameters, const std::vector<slang::Float4>& transfer,
    float sectionY, slang::UInt4 rectangle, const slang::RaycastOutput& output);
} // namespace gvdb::sample
