// SPDX-License-Identifier: Apache-2.0
#pragma once
// Private same-binary authentication seam. Neither installed nor externally exported.
#include "GVDBNvrhiPointApi.h"
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
} // namespace gvdb::portable::detail
