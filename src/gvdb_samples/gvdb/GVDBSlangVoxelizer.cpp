// SPDX-License-Identifier: Apache-2.0
#include "GVDBSlangVoxelizer.h"
#include "ComputeRuntime.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
using nvrhi::BindingSetItem;
namespace gvdb::slang {
namespace {
void finite(float v){if(!std::isfinite(v))throw std::invalid_argument("GVDB voxelization parameters must be finite");}
uint32_t groups(uint32_t n){return n/256+uint32_t(n%256!=0);}
void dispatch(ethereal::slang::ComputeRuntime& runtime,const char* entry,const VoxelizeParameters& p,
    const std::vector<BindingSetItem>& bindings,uint32_t count){
    const uint32_t n=groups(count);if(n)runtime.dispatch("GVDBVoxelize",entry,&p,sizeof(p),bindings,std::min(n,65535u),n/65535u+uint32_t(n%65535u!=0));
}
}
VoxelizedGrid voxelizeMesh(ethereal::slang::ComputeRuntime& runtime,const std::vector<Float4>& vertices,
    const std::vector<uint32_t>& indices,VoxelizeParameters p,VoxelFormat format) {
    const auto res=p.resolution;
    if(!res.x||!res.y||!res.z||uint64_t(res.x)*res.y>UINT32_MAX||uint64_t(res.x)*res.y*res.z>UINT32_MAX/4||
        vertices.size()>UINT32_MAX/sizeof(Float4)||indices.size()>UINT32_MAX/4||indices.size()%3!=0)
        throw std::invalid_argument("GVDB voxelization dimensions or triangle list are invalid");
    if(format!=VoxelFormat::R8_UINT&&format!=VoxelFormat::R32_FLOAT&&format!=VoxelFormat::R32_SINT)
        throw std::invalid_argument("GVDB voxelization supports R8_UINT, R32_FLOAT, or R32_SINT");
    const Float4 values[]={p.boundsMinimum,p.boundsMaximum,p.values,p.transformRows[0],p.transformRows[1],p.transformRows[2]};
    for(const auto& v:values){finite(v.x);finite(v.y);finite(v.z);finite(v.w);}
    if(!(p.boundsMinimum.x<p.boundsMaximum.x&&p.boundsMinimum.y<p.boundsMaximum.y&&p.boundsMinimum.z<p.boundsMaximum.z))
        throw std::invalid_argument("GVDB voxelization bounds have no positive extent");
    const float fill[]={p.values.x,p.values.y,p.values.z};
    for(float v:fill)if((format==VoxelFormat::R8_UINT&&(v<0||v>255))||
        (format==VoxelFormat::R32_SINT&&(double(v)<INT32_MIN||double(v)>INT32_MAX)))
        throw std::invalid_argument("GVDB voxelization values exceed the output format");
    for(const auto& vertex:vertices){finite(vertex.x);finite(vertex.y);finite(vertex.z);
        for(const auto& row:p.transformRows)finite(vertex.x*row.x+vertex.y*row.y+vertex.z*row.z+row.w);
    }
    for(uint32_t index:indices)if(index>=vertices.size())throw std::invalid_argument("GVDB voxelization triangle index exceeds the vertex buffer");
    p.resolution.w=res.x*res.y*res.z;
    const uint32_t bins=std::min(res.y,65535u*256u);
    p.counts={uint32_t(vertices.size()),uint32_t(indices.size()/3),bins,uint32_t(format)};
    p.values.w=(p.boundsMaximum.y-p.boundsMinimum.y)/float(bins);finite(p.values.w);
    if(p.values.w<=0)throw std::invalid_argument("GVDB voxelization bin width is zero");
    const Float4 zeroVertex{};const uint32_t zero=0;
    auto vertexBuffer=runtime.buffer(std::max(size_t(1),vertices.size())*sizeof(Float4),sizeof(Float4),vertices.empty()?&zeroVertex:vertices.data());
    auto indexBuffer=runtime.buffer(std::max(size_t(1),indices.size())*4,4,indices.empty()?&zero:indices.data());
    auto counts=runtime.buffer(size_t(bins)*4,4);
    const std::vector<BindingSetItem> countBindings={BindingSetItem::StructuredBuffer_SRV(0,vertexBuffer),BindingSetItem::StructuredBuffer_SRV(1,indexBuffer),BindingSetItem::StructuredBuffer_UAV(0,counts)};
    dispatch(runtime,"clearTriangleBins",p,countBindings,bins);
    dispatch(runtime,"countTriangleBins",p,countBindings,p.counts.y);
    std::vector<uint32_t> hostCounts(bins);runtime.read(counts,hostCounts.data(),size_t(bins)*4);
    uint64_t total=0;for(uint32_t count:hostCounts)total+=count;
    if(total>UINT32_MAX/sizeof(Triangle))throw std::invalid_argument("GVDB binned triangle storage exceeds portable buffer addressing");
    auto offsets=runtime.exclusiveScan(counts,bins);
    auto triangles=runtime.buffer(std::max(uint64_t(1),total)*sizeof(Triangle),sizeof(Triangle));
    dispatch(runtime,"clearTriangleBins",p,countBindings,bins);
    dispatch(runtime,"scatterTriangleBins",p,{BindingSetItem::StructuredBuffer_SRV(0,vertexBuffer),BindingSetItem::StructuredBuffer_SRV(1,indexBuffer),
        BindingSetItem::StructuredBuffer_SRV(3,offsets),BindingSetItem::StructuredBuffer_UAV(0,counts),BindingSetItem::StructuredBuffer_UAV(1,triangles)},p.counts.y);
    const size_t bytes=size_t(p.resolution.w)*(format==VoxelFormat::R8_UINT?1:4);
    auto output=runtime.buffer((bytes+3)&~size_t(3),0,nullptr,true);
    dispatch(runtime,"solidVoxelize",p,{BindingSetItem::StructuredBuffer_SRV(2,counts),BindingSetItem::StructuredBuffer_SRV(3,offsets),
        BindingSetItem::StructuredBuffer_SRV(4,triangles),BindingSetItem::RawBuffer_UAV(2,output)},
        format==VoxelFormat::R8_UINT?p.resolution.w/4+uint32_t(p.resolution.w%4!=0):p.resolution.w);
    return {output,p.resolution,format,bytes};
}
} // namespace gvdb::slang
