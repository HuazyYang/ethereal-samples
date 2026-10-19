// SPDX-License-Identifier: Apache-2.0
#include "GVDBTypes.slang"
cbuffer Parameters : register(b0) {
    uint4 sourceNodes; // Byte offset, stride, pool count, unused.
    uint4 work; // Output index count, subcells per brick, obstacle active leaves, unused.
    uint4 hierarchy;
};
ByteAddressBuffer sourcePool : register(t0);
ByteAddressBuffer obstaclePool : register(t1);
ByteAddressBuffer obstacleChildren : register(t2);
StructuredBuffer<LevelInfo> obstacleLevels : register(t3);
StructuredBuffer<uint> outputToSource : register(t4);
RWStructuredBuffer<uint> outputIds : register(u0);
uint obstacleLeaf(int3 position) {
    uint level=hierarchy.x, index=hierarchy.y;
    if (work.z==0 || level>=hierarchy.z || hierarchy.z>10) return GVDB_INVALID;
    for (uint depth=0; depth<10; ++depth) {
        LevelInfo info=obstacleLevels[level];
        if (index>=info.nodes.z) return GVDB_INVALID;
        VDBNode node=loadNode(obstaclePool,info.nodes.x+index*info.nodes.y);
        // Published node endpoints fit the owner's checked integer domain.
        int3 relative=position-node.position;
        if (any(relative<0) || any(relative>=info.nodeExtent.xyz)) return GVDB_INVALID;
        if (level==0) return ((node.metadata>>8)&255u)!=0 ? index : GVDB_INVALID;
        uint2 list=node.childList;
        if (nullReference(list) || !referenceFitsIndex(list)) return GVDB_INVALID;
        uint listLevel=referenceLevel(list);
        if (listLevel>=hierarchy.z) return GVDB_INVALID;
        LevelInfo childInfo=obstacleLevels[listLevel];
        uint listIndex=referenceIndex(list);
        if (listIndex>=childInfo.children.z) return GVDB_INVALID;
        uint3 childPosition=uint3(relative)/uint3(obstacleLevels[level-1].nodeExtent.xyz);
        if (any(childPosition>=info.children.w)) return GVDB_INVALID;
        uint bit=flatten3(childPosition,uint3(info.children.w));
        uint2 child=obstacleChildren.Load2(childInfo.children.x+listIndex*childInfo.children.y+bit*8);
        if (nullReference(child) || !referenceFitsIndex(child) || referenceLevel(child)!=level-1) return GVDB_INVALID;
        index=referenceIndex(child); --level;
    }
    return GVDB_INVALID;
}
[shader("compute")]
[numthreads(256,1,1)]
void mapObstacleSubcells(uint3 group:SV_GroupID,uint local:SV_GroupIndex) {
    uint i=(group.y*65535u+group.x)*256u+local;
    if (i>=work.x) return;
    uint source=outputToSource[i/work.y], result=GVDB_INVALID;
    if (source<sourceNodes.z) {
        VDBNode node=loadNode(sourcePool,sourceNodes.x+source*sourceNodes.y);
        if (((node.metadata>>8)&255u)!=0) result=obstacleLeaf(node.position);
    }
    outputIds[i]=result;
}
