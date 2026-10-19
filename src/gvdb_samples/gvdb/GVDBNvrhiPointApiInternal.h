// SPDX-License-Identifier: Apache-2.0
#pragma once
// Private same-binary authentication seam. Neither installed nor externally exported.
#include "GVDBNvrhiPointApi.h"
#include <vector>
#include <array>
namespace gvdb::portable::detail {
struct ResolvedPointSet {
    // Owns all CPU borrows and canonical GPU buffers below. Read-only data remains
    // coherent after the original point factory and publishing volume are released.
    PointSetHandle retained;
    PointSetInfo info;
    BorrowedBytes positions, colors;
    BorrowedPointStream positionStream, colorStream;
};
// Only this binary's concrete PointSet on the specified device is accepted. No
// readback/wait/allocation or external-buffer adoption. Absent colors remain empty.
// Failure preserves output; a successful view retains the immutable PointSet.
nvrhi::FRESULT resolvePointSet(IGVDBPointSet*,nvrhi::IDevice*,ResolvedPointSet& output) noexcept;
// Trusted producer seam: positions are the completed GPU result in the original
// Float3/Float4 layout. Copies auxiliary CPU attributes and retains their buffers.
// Revalidates finite XYZ and computes exact bounds; never submits or waits.
nvrhi::FRESULT clonePointSetPositions(IGVDBPointSet*,nvrhi::IDevice*,nvrhi::IBuffer*,
    std::vector<uint8_t> positions,IGVDBPointSet** output) noexcept;
// Same trusted seam for either canonical float attribute. Velocity replacement
// preserves position bounds; all selected XYZ values must be finite.
nvrhi::FRESULT clonePointSetFloatAttribute(IGVDBPointSet*,nvrhi::IDevice*,PointAttribute,
    nvrhi::IBuffer*,std::vector<uint8_t> values,IGVDBPointSet** output) noexcept;
// Trusted GPU expansion producer: preserves source attribute layouts/mask but uses
// a new count and actual completed GPU bytes. Recomputes finite position bounds.
nvrhi::FRESULT createExpandedPointSet(IGVDBPointSet*,nvrhi::IDevice*,uint64_t count,
    const std::array<nvrhi::BufferHandle,3>&,std::array<std::vector<uint8_t>,3> values,
    IGVDBPointSet** output) noexcept;
} // namespace gvdb::portable::detail
