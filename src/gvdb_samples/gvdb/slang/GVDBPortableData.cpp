// SPDX-License-Identifier: Apache-2.0
#include "GVDBPortableData.h"
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>
namespace gvdb::slang {
namespace {
constexpr uint64_t kAddressSpace = uint64_t(UINT32_MAX) + 1;
bool fail(std::string& error, const char* message) { error = message; return false; }
uint32_t word(const PoolSource& pool, uint64_t index, uint32_t offset) {
    uint32_t value;
    std::memcpy(&value, static_cast<const uint8_t*>(pool.data) + index * pool.stride + offset, 4);
    return value;
}
UInt2 reference(const PoolSource& pool, uint64_t index, uint32_t offset) {
    return {word(pool,index,offset),word(pool,index,offset+4)};
}
bool validReference(UInt2 ref, uint32_t group, uint32_t level, const std::vector<LevelSource>& levels) {
    if (isNull(ref)) return true;
    if (referenceGroup(ref)!=group || referenceLevel(ref)!=level || level>=levels.size()) return false;
    return referenceIndex(ref)<(group==0 ? levels[level].nodes.count : levels[level].children.count);
}
bool checkPool(const PoolSource& pool, uint64_t minimumStride, uint64_t alignment, uint64_t& bytes, std::string& error) {
    if (pool.count>UINT32_MAX || pool.stride>UINT32_MAX) return fail(error,"pool count or stride exceeds the portable 32-bit range");
    if (pool.count==0) return true;
    if (!pool.data || pool.stride<minimumStride || pool.stride%alignment) return fail(error,"pool has missing data, insufficient stride or invalid alignment");
    if (pool.count>kAddressSpace/pool.stride) return fail(error,"pool exceeds the portable 4-GiB address space");
    uint64_t extent=pool.count*pool.stride;
    if (bytes>UINT32_MAX || extent>kAddressSpace-bytes) return fail(error,"combined pools exceed the portable 4-GiB address space");
    bytes+=extent;
    return true;
}
}
bool packVolume(const VolumeSource& source, PackedVolume& output, std::string& error) {
    error.clear();
    const auto& v=source.volume;
    const size_t levels=source.levels.size();
    if (levels==0 || levels>kMaxLevels || v.hierarchy.z!=levels || v.hierarchy.x>=levels)
        return fail(error,"invalid hierarchy level count or top level");
    if (v.hierarchy.y>=source.levels[v.hierarchy.x].nodes.count)
        return fail(error,"root node does not exist");
    const auto res=v.atlasResolutionAndApron;
    const auto grid=v.atlasBrickCountAndWidth;
    if (!res.x || !res.y || !res.z || !grid.x || !grid.y || !grid.z || grid.w<=2ull*res.w)
        return fail(error,"atlas dimensions or apron are invalid");
    if (uint64_t(grid.x)*grid.w!=res.x || uint64_t(grid.y)*grid.w!=res.y || uint64_t(grid.z)*grid.w!=res.z)
        return fail(error,"atlas resolution does not match brick grid");
    if (uint64_t(grid.x)*grid.y>UINT32_MAX || uint64_t(grid.x)*grid.y*grid.z>UINT32_MAX)
        return fail(error,"atlas slot index exceeds the portable 32-bit range");
    const uint64_t slots=uint64_t(grid.x)*grid.y*grid.z;
    if (slots>UINT32_MAX || slots!=source.atlasMapCount || !source.atlasMap)
        return fail(error,"atlas map does not match brick grid");
    if (uint64_t(res.x)*res.y>UINT32_MAX || uint64_t(res.x)*res.y*res.z>UINT32_MAX)
        return fail(error,"atlas voxel index exceeds the portable 32-bit range");
    const float minimum[3]={v.boundsMin.x,v.boundsMin.y,v.boundsMin.z};
    const float maximum[3]={v.boundsMax.x,v.boundsMax.y,v.boundsMax.z};
    for(unsigned axis=0;axis<3;++axis) if(!std::isfinite(minimum[axis]) || !std::isfinite(maximum[axis]) || minimum[axis]>maximum[axis])
        return fail(error,"volume bounds are invalid");
    uint64_t nodeBytes=0,childBytes=0,range=1;
    std::vector<LevelInfo> metadata(levels);
    for(size_t level=0;level<levels;++level) {
        const auto& input=source.levels[level];
        if(input.logDimension==0 || input.logDimension>10) return fail(error,"branching dimension is unsupported");
        const uint32_t width=1u<<input.logDimension;
        const uint64_t childGrid=uint64_t(width)*width*width;
        const uint64_t childSize=childGrid*8;
        if(range>uint64_t(INT32_MAX)/width) return fail(error,"node index-space extent exceeds signed 32-bit range");
        auto& info=metadata[level];
        info.nodes={uint32_t(nodeBytes),uint32_t(input.nodes.stride),uint32_t(input.nodes.count),input.logDimension};
        info.children={uint32_t(childBytes),uint32_t(input.children.stride),uint32_t(input.children.count),width};
        info.childExtent={float(range),float(range),float(range),0};
        range*=width;
        info.nodeExtent={int32_t(range),int32_t(range),int32_t(range),0};
        if(!checkPool(input.nodes,64,16,nodeBytes,error) || !checkPool(input.children,childSize,8,childBytes,error)) return false;
        if(level==0 && input.children.count) return fail(error,"leaf nodes cannot own child lists");
        for(uint64_t index=0;index<input.nodes.count;++index) {
            if((word(input.nodes,index,0)&255u)!=level) return fail(error,"serialized node level does not match its pool");
            if(!validReference(reference(input.nodes,index,40),0,uint32_t(level+1),source.levels)) return fail(error,"invalid serialized parent reference");
            const UInt2 list=reference(input.nodes,index,48);
            if((level==0 && !isNull(list)) || !validReference(list,1,uint32_t(level),source.levels)) return fail(error,"invalid serialized child-list reference");
        }
        for(uint64_t index=0;index<input.children.count;++index) for(uint64_t child=0;child<childGrid;++child) {
            if(!validReference(reference(input.children,index,uint32_t(child*8)),0,uint32_t(level-1),source.levels))
                return fail(error,"invalid child reference in serialized child list");
        }
    }
    if((uint64_t(1)<<source.levels[0].logDimension)+2ull*res.w!=grid.w)
        return fail(error,"atlas interior width does not match the leaf geometry");
    for(size_t slot=0;slot<source.atlasMapCount;++slot) {
        const auto& entry=source.atlasMap[slot];
        if(entry.leaf<0) continue;
        if(uint32_t(entry.leaf)>=source.levels[0].nodes.count) return fail(error,"atlas map references a missing leaf");
        const auto& pool=source.levels[0].nodes;
        const uint32_t leaf=uint32_t(entry.leaf);
        if(((word(pool,leaf,0)>>8)&255u)==0) return fail(error,"atlas map references an inactive leaf");
        if(int32_t(word(pool,leaf,4))!=entry.x || int32_t(word(pool,leaf,8))!=entry.y || int32_t(word(pool,leaf,12))!=entry.z)
            return fail(error,"atlas map world position differs from its leaf");
        const uint32_t x=uint32_t(slot%grid.x), y=uint32_t((slot/grid.x)%grid.y), z=uint32_t(slot/(uint64_t(grid.x)*grid.y));
        if(word(pool,leaf,16)!=x*grid.w+res.w || word(pool,leaf,20)!=y*grid.w+res.w || word(pool,leaf,24)!=z*grid.w+res.w)
            return fail(error,"leaf atlas position differs from its atlas slot");
    }
    PackedVolume packed;
    packed.volume=v;
    packed.levels=std::move(metadata);
    packed.nodes.resize(size_t(nodeBytes/4));
    packed.children.resize(size_t(childBytes/4));
    packed.atlasMap.assign(source.atlasMap,source.atlasMap+source.atlasMapCount);
    for(size_t level=0;level<levels;++level) {
        const auto& input=source.levels[level];
        if(input.nodes.count) std::memcpy(reinterpret_cast<uint8_t*>(packed.nodes.data())+packed.levels[level].nodes.x,input.nodes.data,size_t(input.nodes.count*input.nodes.stride));
        if(input.children.count) std::memcpy(reinterpret_cast<uint8_t*>(packed.children.data())+packed.levels[level].children.x,input.children.data,size_t(input.children.count*input.children.stride));
    }
    output=std::move(packed);
    return true;
}
} // namespace gvdb::slang
