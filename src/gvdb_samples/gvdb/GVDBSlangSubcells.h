// GPU sparse subcell pair binning for SlangPointFusion.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "GVDBSlangPointFusion.h"
#include "slang/GVDBPortableData.h"
namespace gvdb::slang {
struct SlangPointBins {
    std::vector<GatherCell> cells;
    nvrhi::BufferHandle positions,colors;
    uint32_t pointCount=0;
};
// Expands each point to every existing subcell intersecting its support box,
// following the legacy [point-radius,point+1+radius] coverage convention.
// Output points already include translation. Cell ordering is leaf/local-cell;
// point ordering within each cell is unspecified because insertion is atomic.
SlangPointBins binPointSubcells(ethereal::slang::ComputeRuntime& runtime,const PackedVolume& volume,
    nvrhi::IBuffer* positions,uint32_t count,uint32_t subcellWidth,float radius,
    Float4 translation={},nvrhi::IBuffer* colors=nullptr);
} // namespace gvdb::slang
