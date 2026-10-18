// CPU topology construction and sparse apron tests.
// SPDX-License-Identifier: Apache-2.0
#include "../gvdb/slang/GVDBTopologyBuilder.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <iostream>
#include <stdexcept>
using namespace gvdb::slang;
static void require(bool c,const char* m){if(!c)throw std::runtime_error(m);}
static bool lookup(const PackedVolume& v,Int4 p){
    uint32_t level=v.volume.hierarchy.x,index=v.volume.hierarchy.y;
    for(;;){const auto& info=v.levels[level];const auto* node=v.nodes.data()+(info.nodes.x+size_t(index)*info.nodes.y)/4;
        const int64_t x=int64_t(p.x)-int32_t(node[1]),y=int64_t(p.y)-int32_t(node[2]),z=int64_t(p.z)-int32_t(node[3]);
        if(x<0||y<0||z<0||x>=info.nodeExtent.x||y>=info.nodeExtent.y||z>=info.nodeExtent.z)return false;
        if(level==0)return ((node[0]>>8)&255u)!=0;
        const UInt2 list{node[12],node[13]};if(isNull(list))return false;
        const uint32_t r=info.children.w,extent=uint32_t(info.childExtent.x);
        const size_t bit=(size_t(z/extent)*r+size_t(y/extent))*r+size_t(x/extent);
        const size_t word=(info.children.x+referenceIndex(list)*info.children.y)/4+bit*2;
        const UInt2 child{v.children[word],v.children[word+1]};if(isNull(child))return false;index=uint32_t(referenceIndex(child));--level;
    }
}
static void verifyTranslation(const ScalarVolumeData& source,const ScalarVolumeData& shifted,Int4 origin){
    const int32_t delta[]={origin.x,origin.y,origin.z};auto expectedNodes=source.packed.nodes;
    require(source.packed.levels.size()==shifted.packed.levels.size(),"translation changed level count");
    for(size_t l=0;l<source.packed.levels.size();++l){const auto& level=source.packed.levels[l];
        require(std::memcmp(&level,&shifted.packed.levels[l],sizeof(level))==0,"translation changed level layout");
        for(uint32_t i=0;i<level.nodes.z;++i){auto* node=expectedNodes.data()+(level.nodes.x+size_t(i)*level.nodes.y)/4;
            for(size_t axis=0;axis<3;++axis)node[1+axis]=uint32_t(int64_t(int32_t(node[1+axis]))+delta[axis]);}
    }
    require(expectedNodes==shifted.packed.nodes,"translation did not shift every signed node position");
    require(source.packed.children==shifted.packed.children&&source.atlas==shifted.atlas,"translation changed references or atlas values");
    require(source.packed.atlasMap.size()==shifted.packed.atlasMap.size(),"translation changed atlas map count");
    for(size_t i=0;i<source.packed.atlasMap.size();++i){const auto& a=source.packed.atlasMap[i];const auto& b=shifted.packed.atlasMap[i];
        const bool active=a.leaf>=0;
        require(b.leaf==a.leaf&&b.x==a.x+(active?origin.x:0)&&b.y==a.y+(active?origin.y:0)&&b.z==a.z+(active?origin.z:0),"translation did not shift signed atlas map positions");}
    auto expectedVolume=source.packed.volume;
    expectedVolume.boundsMin.x+=origin.x;expectedVolume.boundsMin.y+=origin.y;expectedVolume.boundsMin.z+=origin.z;
    expectedVolume.boundsMax.x+=origin.x;expectedVolume.boundsMax.y+=origin.y;expectedVolume.boundsMax.z+=origin.z;
    require(std::memcmp(&expectedVolume,&shifted.packed.volume,sizeof(expectedVolume))==0,"translation did not shift bounds or changed volume layout");
    require(lookup(shifted.packed,{origin.x+1,origin.y,origin.z,0})&&
        !lookup(shifted.packed,{origin.x,origin.y+2,origin.z,0}),"translated sparse lookup differs");
}
int main(){try{
    TopologyBuildOptions options;options.logDimensions={1,1,1};options.atlasGrid={3,1,1,0};PackedVolume volume;std::string error;
    require(buildTopology({{-4,0,0,0},{0,0,0,0},{2,2,0,0},{-4,0,0,0}},options,volume,error),error.c_str());
    require(volume.levels[0].nodes.z==3&&volume.volume.hierarchy.x==2,"deduplicated topology depth");
    require(lookup(volume,{-3,0,0,0})&&lookup(volume,{0,0,0,0})&&lookup(volume,{3,3,1,0}),"negative/multilevel leaf lookup");
    require(!lookup(volume,{-1,0,0,0})&&!lookup(volume,{4,0,0,0}),"sparse hole or outer bounds lookup");
    const auto original=volume.nodes;
    require(!buildTopology({{1,0,0,0}},options,volume,error)&&volume.nodes==original,"misaligned leaf accepted or output changed");
    auto invalid=options;invalid.logDimensions={1};require(!buildTopology({{0,0,0,0},{4,0,0,0}},invalid,volume,error),"insufficient root accepted");
    invalid=options;invalid.apron=UINT32_MAX;require(!buildTopology({{0,0,0,0}},invalid,volume,error),"overflowing apron accepted");
    invalid=options;invalid.atlasGrid={1,1,1,0};require(!buildTopology({{0,0,0,0},{2,0,0,0}},invalid,volume,error),"short atlas accepted");
    std::vector<float> values(5*3*2,0);values[1]=7;values[2]=11;values[(1*3+2)*5+4]=13;ScalarVolumeData scalar;
    require(buildScalarVolume(values.data(),values.size(),{5,3,2,0},options,0,0,scalar,error),error.c_str());
    require(scalar.packed.levels[0].nodes.z==3&&scalar.atlas.size()==12*4*4,"scalar activated leaves and atlas shape");
    const auto at=[&](size_t x,size_t y,size_t z){return scalar.atlas[(z*4+y)*12+x];};
    require(at(2,1,1)==7&&at(3,1,1)==11&&at(4,1,1)==7,"cross-brick scalar apron values");
    require(at(9,1,2)==13&&at(10,1,2)==0&&at(0,0,0)==0,"partial source brick and outer background");
    for(const auto delta:std::vector<Int4>{{-8,4,-6,0},{8388598,-8388606,0,0}}){
        ScalarVolumeData shifted;require(buildScalarVolumeAtOrigin(values.data(),values.size(),{5,3,2,0},delta,options,0,0,shifted,error),error.c_str());
        verifyTranslation(scalar,shifted,delta);
    }
    // Integer coordinates alone are insufficient: these accepted int32 values
    // collapse an eight-voxel float box to zero width and lose voxel centers.
    constexpr int32_t large=1<<28;
    require(float(large)==float(large+8),"large-coordinate regression premise changed");
    const auto savedNodes=scalar.packed.nodes,savedChildren=scalar.packed.children;const auto saved=scalar.atlas;
    for(const auto delta:std::vector<Int4>{{large,0,0,0},{0,-large,0,0},{0,0,large,0},{8388600,0,0,0},{-8388608,0,0,0}}){
        require(!buildScalarVolumeAtOrigin(values.data(),values.size(),{5,3,2,0},delta,options,0,0,scalar,error),"unrepresentable translated coordinates accepted");
        require(scalar.packed.nodes==savedNodes&&scalar.packed.children==savedChildren&&scalar.atlas==saved,"invalid translation changed output");
    }
    for(const auto p:std::vector<Int4>{{large,0,0,0},{0,-large,0,0},{0,0,large,0},{8388606,0,0,0},{-8388608,0,0,0}})
        require(!buildTopology({p},options,volume,error)&&volume.nodes==original,"unrepresentable topology or apron accepted or output changed");
    require(!buildTopology({{8388600,0,0,0},{8388604,0,0,0}},options,volume,error),"root extent outside float range accepted");
    for(const auto p:std::vector<Int4>{{8388604,0,0,0},{-8388606,0,0,0}}){
        PackedVolume edge;require(buildTopology({p},options,edge,error),error.c_str());
        require(lookup(edge,p)&&double(float(p.x)+.5f)==double(p.x)+.5&&
            double(float(p.x+1)+.5f)==double(p.x)+1.5,"representable boundary voxel center or lookup differs");
    }
    values[0]=std::numeric_limits<float>::quiet_NaN();
    require(!buildScalarVolume(values.data(),values.size(),{5,3,2,0},options,0,0,scalar,error)&&scalar.atlas==saved,"NaN scalar accepted or output changed");
    std::fill(values.begin(),values.end(),0.f);
    require(!buildScalarVolume(values.data(),values.size(),{5,3,2,0},options,0,0,scalar,error)&&scalar.atlas==saved,"empty activation changed output");
    std::cout<<"Portable topology builder, signed translations, float coordinate limits and sparse apron tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
