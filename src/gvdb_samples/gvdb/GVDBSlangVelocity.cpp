// SPDX-License-Identifier: Apache-2.0
#include "GVDBSlangVelocity.h"
#include "ComputeRuntime.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
using nvrhi::BindingSetItem;
namespace gvdb::slang {
VelocityTransfer transferGridVelocity(ethereal::slang::ComputeRuntime& runtime,const SlangVolume& volume,
    const std::array<nvrhi::IBuffer*,3>& current,const std::array<nvrhi::IBuffer*,3>& previous,
    nvrhi::IBuffer* positions,nvrhi::IBuffer* velocities,uint32_t count,float timeStep,float flipWeight,bool advect) {
    if(!std::isfinite(timeStep)||timeStep<0||!std::isfinite(flipWeight)||flipWeight<0||flipWeight>1||count>UINT32_MAX/16)
        throw std::invalid_argument("GVDB velocity transfer count or integration parameters are invalid");
    if(!count)return {};
    const size_t bytes=size_t(count)*16;
    if(!positions||!velocities||positions->getDesc().structStride!=16||velocities->getDesc().structStride!=16||
        positions->getDesc().byteSize<bytes||velocities->getDesc().byteSize<bytes)
        throw std::invalid_argument("GVDB velocity transfer requires complete float4 point buffers");
    const auto resolution=volume.info().atlasResolutionAndApron;
    if(resolution.w<1)throw std::invalid_argument("GVDB MAC sampling requires at least one updated apron voxel");
    const uint64_t atlasBytes=uint64_t(resolution.x)*resolution.y*resolution.z*4;
    for(size_t i=0;i<3;++i)for(nvrhi::IBuffer* channel:{current[i],previous[i]})
        if(!channel||channel->getDesc().structStride!=4||channel->getDesc().byteSize<atlasBytes)
            throw std::invalid_argument("GVDB MAC channel does not match the scalar atlas layout");
    VelocityTransfer result{runtime.buffer(bytes,16),runtime.buffer(bytes,16),runtime.buffer(bytes,16)};
    VelocityParameters p{volume.info(),{count,advect?1u:0u,0,0},{timeStep,flipWeight,0,0}};
    std::vector<BindingSetItem> bindings={BindingSetItem::StructuredBuffer_SRV(0,positions),BindingSetItem::StructuredBuffer_SRV(1,velocities),
        BindingSetItem::RawBuffer_SRV(2,volume.nodes()),BindingSetItem::RawBuffer_SRV(3,volume.children()),
        BindingSetItem::StructuredBuffer_SRV(4,volume.levels()),BindingSetItem::StructuredBuffer_UAV(0,result.velocities),
        BindingSetItem::StructuredBuffer_UAV(1,result.positions),BindingSetItem::StructuredBuffer_UAV(2,result.sampledGrid)};
    for(uint32_t i=0;i<3;++i){bindings.push_back(BindingSetItem::StructuredBuffer_SRV(5+i,current[i]));bindings.push_back(BindingSetItem::StructuredBuffer_SRV(8+i,previous[i]));}
    const uint32_t groups=count/256+uint32_t(count%256!=0);
    for(const char* entry:{"sampleGridVelocity","updateFlipParticles"})runtime.dispatch("GVDBVelocity",entry,&p,sizeof(p),bindings,
        std::min(groups,65535u),groups/65535u+uint32_t(groups%65535u!=0));
    return result;
}
} // namespace gvdb::slang
