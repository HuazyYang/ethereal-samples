// SPDX-License-Identifier: Apache-2.0
#include "GVDBLegacyAdapter.h"
#include "GVDB.h"
namespace gvdb::slang {
bool packLegacyVolume(IGVDBVolume& volume,PackedVolume& output,std::string& error) {
    if(volume.getLevelConfig().useBitmasks) {error="portable GVDB does not support bitmask-compressed child lists";return false;}
    const NodeId root=volume.getRootId();
    if(root==NodeId::null() || root.group()!=0 || root.index()>UINT32_MAX) {error="volume has no portable root node";return false;}
    VolumeSource source;
    const auto res=volume.getAtlasResDim();
    const auto grid=volume.getAtlasGridDim();
    const auto brick=volume.getBrickDimWithApron();
    const auto bounds=volume.getVoxelBounds();
    source.volume.atlasResolutionAndApron={res.x,res.y,res.z,volume.getAtlasConfig().apron};
    source.volume.atlasBrickCountAndWidth={grid.x,grid.y,grid.z,brick.x};
    source.volume.hierarchy={root.level(),uint32_t(root.index()),volume.getNumLevels(),0};
    if(!bounds.isempty()) {
        const auto lo=bounds.lower(),hi=bounds.upper();
        source.volume.boundsMin={float(lo.x),float(lo.y),float(lo.z),0};
        source.volume.boundsMax={float(hi.x),float(hi.y),float(hi.z),0};
    }
    for(uint8_t level=0;level<volume.getNumLevels();++level) {
        const auto nodes=volume.getNodePool(level),children=volume.getChildListPool(level);
        source.levels.push_back({{nodes.data,nodes.count,nodes.stride},{children.data,children.count,children.stride},volume.getLogDim(level)});
    }
    std::vector<slang::AtlasNode> atlasMap;
    for(const auto& entry:volume.getAtlasMap())atlasMap.push_back({entry.pos.x,entry.pos.y,entry.pos.z,entry.leafNode});
    source.atlasMap=atlasMap.data();source.atlasMapCount=atlasMap.size();
    return packVolume(source,output,error);
}
} // namespace gvdb::slang
