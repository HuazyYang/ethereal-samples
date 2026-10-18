// Sparse topology discovery built from the public NVRHI compute runtime.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "slang/GVDBPortable.h"
#include <nvrhi/nvrhi.h>
#include <vector>
namespace ethereal::slang { class ComputeRuntime; }
namespace gvdb::slang {
struct SlangBrickKeys {
    nvrhi::BufferHandle keys;
    uint32_t count=0;
    std::vector<uint32_t> levelCounts;
    bool hasInvalidPositions=false;
};
// Positions are float4, in index space after translation. The returned sorted
// unique keys preserve the first original particle index. Invalid/nonfinite or
// out-of-range (16-bit coordinate) positions produce one sentinel key at end.
SlangBrickKeys discoverBrickKeys(ethereal::slang::ComputeRuntime& runtime,nvrhi::IBuffer* positions,
    uint32_t pointCount,const std::vector<uint32_t>& levelRanges,Float4 translation={});
} // namespace gvdb::slang
