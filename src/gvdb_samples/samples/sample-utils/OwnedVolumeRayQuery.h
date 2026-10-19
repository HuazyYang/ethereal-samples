// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <gvdb/GVDBSlangRayQuery.h>
#include <gvdb/GVDBNvrhiApi.h>
namespace gvdb::sample {
// Immediate sample query of a nonempty retained raw publication. Caller flushes
// prior producers; ComputeRuntime owns recording/submission and explicit readback.
slang::RayQueryOutput queryOwnedVolumeRays(ethereal::slang::ComputeRuntime&, portable::SnapshotHandle,
                                           nvrhi::IBuffer* origins, nvrhi::IBuffer* directions, uint32_t count,
                                           slang::RayQueryOptions = {}, uint32_t scalarChannel = 0);
}  // namespace gvdb::sample
