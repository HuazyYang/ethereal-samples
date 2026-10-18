// SPDX-License-Identifier: Apache-2.0
#include "GVDBSlangPointFusion.h"
#include "ComputeRuntime.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>
using nvrhi::BindingSetItem;
namespace gvdb::slang {
SlangPointFusion::SlangPointFusion(ethereal::slang::ComputeRuntime& runtime,UInt4 resolution,
    const std::vector<GatherCell>& cells,nvrhi::IBuffer* positions,uint32_t pointCount,nvrhi::IBuffer* colors)
    :m_runtime(&runtime),m_resolution(resolution),m_colors(colors!=nullptr) {
    if(!resolution.x||!resolution.y||!resolution.z||uint64_t(resolution.x)*resolution.y>UINT32_MAX||
        uint64_t(resolution.x)*resolution.y*resolution.z>UINT32_MAX/4||cells.size()>UINT32_MAX/sizeof(GatherCell)||
        !positions||positions->getDesc().byteSize<uint64_t(pointCount)*sizeof(Float4)||
        (colors&&colors->getDesc().byteSize<uint64_t(pointCount)*4))
        throw std::invalid_argument("GVDB gather dimensions, cells, or point buffers exceed the portable range");
    m_voxels=resolution.x*resolution.y*resolution.z;m_cellCount=uint32_t(cells.size());
    std::vector<std::pair<uint32_t,uint32_t>> rows;
    for(const auto& cell:cells) {
        const auto a=cell.atlasOrigin;const auto w=cell.worldOriginAndWidth;const uint32_t width=uint32_t(w.w);
        if(w.w<1||w.w>8||uint64_t(a.x)+width>resolution.x||uint64_t(a.y)+width>resolution.y||uint64_t(a.z)+width>resolution.z||
            uint64_t(cell.points.x)+cell.points.y>pointCount||int64_t(w.x)+width>INT32_MAX||int64_t(w.y)+width>INT32_MAX||int64_t(w.z)+width>INT32_MAX)
            throw std::invalid_argument("GVDB gather cell has an invalid spatial or particle range");
        for(uint32_t z=0;z<width;++z)for(uint32_t y=0;y<width;++y){
            const uint32_t begin=((a.z+z)*resolution.y+a.y+y)*resolution.x+a.x;
            rows.emplace_back(begin,begin+width);
        }
    }
    std::sort(rows.begin(),rows.end());
    for(size_t i=1;i<rows.size();++i)if(rows[i].first<rows[i-1].second)
        throw std::invalid_argument("GVDB gather cells overlap and would race");
    const GatherCell empty{};
    m_cells=runtime.buffer(std::max(size_t(1),cells.size())*sizeof(GatherCell),sizeof(GatherCell),cells.empty()?&empty:cells.data());
    m_positions=positions;const uint32_t zero=0;m_pointColors=colors?colors:runtime.buffer(4,4,&zero).Get();
    std::vector<uint32_t> initial(m_voxels,0);
    m_density=runtime.buffer(size_t(m_voxels)*4,4,initial.data());m_color=runtime.buffer(size_t(m_voxels)*4,4,initial.data());
    m_nextDensity=runtime.buffer(size_t(m_voxels)*4,4);m_nextColor=runtime.buffer(size_t(m_voxels)*4,4);
}
void SlangPointFusion::gather(float radius,bool levelSet,bool accumulate) {
    if(!std::isfinite(radius)||radius<=0)throw std::invalid_argument("GVDB gather radius must be finite and positive");
    GatherParameters p{{m_cellCount,m_voxels,m_colors?1u:0u,accumulate?1u:0u},{radius,levelSet?3.f:0.f,0,0},m_resolution};
    const std::vector<BindingSetItem> bindings={BindingSetItem::StructuredBuffer_SRV(0,m_positions),
        BindingSetItem::StructuredBuffer_SRV(1,m_pointColors),BindingSetItem::StructuredBuffer_SRV(2,m_cells),
        BindingSetItem::StructuredBuffer_SRV(3,m_density),BindingSetItem::StructuredBuffer_SRV(4,m_color),
        BindingSetItem::StructuredBuffer_UAV(0,m_nextDensity),BindingSetItem::StructuredBuffer_UAV(1,m_nextColor)};
    const uint32_t groups=m_voxels/256+uint32_t(m_voxels%256!=0);
    m_runtime->dispatch("GVDBGather","initializeGather",&p,sizeof(p),bindings,std::min(groups,65535u),groups/65535u+uint32_t(groups%65535u!=0));
    if(m_cellCount)m_runtime->dispatch("GVDBGather",levelSet?"gatherLevelSet":"gatherDensity",&p,sizeof(p),bindings,
        std::min(m_cellCount,65535u),m_cellCount/65535u+uint32_t(m_cellCount%65535u!=0));
    std::swap(m_density,m_nextDensity);std::swap(m_color,m_nextColor);
}
std::vector<float> SlangPointFusion::readDensity(){std::vector<float> result(m_voxels);m_runtime->read(m_density,result.data(),result.size()*4);return result;}
std::vector<uint32_t> SlangPointFusion::readColor(){std::vector<uint32_t> result(m_voxels);m_runtime->read(m_color,result.data(),result.size()*4);return result;}
} // namespace gvdb::slang
