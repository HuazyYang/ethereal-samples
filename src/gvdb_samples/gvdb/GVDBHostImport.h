// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "GVDBHostVolume.h"
#include "slang/GVDBPortableData.h"
#include <memory>

namespace gvdb::portable {
// CPU-only transactional import. Preserves pool order, IDs, flags and graph.
// Validates offsets, strides, metadata and reciprocal graph/geometry links.
// Padded records are normalized to the 64/8-byte host layout. Bounds are
// recomputed; placement is discarded (atlasInterior becomes {-1,-1,-1}).
// No atlas map or channel data is imported or validated by these functions.
// Failure leaves output untouched. Imported IDs belong to the new owner/epoch.
nvrhi::FRESULT importHostTopology(const PackedHostTopology& source, std::unique_ptr<HostVolume>& output,
                                  HostVolumeLimits limits = {}) noexcept;
nvrhi::FRESULT importHostTopology(const slang::PackedVolume& source, std::unique_ptr<HostVolume>& output,
                                  HostVolumeLimits limits = {}) noexcept;
}  // namespace gvdb::portable
