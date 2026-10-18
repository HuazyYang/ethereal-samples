// SPDX-License-Identifier: Apache-2.0
#include "GVDBTopologyBuilder.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
namespace gvdb::slang {
namespace {
using Position=std::tuple<int32_t,int32_t,int32_t>;
void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
Position position(Int4 p){return {p.x,p.y,p.z};}
Int4 coordinate(const Position& p){return {std::get<0>(p),std::get<1>(p),std::get<2>(p),0};}
void reference(uint32_t* destination,UInt2 value){destination[0]=value.x;destination[1]=value.y;}
Position parentPosition(Int4 p,Int4 root,uint32_t extent){return {
    int32_t(int64_t(root.x)+(int64_t(p.x)-root.x)/extent*extent),
    int32_t(int64_t(root.y)+(int64_t(p.y)-root.y)/extent*extent),
    int32_t(int64_t(root.z)+(int64_t(p.z)-root.z)/extent*extent)};}
}
bool buildTopology(const std::vector<Int4>& leafOrigins,const TopologyBuildOptions& options,PackedVolume& output,std::string& error){try{
    require(!leafOrigins.empty()&&!options.logDimensions.empty()&&options.logDimensions.size()<=kMaxLevels,"topology builder requires leaves and one to ten levels");
    std::vector<uint32_t> extents;uint64_t extent=1;
    for(uint32_t log:options.logDimensions){require(log>0&&log<=10,"topology branching dimension is invalid");extent*=uint64_t(1)<<log;require(extent<=INT32_MAX,"topology extent exceeds signed coordinates");extents.push_back(uint32_t(extent));}
    const uint32_t leafWidth=extents[0];std::set<Position> leaves;
    Int4 root=leafOrigins[0],maximum=leafOrigins[0];
    for(const auto& p:leafOrigins){require(p.x%int32_t(leafWidth)==0&&p.y%int32_t(leafWidth)==0&&p.z%int32_t(leafWidth)==0,"leaf origin is not aligned to its width");
        require(int64_t(p.x)+leafWidth<=INT32_MAX&&int64_t(p.y)+leafWidth<=INT32_MAX&&int64_t(p.z)+leafWidth<=INT32_MAX,"leaf endpoint exceeds signed coordinates");
        leaves.insert(position(p));root.x=std::min(root.x,p.x);root.y=std::min(root.y,p.y);root.z=std::min(root.z,p.z);
        maximum.x=std::max(maximum.x,p.x);maximum.y=std::max(maximum.y,p.y);maximum.z=std::max(maximum.z,p.z);
    }
    require(leaves.size()<=INT32_MAX,"too many leaves for atlas mapping");
    const uint64_t span=uint64_t(std::max({int64_t(maximum.x)-root.x,int64_t(maximum.y)-root.y,int64_t(maximum.z)-root.z}))+leafWidth;
    size_t top=0;while(top<extents.size()&&extents[top]<span)++top;require(top<extents.size(),"configured root cannot cover all leaf origins");
    require(int64_t(root.x)+extents[top]<=INT32_MAX&&int64_t(root.y)+extents[top]<=INT32_MAX&&int64_t(root.z)+extents[top]<=INT32_MAX,"root endpoint exceeds signed coordinates");
    UInt4 grid=options.atlasGrid;
    if(!grid.x&&!grid.y&&!grid.z){uint32_t side=1;while(uint64_t(side)*side*side<leaves.size())++side;grid={side,side,uint32_t((leaves.size()+uint64_t(side)*side-1)/(uint64_t(side)*side)),0};}
    require(grid.x&&grid.y&&grid.z&&uint64_t(grid.x)*grid.y<=UINT32_MAX&&uint64_t(grid.x)*grid.y*grid.z<=UINT32_MAX,"atlas grid dimensions exceed the portable range");
    const uint64_t slots=uint64_t(grid.x)*grid.y*grid.z,width=uint64_t(leafWidth)+2ull*options.apron;
    require(slots>=leaves.size()&&width<=UINT32_MAX&&uint64_t(grid.x)*width<=UINT32_MAX&&uint64_t(grid.y)*width<=UINT32_MAX&&uint64_t(grid.z)*width<=UINT32_MAX,"atlas grid cannot contain its leaves");
    require(width*width<=UINT32_MAX&&slots<=UINT32_MAX/(width*width*width),"atlas voxel count exceeds the portable range");
    grid.w=uint32_t(width);
    std::vector<std::map<Position,uint32_t>> maps(options.logDimensions.size());
    for(const auto& leaf:leaves){const auto p=coordinate(leaf);for(size_t level=0;level<=top;++level)maps[level].emplace(parentPosition(p,root,extents[level]),0);}
    std::vector<std::vector<SerializedNode>> nodes(maps.size());std::vector<std::vector<UInt2>> children(maps.size());
    uint64_t totalNodes=0,totalChildren=0;
    for(size_t level=0;level<maps.size();++level){uint32_t index=0;for(auto& entry:maps[level])entry.second=index++;
        totalNodes+=uint64_t(maps[level].size())*64;require(totalNodes<=UINT32_MAX,"node pools exceed portable addressing");nodes[level].resize(maps[level].size());
        if(level&&!maps[level].empty()){const uint64_t childCount=uint64_t(1)<<(3*options.logDimensions[level]);totalChildren+=uint64_t(maps[level].size())*childCount*8;
            require(totalChildren<=UINT32_MAX,"child pools exceed portable addressing");children[level].resize(size_t(maps[level].size()*childCount),{UINT32_MAX,UINT32_MAX});}
    }
    std::vector<AtlasNode> atlas(size_t(slots),AtlasNode{-1,-1,-1,-1});
    for(size_t level=0;level<maps.size();++level)for(const auto& entry:maps[level]){
        const auto p=coordinate(entry.first);const uint32_t index=entry.second;auto* n=nodes[level][index].words;n[0]=uint32_t(level)|0x100u;
        n[1]=uint32_t(p.x);n[2]=uint32_t(p.y);n[3]=uint32_t(p.z);reference(n+10,{UINT32_MAX,UINT32_MAX});reference(n+12,{UINT32_MAX,UINT32_MAX});
        if(level){reference(n+12,makeReference(1,uint8_t(level),index));}
        else {const uint32_t x=index%grid.x,y=(index/grid.x)%grid.y,z=uint32_t(index/(uint64_t(grid.x)*grid.y));n[4]=x*grid.w+options.apron;n[5]=y*grid.w+options.apron;n[6]=z*grid.w+options.apron;atlas[index]={p.x,p.y,p.z,int32_t(index)};}
        if(level<top){const auto parentKey=parentPosition(p,root,extents[level+1]);const auto parent=coordinate(parentKey);const uint32_t parentIndex=maps[level+1].at(parentKey),resolution=1u<<options.logDimensions[level+1];
            const uint32_t x=uint32_t((int64_t(p.x)-parent.x)/extents[level]),y=uint32_t((int64_t(p.y)-parent.y)/extents[level]),z=uint32_t((int64_t(p.z)-parent.z)/extents[level]);
            const uint64_t bit=(uint64_t(z)*resolution+y)*resolution+x,childCount=uint64_t(resolution)*resolution*resolution;
            children[level+1][size_t(uint64_t(parentIndex)*childCount+bit)]=makeReference(0,uint8_t(level),index);reference(n+10,makeReference(0,uint8_t(level+1),parentIndex));
        }
    }
    VolumeSource source;source.volume.atlasResolutionAndApron={grid.x*grid.w,grid.y*grid.w,grid.z*grid.w,options.apron};source.volume.atlasBrickCountAndWidth=grid;
    source.volume.hierarchy={uint32_t(top),0,uint32_t(maps.size()),0};source.volume.boundsMin={float(root.x),float(root.y),float(root.z),0};source.volume.boundsMax={float(int64_t(maximum.x)+leafWidth),float(int64_t(maximum.y)+leafWidth),float(int64_t(maximum.z)+leafWidth),0};
    for(size_t level=0;level<maps.size();++level){const uint64_t stride=children[level].empty()?0:8ull<<(3*options.logDimensions[level]);source.levels.push_back({{nodes[level].data(),nodes[level].size(),64},{children[level].data(),level?nodes[level].size():0,stride},options.logDimensions[level]});}
    source.atlasMap=atlas.data();source.atlasMapCount=atlas.size();return packVolume(source,output,error);
}catch(const std::exception& exception){error=exception.what();return false;}}
bool buildScalarVolume(const float* values,size_t count,UInt4 res,const TopologyBuildOptions& options,float threshold,float background,ScalarVolumeData& output,std::string& error){try{
    require(values&&res.x&&res.y&&res.z&&uint64_t(res.x)*res.y<=UINT32_MAX&&uint64_t(res.x)*res.y*res.z==count&&count<=UINT32_MAX/4,"dense scalar dimensions are invalid");
    require(!options.logDimensions.empty()&&options.logDimensions[0]>0&&options.logDimensions[0]<=10,"leaf dimension is invalid");
    require(std::isfinite(threshold)&&threshold>=0&&std::isfinite(background),"scalar activation threshold or background is invalid");
    const uint32_t width=1u<<options.logDimensions[0];std::set<Position> active;
    for(uint32_t z=0;z<res.z;++z)for(uint32_t y=0;y<res.y;++y)for(uint32_t x=0;x<res.x;++x){const float value=values[(size_t(z)*res.y+y)*res.x+x];require(std::isfinite(value),"scalar topology activation requires finite values");
        if(std::abs(double(value)-background)>threshold)active.emplace(int32_t(x/width*width),int32_t(y/width*width),int32_t(z/width*width));}
    std::vector<Int4> origins;origins.reserve(active.size());for(const auto& p:active)origins.push_back(coordinate(p));ScalarVolumeData result;
    if(!buildTopology(origins,options,result.packed,error))return false;
    const auto grid=result.packed.volume.atlasBrickCountAndWidth,atlasRes=result.packed.volume.atlasResolutionAndApron;const uint64_t atlasCount=uint64_t(atlasRes.x)*atlasRes.y*atlasRes.z;
    require(atlasCount<=UINT32_MAX/4,"scalar atlas exceeds portable float storage");result.atlas.resize(size_t(atlasCount),background);
    for(size_t slot=0;slot<result.packed.atlasMap.size();++slot){const auto& map=result.packed.atlasMap[slot];if(map.leaf<0)continue;
        const uint32_t ax=uint32_t(slot%grid.x)*grid.w,ay=uint32_t((slot/grid.x)%grid.y)*grid.w,az=uint32_t(slot/(uint64_t(grid.x)*grid.y))*grid.w;
        for(uint32_t z=0;z<grid.w;++z)for(uint32_t y=0;y<grid.w;++y)for(uint32_t x=0;x<grid.w;++x){const int64_t wx=int64_t(map.x)+x-options.apron,wy=int64_t(map.y)+y-options.apron,wz=int64_t(map.z)+z-options.apron;
            if(wx<0||wy<0||wz<0||wx>=res.x||wy>=res.y||wz>=res.z)continue;
            if(active.find({int32_t(wx/width*width),int32_t(wy/width*width),int32_t(wz/width*width)})==active.end())continue;
            result.atlas[((size_t(az)+z)*atlasRes.y+ay+y)*atlasRes.x+ax+x]=values[(size_t(wz)*res.y+size_t(wy))*res.x+size_t(wx)];
        }
    }
    output=std::move(result);error.clear();return true;
}catch(const std::exception& exception){error=exception.what();return false;}}
} // namespace gvdb::slang
