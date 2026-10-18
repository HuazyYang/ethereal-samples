// Fixed mixed-scene resource-array ABI probe. SPDX-License-Identifier: Apache-2.0
#include "GVDBTypes.slang"
StructuredBuffer<float> scalarAtlases[8] : register(t16);
ByteAddressBuffer nodePools[8] : register(t24);
ByteAddressBuffer childPools[8] : register(t32);
StructuredBuffer<LevelInfo> levelPools[8] : register(t40);
ByteAddressBuffer colorAtlases[8] : register(t48);
StructuredBuffer<float4> transfers[8] : register(t56);
struct Result {uint4 values;uint4 more;uint4 dimensions;};
RWStructuredBuffer<Result> results : register(u0);
[shader("raygeneration")]
void arrayProbe() {
    uint pixel=DispatchRaysIndex().x;
    if(pixel>=8)return;
    uint slot=(pixel*5+3)%8,index=NonUniformResourceIndex(slot);
    uint scalarCount,scalarStride,levelCount,levelStride,transferCount,transferStride,nodeBytes,childBytes,colorBytes;
    scalarAtlases[index].GetDimensions(scalarCount,scalarStride);
    levelPools[index].GetDimensions(levelCount,levelStride);
    transfers[index].GetDimensions(transferCount,transferStride);
    nodePools[index].GetDimensions(nodeBytes);
    childPools[index].GetDimensions(childBytes);
    colorAtlases[index].GetDimensions(colorBytes);
    LevelInfo level=levelPools[index][levelCount-1];
    float4 transfer=transfers[index][transferCount-1];
    Result result;
    result.values=uint4(asuint(scalarAtlases[index][scalarCount-1]),nodePools[index].Load(nodeBytes-4),
        childPools[index].Load(childBytes-4),colorAtlases[index].Load(colorBytes-4));
    result.more=uint4(level.nodes.x+level.children.y+uint(level.childExtent.z)+uint(level.nodeExtent.w),
        asuint(transfer.x+transfer.y+transfer.z+transfer.w),slot,(scalarStride<<24)|(levelStride<<12)|transferStride);
    result.dimensions=uint4(scalarCount,levelCount,transferCount,nodeBytes+childBytes+colorBytes);
    results[pixel]=result;
}
