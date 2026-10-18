// CPU topology construction and sparse apron tests.
// SPDX-License-Identifier: Apache-2.0
#include "../gvdb/slang/GVDBTopologyBuilder.h"
#include <cstring>
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
    const auto saved=scalar.atlas;std::fill(values.begin(),values.end(),0);
    require(!buildScalarVolume(values.data(),values.size(),{5,3,2,0},options,0,0,scalar,error)&&scalar.atlas==saved,"empty activation changed output");
    std::cout<<"Portable topology builder, negative coordinates, sparse lookup and dense apron tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
