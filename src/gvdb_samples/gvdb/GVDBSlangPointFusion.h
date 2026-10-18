// NVRHI particle-to-volume subcell gathers with deterministic voxel ownership.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "slang/GVDBPortable.h"
#include <nvrhi/nvrhi.h>
#include <vector>
namespace ethereal::slang {class ComputeRuntime;}
namespace gvdb::slang {
class SlangPointFusion {
    ethereal::slang::ComputeRuntime* m_runtime;
    UInt4 m_resolution;
    uint32_t m_voxels,m_cellCount;
    bool m_colors;
    nvrhi::BufferHandle m_cells,m_positions,m_pointColors,m_density,m_color,m_nextDensity,m_nextColor;
public:
    // Cells must not overlap and must describe ranges into the supplied point
    // buffers. Point coordinates are already in volume index space, float4 stride.
    SlangPointFusion(ethereal::slang::ComputeRuntime& runtime,UInt4 atlasResolution,
        const std::vector<GatherCell>& cells,nvrhi::IBuffer* positions,uint32_t pointCount,nvrhi::IBuffer* colors=nullptr);
    void gather(float radius,bool levelSet=false,bool accumulate=false);
    nvrhi::IBuffer* density()const{return m_density;}
    nvrhi::IBuffer* color()const{return m_color;}
    std::vector<float> readDensity();
    std::vector<uint32_t> readColor();
};
} // namespace gvdb::slang
