// SPDX-License-Identifier: Apache-2.0
#include "GVDBSlangVolume.h"
#include "ComputeRuntime.h"
#include <cmath>
#include <stdexcept>
#include <utility>
using nvrhi::BindingSetItem;
namespace gvdb::slang {
namespace {
uint32_t groups8(uint32_t value) { return value/8+uint32_t(value%8!=0); }
void finite(float value) { if(!std::isfinite(value)) throw std::invalid_argument("GVDB requires finite operator parameters"); }
}
SlangVolume::SlangVolume(ethereal::slang::ComputeRuntime& runtime,const PackedVolume& volume,
    const float* atlas,size_t voxelCount):m_runtime(&runtime),m_volume(volume.volume),m_voxelCount(voxelCount) {
    const auto res=m_volume.atlasResolutionAndApron;
    if(volume.levels.empty() || volume.nodes.empty() || volume.atlasMap.empty() || !atlas ||
        voxelCount!=uint64_t(res.x)*res.y*res.z || voxelCount>UINT32_MAX)
        throw std::invalid_argument("GVDB Slang upload requires a packed hierarchy and a complete scalar atlas");
    m_nodes=runtime.buffer(volume.nodes.size()*4,0,volume.nodes.data(),true);
    const uint32_t emptyChildren[2]={UINT32_MAX,UINT32_MAX};
    m_children=runtime.buffer(volume.children.empty()?sizeof(emptyChildren):volume.children.size()*4,
        0,volume.children.empty()?emptyChildren:volume.children.data(),true);
    m_levels=runtime.buffer(volume.levels.size()*sizeof(LevelInfo),sizeof(LevelInfo),volume.levels.data());
    m_atlasMap=runtime.buffer(volume.atlasMap.size()*sizeof(AtlasNode),sizeof(AtlasNode),volume.atlasMap.data());
    m_atlas=runtime.buffer(voxelCount*sizeof(float),sizeof(float),atlas);
    m_scratch=runtime.buffer(voxelCount*sizeof(float),sizeof(float));
}
void SlangVolume::operate(const char* entry,OperatorParameters p,nvrhi::IBuffer* source) {
    p.volume=m_volume;
    const auto res=m_volume.atlasResolutionAndApron;
    m_runtime->dispatch("GVDBOperators",entry,&p,sizeof(p),{
        BindingSetItem::StructuredBuffer_SRV(0,source?source:m_atlas.Get()),BindingSetItem::StructuredBuffer_SRV(1,m_atlasMap),
        BindingSetItem::RawBuffer_SRV(2,m_nodes),BindingSetItem::RawBuffer_SRV(3,m_children),
        BindingSetItem::StructuredBuffer_SRV(4,m_levels),BindingSetItem::StructuredBuffer_UAV(0,m_scratch)},
        groups8(res.x),groups8(res.y),groups8(res.z));
    std::swap(m_atlas,m_scratch);
}
void SlangVolume::clear() { operate("clearAtlas",{}); }
void SlangVolume::fill(float value) {
    finite(value);OperatorParameters p{};p.value={value,0,1,0};operate("fillAtlas",p);
}
void SlangVolume::updateApron(float boundary) {
    finite(boundary);OperatorParameters p{};p.value.x=boundary;operate("updateApron",p);
}
void SlangVolume::smooth(float centerWeight,float bias,uint32_t iterations,float boundary) {
    finite(centerWeight);finite(bias);finite(boundary);
    if(centerWeight==-6)throw std::invalid_argument("GVDB smoothing denominator is zero");
    OperatorParameters p{};p.value={centerWeight,bias,0,0};
    for(uint32_t i=0;i<iterations;++i) {updateApron(boundary);operate("smoothAtlas",p);}
    if(iterations)updateApron(boundary);
}
void SlangVolume::grow(float amount,uint32_t iterations) {
    finite(amount);OperatorParameters p{};p.value.x=amount;
    for(uint32_t i=0;i<iterations;++i)operate("growAtlas",p);
}
void SlangVolume::noise(float amplitude) {
    finite(amplitude);OperatorParameters p{};p.value.x=amplitude;operate("noiseAtlas",p);
}
namespace {
void validateDenseSource(nvrhi::IBuffer* source,UInt4 resolution) {
    if(!source||source->getDesc().structStride!=4||!resolution.x||!resolution.y||!resolution.z||
        uint64_t(resolution.x)*resolution.y>UINT32_MAX||uint64_t(resolution.x)*resolution.y*resolution.z>UINT32_MAX/4||
        source->getDesc().byteSize<uint64_t(resolution.x)*resolution.y*resolution.z*4)
        throw std::invalid_argument("GVDB dense source requires a complete structured float buffer");
}
void validateTransform(const std::array<Float4,3>& transform,Float4 remap) {
    for(const auto& row:transform){finite(row.x);finite(row.y);finite(row.z);finite(row.w);}
    finite(remap.x);finite(remap.y);finite(remap.z);finite(remap.w);
    if(remap.x==remap.y)throw std::invalid_argument("GVDB resample input range is empty");
}
void validateCoordinates(const std::array<Float4,3>& transform,Float4 lower,Float4 upper) {
    for(unsigned corner=0;corner<8;++corner)for(const auto& row:transform) {
        const double value=double(row.x)*((corner&1)?upper.x:lower.x)+double(row.y)*((corner&2)?upper.y:lower.y)+double(row.z)*((corner&4)?upper.z:lower.z)+row.w;
        if(!std::isfinite(value)||value< -2147483000.0||value>2147483000.0)
            throw std::invalid_argument("GVDB resample coordinates exceed the shader integer range");
    }
}
}
void SlangVolume::cut(){operate("cutAtlas",{});}
void SlangVolume::resample(nvrhi::IBuffer* source,UInt4 resolution,const std::array<Float4,3>& transform,Float4 remap,float background) {
    validateDenseSource(source,resolution);validateTransform(transform,remap);finite(background);
    auto low=m_volume.boundsMin,high=m_volume.boundsMax;
    const float apron=float(m_volume.atlasResolutionAndApron.w);low.x-=apron;low.y-=apron;low.z-=apron;high.x+=apron;high.y+=apron;high.z+=apron;
    validateCoordinates(transform,low,high);
    nvrhi::BufferHandle retainedSource=source;
    if(source==m_scratch.Get())m_scratch=m_runtime->buffer(m_voxelCount*4,4);
    OperatorParameters p{};p.sourceResolution=resolution;p.remap=remap;p.value.x=background;
    for(size_t i=0;i<3;++i)p.transformRows[i]=transform[i];operate("resampleAtlas",p,source);
}
float SlangVolume::interiorSum() {
    const auto grid=m_volume.atlasBrickCountAndWidth;
    const uint32_t width=grid.w-2*m_volume.atlasResolutionAndApron.w;
    const uint64_t x=uint64_t(grid.x)*width,y=uint64_t(grid.y)*width,count=x*y;
    if(x>65535u*8u||y>65535u*8u||count>65535u*256u)
        throw std::invalid_argument("GVDB interior reduction exceeds the portable dispatch range");
    OperatorParameters p{};p.volume=m_volume;
    auto columns=m_runtime->buffer(size_t(count)*4,4);
    m_runtime->dispatch("GVDBOperators","reduceAtlasColumns",&p,sizeof(p),{
        BindingSetItem::StructuredBuffer_SRV(0,m_atlas),BindingSetItem::StructuredBuffer_UAV(0,columns)},groups8(uint32_t(x)),groups8(uint32_t(y)),1);
    auto reduction=m_runtime->reduce(columns,uint32_t(count));Reduction result{};m_runtime->read(reduction,&result,sizeof(result));return result.sum;
}
std::vector<float> downsampleDense(ethereal::slang::ComputeRuntime& runtime,nvrhi::IBuffer* source,
    UInt4 sourceResolution,UInt4 destinationResolution,const std::array<Float4,3>& transform,Float4 remap) {
    validateDenseSource(source,sourceResolution);validateTransform(transform,remap);
    const auto d=destinationResolution;
    if(!d.x||!d.y||!d.z||d.x>65535u*8u||d.y>65535u*8u||d.z>65535u*8u||
        uint64_t(d.x)*d.y>UINT32_MAX||uint64_t(d.x)*d.y*d.z>UINT32_MAX/4)
        throw std::invalid_argument("GVDB downsample destination exceeds the portable range");
    if(transform[0].x<0||transform[1].y<0||transform[2].z<0||transform[0].y!=0||transform[0].z!=0||
        transform[1].x!=0||transform[1].z!=0||transform[2].x!=0||transform[2].y!=0)
        throw std::invalid_argument("GVDB downsample requires an axis-aligned nonnegative scale");
    validateCoordinates(transform,{0,0,0,0},{float(d.x),float(d.y),float(d.z),0});
    OperatorParameters p{};p.volume.atlasResolutionAndApron=d;p.sourceResolution=sourceResolution;p.remap=remap;
    for(size_t i=0;i<3;++i)p.transformRows[i]=transform[i];
    const size_t count=size_t(d.x)*d.y*d.z;auto output=runtime.buffer(count*4,4);
    runtime.dispatch("GVDBOperators","downsampleDense",&p,sizeof(p),{
        BindingSetItem::StructuredBuffer_SRV(0,source),BindingSetItem::StructuredBuffer_UAV(0,output)},groups8(d.x),groups8(d.y),groups8(d.z));
    std::vector<float> result(count);runtime.read(output,result.data(),count*4);return result;
}
std::vector<float> downsampleRegion(ethereal::slang::ComputeRuntime& runtime,nvrhi::IBuffer* source,
    UInt4 sourceResolution,UInt4 destinationResolution,Float4 destinationExtent,
    const std::array<Float4,3>& transform,Float4 remap) {
    validateDenseSource(source,sourceResolution);validateTransform(transform,remap);
    const auto d=destinationResolution;
    finite(destinationExtent.x);finite(destinationExtent.y);finite(destinationExtent.z);
    if(destinationExtent.x<=0||destinationExtent.y<=0||destinationExtent.z<=0||
        !d.x||!d.y||!d.z||d.x>65535u*8u||d.y>65535u*8u||d.z>65535u*8u||
        uint64_t(d.x)*d.y>UINT32_MAX||uint64_t(d.x)*d.y*d.z>UINT32_MAX/4)
        throw std::invalid_argument("GVDB region downsample dimensions exceed the portable range");
    validateCoordinates(transform,{-1,-1,-1,0},destinationExtent);
    OperatorParameters p{};p.volume.atlasResolutionAndApron=d;p.sourceResolution=sourceResolution;p.remap=remap;p.value=destinationExtent;
    for(size_t i=0;i<3;++i)p.transformRows[i]=transform[i];
    const size_t count=size_t(d.x)*d.y*d.z;auto output=runtime.buffer(count*4,4);
    runtime.dispatch("GVDBResample","downsampleRegion",&p,sizeof(p),{
        BindingSetItem::StructuredBuffer_SRV(0,source),BindingSetItem::StructuredBuffer_UAV(0,output)},groups8(d.x),groups8(d.y),groups8(d.z));
    std::vector<float> result(count);runtime.read(output,result.data(),count*4);return result;
}
void validateRaycastParameters(const RaycastParameters& p,const std::vector<Float4>& transfer) {
    if(!p.image.x || !p.image.y || uint64_t(p.image.x)*p.image.y>UINT32_MAX || !p.image.w || transfer.empty() || transfer.size()>UINT32_MAX)
        throw std::invalid_argument("GVDB raycast dimensions or transfer function are invalid");
    finite(p.march.x);finite(p.march.y);finite(p.march.z);finite(p.march.w);
    finite(p.valueRange.x);finite(p.valueRange.y);finite(p.valueRange.z);finite(p.valueRange.w);
    if(p.march.x<=0 || p.march.y>p.march.z || p.march.w<0 || p.valueRange.x==p.valueRange.y || p.valueRange.w<=0 || p.valueRange.w>1)
        throw std::invalid_argument("GVDB raycast range, step or extinction is invalid");
    if(p.direction00.w!=0&&p.direction00.w!=1)
        throw std::invalid_argument("GVDB raycast clip mode must be zero or one");
    const Float4 vectors[]={p.rayOrigin,p.direction00,p.directionDx,p.directionDy,p.background,p.light};
    for(const auto& v:vectors) {finite(v.x);finite(v.y);finite(v.z);finite(v.w);}
    for(const auto& value:transfer) {
        finite(value.x);finite(value.y);finite(value.z);finite(value.w);
        if(value.w<0)throw std::invalid_argument("GVDB transfer extinction must be nonnegative");
    }
}
RaycastOutput SlangVolume::raycast(RaycastParameters p,const std::vector<Float4>& transfer,bool surface) {
    validateRaycastParameters(p,transfer);
    p.volume=m_volume;p.image.z=uint32_t(transfer.size());
    auto transferBuffer=m_runtime->buffer(transfer.size()*sizeof(Float4),sizeof(Float4),transfer.data());
    RaycastOutput result;
    result.width=p.image.x;result.height=p.image.y;
    result.color=m_runtime->buffer(size_t(p.image.x)*p.image.y*sizeof(Float4),sizeof(Float4));
    result.depth=m_runtime->buffer(size_t(p.image.x)*p.image.y*sizeof(float),sizeof(float));
    m_runtime->dispatch("GVDBRaycast",surface?"raycastSurface":"raycastVolume",&p,sizeof(p),{
        BindingSetItem::StructuredBuffer_SRV(0,m_atlas),BindingSetItem::RawBuffer_SRV(1,m_nodes),
        BindingSetItem::RawBuffer_SRV(2,m_children),BindingSetItem::StructuredBuffer_SRV(3,m_levels),
        BindingSetItem::StructuredBuffer_SRV(4,transferBuffer),BindingSetItem::StructuredBuffer_UAV(0,result.color),
        BindingSetItem::StructuredBuffer_UAV(1,result.depth)},groups8(p.image.x),groups8(p.image.y),1);
    return result;
}
std::vector<float> SlangVolume::readAtlas() {
    std::vector<float> result(m_voxelCount);m_runtime->read(m_atlas,result.data(),result.size()*sizeof(float));return result;
}
} // namespace gvdb::slang
