// SPDX-License-Identifier: Apache-2.0
#include "VolumeSurface.h"
#include <cmath>
#include <stdexcept>
namespace gvdb::sample {
namespace {
struct alignas(16) SurfaceParameters { slang::RaycastParameters ray;slang::Float4 options; };
static_assert(sizeof(SurfaceParameters)==240);
}
slang::RaycastOutput renderVolumeSurface(ethereal::slang::ComputeRuntime& runtime,
    slang::SlangVolume& volume,const slang::RaycastParameters& parameters,
    const std::vector<slang::Float4>& transfer,nvrhi::IBuffer* packedColorAtlas,float scalarBackground) {
    using nvrhi::BindingSetItem;
    slang::validateRaycastParameters(parameters,transfer);
    if(!std::isfinite(scalarBackground))throw std::invalid_argument("Surface scalar background must be finite");
    if(parameters.light.w<0||parameters.light.w>1)throw std::invalid_argument("Surface ambient light must be in [0,1]");
    if(parameters.image.x>65535u*8||parameters.image.y>65535u*8)
        throw std::invalid_argument("Surface image exceeds portable dispatch dimensions");
    const auto atlasBytes=volume.atlas()->getDesc().byteSize;
    if(packedColorAtlas&&(!packedColorAtlas->getDesc().canHaveRawViews||
        packedColorAtlas->getDesc().byteSize<atlasBytes||atlasBytes>UINT32_MAX))
        throw std::invalid_argument("Surface packed color requires a raw RGBA8 buffer covering the scalar atlas");
    SurfaceParameters p{parameters,{scalarBackground,packedColorAtlas?1.f:0.f,0,0}};
    p.ray.volume=volume.info();p.ray.image.z=uint32_t(transfer.size());
    auto transferBuffer=runtime.buffer(transfer.size()*sizeof(slang::Float4),sizeof(slang::Float4),transfer.data());
    nvrhi::BufferHandle emptyColor;
    if(!packedColorAtlas){const uint32_t zero=0;emptyColor=runtime.buffer(4,0,&zero,true);packedColorAtlas=emptyColor;}
    slang::RaycastOutput result;result.width=p.ray.image.x;result.height=p.ray.image.y;
    const size_t pixels=size_t(result.width)*result.height;
    result.color=runtime.buffer(pixels*sizeof(slang::Float4),sizeof(slang::Float4));
    result.depth=runtime.buffer(pixels*sizeof(float),sizeof(float));
    runtime.dispatch("VolumeSurface","renderSurface",&p,sizeof(p),{
        BindingSetItem::StructuredBuffer_SRV(0,volume.atlas()),BindingSetItem::RawBuffer_SRV(1,volume.nodes()),
        BindingSetItem::RawBuffer_SRV(2,volume.children()),BindingSetItem::StructuredBuffer_SRV(3,volume.levels()),
        BindingSetItem::StructuredBuffer_SRV(4,transferBuffer),BindingSetItem::RawBuffer_SRV(5,packedColorAtlas),
        BindingSetItem::StructuredBuffer_UAV(0,result.color),BindingSetItem::StructuredBuffer_UAV(1,result.depth)},
        (result.width+7)/8,(result.height+7)/8,1);
    return result;
}
} // namespace gvdb::sample
