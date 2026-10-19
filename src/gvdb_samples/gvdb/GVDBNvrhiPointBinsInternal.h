// SPDX-License-Identifier: Apache-2.0
#pragma once
// Shared private stable support bins for gather and public subcell expansion.
#include "GVDBNvrhiGatherApi.h"
#include "GVDBNvrhiPointApiInternal.h"
#include "slang/GVDBPortableData.h"
#include <vector>
namespace gvdb::portable::detail {
struct PointBins {
    std::vector<slang::UInt2> cells;
    std::vector<uint32_t> indices;  // One dummy element when no pairs; ranges remain zero.
    uint32_t side = 0, perLeaf = 0;
};
nvrhi::FRESULT buildPointBins(const slang::PackedVolume&, const GatherRequestDesc&,
                              const GatherPreparationBudget&, const ResolvedPointSet&,
                              PointBins& output) noexcept;
}  // namespace gvdb::portable::detail
