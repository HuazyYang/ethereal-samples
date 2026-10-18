// SPDX-License-Identifier: Apache-2.0
#include "GVDBSlangSph.h"
#include "ComputeRuntime.h"
#include <cmath>
#include <stdexcept>
#include <utility>
using nvrhi::BindingSetItem;
namespace gvdb::slang {
SlangSphSimulation::SlangSphSimulation(ethereal::slang::ComputeRuntime& runtime,SphParameters p,
    const std::vector<Float4>& positions,const std::vector<Float4>& velocities):m_runtime(&runtime),m_parameters(p) {
    const auto grid=p.gridResolutionAndCount;
    if(positions.empty() || positions.size()>UINT32_MAX || (!velocities.empty()&&velocities.size()!=positions.size()))
        throw std::invalid_argument("SPH particle streams are empty, oversized or inconsistent");
    if(!grid.x || !grid.y || !grid.z || uint64_t(grid.x)*grid.y>UINT32_MAX || uint64_t(grid.x)*grid.y*grid.z>UINT32_MAX)
        throw std::invalid_argument("SPH grid size is invalid or overflows");
    const Float4 values[]={p.gridMinimumAndCellWidth,p.physical,p.kernels,p.boundaryMinimum,p.boundaryMaximum,p.boundary,p.integration,p.gravityAndSlope,p.wall};
    for(const auto& v:values)if(!std::isfinite(v.x)||!std::isfinite(v.y)||!std::isfinite(v.z)||!std::isfinite(v.w))
        throw std::invalid_argument("SPH parameters must be finite");
    if(p.physical.x<=0 || p.physical.y<=0 || p.physical.z<=0 || p.physical.w<0 ||
        p.gridMinimumAndCellWidth.w<p.physical.x || p.integration.y<0 || p.integration.z<0 || p.integration.w<0 ||
        p.boundary.x<0 || p.boundary.y<0 || p.boundary.z<0 || p.boundary.w<0 ||
        p.boundaryMinimum.x>=p.boundaryMaximum.x || p.boundaryMinimum.y>=p.boundaryMaximum.y || p.boundaryMinimum.z>=p.boundaryMaximum.z)
        throw std::invalid_argument("SPH physical parameters or neighbor-search cell width are invalid");
    if((p.wall.w!=0 && p.wall.w!=1) || (p.wall.w==1 &&
        (p.wall.y<0 || p.wall.z<0 || grid.x<4 || grid.y<4 || grid.z<4)))
        throw std::invalid_argument("SPH wall mode must be 0 (pool) or 1 (FLUIDS-v3), with valid forces and interior grid");
    for(const auto& v:positions)if(!std::isfinite(v.x)||!std::isfinite(v.y)||!std::isfinite(v.z))
        throw std::invalid_argument("SPH initial positions must be finite");
    for(const auto& v:velocities)if(!std::isfinite(v.x)||!std::isfinite(v.y)||!std::isfinite(v.z))
        throw std::invalid_argument("SPH initial velocities must be finite");
    m_count=uint32_t(positions.size());m_cellCount=grid.x*grid.y*grid.z;m_parameters.gridResolutionAndCount.w=m_count;
    std::vector<Float4> zero(m_count,Float4{0,0,0,0});const auto* velocity=velocities.empty()?zero.data():velocities.data();
    const size_t bytes=size_t(m_count)*sizeof(Float4);
    m_positions=runtime.buffer(bytes,sizeof(Float4),positions.data());
    m_velocities=runtime.buffer(bytes,sizeof(Float4),velocity);m_evaluated=runtime.buffer(bytes,sizeof(Float4),velocity);
    m_nextPositions=runtime.buffer(bytes,sizeof(Float4));m_nextVelocities=runtime.buffer(bytes,sizeof(Float4));m_nextEvaluated=runtime.buffer(bytes,sizeof(Float4));
    m_densities=runtime.buffer(size_t(m_count)*4,4);m_forces=runtime.buffer(bytes,sizeof(Float4));
    m_pointCells=runtime.buffer(size_t(m_count)*4,4);m_pointOffsets=runtime.buffer(size_t(m_count)*4,4);
    m_indices=runtime.buffer(size_t(m_count)*4,4);m_cellCounts=runtime.buffer(size_t(m_cellCount)*4,4);
}
void SlangSphSimulation::step(uint32_t count) {
    const uint32_t groups=scanBlockCount(m_count);
    for(uint32_t iteration=0;iteration<count;++iteration) {
        const auto& p=m_parameters;
        m_runtime->dispatch("GVDBSph","clearGrid",&p,sizeof(p),{BindingSetItem::StructuredBuffer_UAV(6,m_cellCounts)},scanBlockCount(m_cellCount));
        m_runtime->dispatch("GVDBSph","insertGrid",&p,sizeof(p),{
            BindingSetItem::StructuredBuffer_SRV(0,m_positions),BindingSetItem::StructuredBuffer_UAV(5,m_pointCells),
            BindingSetItem::StructuredBuffer_UAV(6,m_cellCounts),BindingSetItem::StructuredBuffer_UAV(7,m_pointOffsets)},groups);
        m_cellOffsets=m_runtime->exclusiveScan(m_cellCounts,m_cellCount);
        m_runtime->dispatch("GVDBSph","scatterGrid",&p,sizeof(p),{
            BindingSetItem::StructuredBuffer_SRV(3,m_pointCells),BindingSetItem::StructuredBuffer_SRV(5,m_cellOffsets),
            BindingSetItem::StructuredBuffer_SRV(9,m_pointOffsets),BindingSetItem::StructuredBuffer_UAV(5,m_indices)},groups);
        m_runtime->dispatch("GVDBSph","computeDensity",&p,sizeof(p),{
            BindingSetItem::StructuredBuffer_SRV(0,m_positions),BindingSetItem::StructuredBuffer_SRV(3,m_pointCells),
            BindingSetItem::StructuredBuffer_SRV(4,m_cellCounts),BindingSetItem::StructuredBuffer_SRV(5,m_cellOffsets),
            BindingSetItem::StructuredBuffer_SRV(6,m_indices),BindingSetItem::StructuredBuffer_UAV(0,m_densities)},groups);
        m_runtime->dispatch("GVDBSph","computeForce",&p,sizeof(p),{
            BindingSetItem::StructuredBuffer_SRV(0,m_positions),BindingSetItem::StructuredBuffer_SRV(2,m_evaluated),
            BindingSetItem::StructuredBuffer_SRV(3,m_pointCells),BindingSetItem::StructuredBuffer_SRV(4,m_cellCounts),
            BindingSetItem::StructuredBuffer_SRV(5,m_cellOffsets),BindingSetItem::StructuredBuffer_SRV(6,m_indices),
            BindingSetItem::StructuredBuffer_SRV(7,m_densities),BindingSetItem::StructuredBuffer_UAV(1,m_forces)},groups);
        m_runtime->dispatch("GVDBSph","advanceParticles",&p,sizeof(p),{
            BindingSetItem::StructuredBuffer_SRV(0,m_positions),BindingSetItem::StructuredBuffer_SRV(1,m_velocities),
            BindingSetItem::StructuredBuffer_SRV(2,m_evaluated),BindingSetItem::StructuredBuffer_SRV(3,m_pointCells),
            BindingSetItem::StructuredBuffer_SRV(8,m_forces),BindingSetItem::StructuredBuffer_UAV(2,m_nextPositions),
            BindingSetItem::StructuredBuffer_UAV(3,m_nextVelocities),BindingSetItem::StructuredBuffer_UAV(4,m_nextEvaluated)},groups);
        std::swap(m_positions,m_nextPositions);std::swap(m_velocities,m_nextVelocities);std::swap(m_evaluated,m_nextEvaluated);
        m_parameters.integration.x+=m_parameters.integration.y;
    }
}
std::vector<Float4> SlangSphSimulation::readPositions(){std::vector<Float4> result(m_count);m_runtime->read(m_positions,result.data(),result.size()*sizeof(Float4));return result;}
std::vector<Float4> SlangSphSimulation::readVelocities(){std::vector<Float4> result(m_count);m_runtime->read(m_velocities,result.data(),result.size()*sizeof(Float4));return result;}
} // namespace gvdb::slang
