// SPDX-License-Identifier: Apache-2.0
#include "GVDBSlangSubcells.h"
#include "ComputeRuntime.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
using nvrhi::BindingSetItem;
namespace gvdb::slang {
SlangPointBins binPointSubcells(ethereal::slang::ComputeRuntime& runtime,const PackedVolume& volume,
    nvrhi::IBuffer* positions,uint32_t count,uint32_t width,float radius,Float4 translation,nvrhi::IBuffer* colors) {
    if(volume.levels.empty()||volume.nodes.empty()||!width||width>8||!std::isfinite(radius)||radius<0||
        !std::isfinite(translation.x)||!std::isfinite(translation.y)||!std::isfinite(translation.z))
        throw std::invalid_argument("GVDB subcell binning parameters are invalid");
    const auto& leaves=volume.levels[0];const uint32_t leafWidth=uint32_t(leaves.nodeExtent.x);
    if(leafWidth%width||!leaves.nodes.z)throw std::invalid_argument("GVDB subcell width must divide the leaf width");
    const uint32_t side=leafWidth/width;const uint64_t perLeaf=uint64_t(side)*side*side,cellCount=perLeaf*leaves.nodes.z;
    if(cellCount>65535u*256u||cellCount>UINT32_MAX/sizeof(GatherCell)||count>UINT32_MAX/16||
        !positions||positions->getDesc().structStride!=16||positions->getDesc().byteSize<uint64_t(count)*16||
        (colors&&(colors->getDesc().structStride!=4||colors->getDesc().byteSize<uint64_t(count)*4)))
        throw std::invalid_argument("GVDB subcell ranges exceed input buffers or portable scan limits");
    SlangPointBins result;result.cells.reserve(size_t(cellCount));
    const uint32_t zero=0,emptyChild[2]={UINT32_MAX,UINT32_MAX};
    auto nodes=runtime.buffer(volume.nodes.size()*4,0,volume.nodes.data(),true);
    auto children=runtime.buffer(volume.children.empty()?8:volume.children.size()*4,0,volume.children.empty()?emptyChild:volume.children.data(),true);
    auto levels=runtime.buffer(volume.levels.size()*sizeof(LevelInfo),sizeof(LevelInfo),volume.levels.data());
    nvrhi::BufferHandle pointColors=colors;if(!pointColors)pointColors=runtime.buffer(4,4,&zero);
    auto counts=runtime.buffer(size_t(cellCount)*4,4);
    SubcellParameters p{volume.volume,{count,leaves.nodes.z,width,uint32_t(perLeaf)},{translation.x,translation.y,translation.z,radius},{side,uint32_t(cellCount),colors?1u:0u,0}};
    auto launch=[&](const char* entry,const std::vector<BindingSetItem>& bindings,uint32_t elements){const uint32_t groups=elements/256+uint32_t(elements%256!=0);if(groups)runtime.dispatch("GVDBSubcells",entry,&p,sizeof(p),bindings,std::min(groups,65535u),groups/65535u+uint32_t(groups%65535u!=0));};
    std::vector<BindingSetItem> bindings={BindingSetItem::StructuredBuffer_SRV(0,positions),BindingSetItem::StructuredBuffer_SRV(1,pointColors),
        BindingSetItem::RawBuffer_SRV(2,nodes),BindingSetItem::RawBuffer_SRV(3,children),BindingSetItem::StructuredBuffer_SRV(4,levels),BindingSetItem::StructuredBuffer_UAV(0,counts)};
    launch("clearSubcellCounts",bindings,uint32_t(cellCount));launch("countSubcellPoints",bindings,count);
    std::vector<uint32_t> hostCounts(size_t(cellCount),0);runtime.read(counts,hostCounts.data(),hostCounts.size()*4);
    uint64_t total=0;
    for(uint32_t leaf=0;leaf<leaves.nodes.z;++leaf){const size_t word=(leaves.nodes.x+uint64_t(leaf)*leaves.nodes.y)/4;const auto* node=volume.nodes.data()+word;
        for(uint32_t cell=0;cell<perLeaf;++cell){const uint32_t x=cell%side,y=(cell/side)%side,z=cell/(side*side),index=leaf*uint32_t(perLeaf)+cell;
            if(total+hostCounts[index]>UINT32_MAX/16)throw std::invalid_argument("GVDB expanded point pairs exceed portable buffer addressing");
            if(((node[0]>>8)&255u)!=0)result.cells.push_back({{int32_t(node[1])+int32_t(x*width),int32_t(node[2])+int32_t(y*width),int32_t(node[3])+int32_t(z*width),int32_t(width)},
                {node[4]+x*width,node[5]+y*width,node[6]+z*width,0},{uint32_t(total),hostCounts[index],0,0}});total+=hostCounts[index];
        }
    }
    result.pointCount=uint32_t(total);result.positions=runtime.buffer(size_t(std::max(uint64_t(1),total))*16,16);
    if(colors)result.colors=runtime.buffer(size_t(std::max(uint64_t(1),total))*4,4);
    auto outputColors=result.colors?result.colors:runtime.buffer(size_t(std::max(uint64_t(1),total))*4,4);
    auto offsets=runtime.exclusiveScan(counts,uint32_t(cellCount));
    bindings.push_back(BindingSetItem::StructuredBuffer_SRV(5,offsets));bindings.push_back(BindingSetItem::StructuredBuffer_UAV(1,result.positions));bindings.push_back(BindingSetItem::StructuredBuffer_UAV(2,outputColors));
    launch("clearSubcellCounts",bindings,uint32_t(cellCount));launch("scatterSubcellPoints",bindings,count);
    return result;
}
} // namespace gvdb::slang
