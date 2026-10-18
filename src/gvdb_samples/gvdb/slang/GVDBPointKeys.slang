// Sparse point topology discovery using uint2 keys and stable scan compaction.
// Copyright 2017 NVIDIA Corporation. SPDX-License-Identifier: Apache-2.0
#include "GVDBTypes.slang"
struct PointKeyParameters { uint4 counts; float4 translation; uint4 grid; };
ConstantBuffer<PointKeyParameters> g_Parameters : register(b0);
StructuredBuffer<float4> g_Positions : register(t0);
StructuredBuffer<uint> g_LevelRanges : register(t1);
StructuredBuffer<KeyValue> g_Keys : register(t2);
StructuredBuffer<uint> g_Markers : register(t3);
StructuredBuffer<uint> g_Offsets : register(t4);
RWStructuredBuffer<KeyValue> g_OutputKeys : register(u0);
RWStructuredBuffer<uint> g_OutputMarkers : register(u1);
RWStructuredBuffer<uint> g_Counts : register(u2);
uint invocation(uint3 id){return id.x+id.y*(65535u*256u);}
bool validPosition(float3 point) {
    // Integer IEEE classification also works with FXC finite-math optimization.
    return all((asuint(point)&uint3(0x7fffffffu))<uint3(0x7f800000u)) && all(point>=0);
}
[shader("compute")][numthreads(256,1,1)]
void calculateBrickKeys(uint3 id:SV_DispatchThreadID) {
    uint i=invocation(id);[branch] if(i>=g_Parameters.counts.z)return;
    uint pointIndex=i/g_Parameters.counts.y,level=i%g_Parameters.counts.y;
    float3 position=g_Positions[pointIndex].xyz+g_Parameters.translation.xyz;
    KeyValue key;key.key=uint2(0,0x00ff0000u);key.value=pointIndex;key.reserved=0;
    uint range=g_LevelRanges[level];
    if(validPosition(position)&&range>0) {
        float3 coordinates=floor(position/float(range));
        if(all(coordinates<=65535)) {
            uint3 cell=uint3(coordinates);
            key.key=uint2(cell.z|(cell.y<<16),cell.x|(level<<16));
        }
    }
    g_OutputKeys[i]=key;
}
[shader("compute")][numthreads(256,1,1)]
void findActiveBricks(uint3 id:SV_DispatchThreadID) {
    uint i=invocation(id);if(i>=g_Parameters.counts.x)return;
    float3 position=g_Positions[i].xyz+g_Parameters.translation.xyz;
    uint range=g_LevelRanges[g_Parameters.counts.w];
    uint brick=GVDB_INVALID;
    if(validPosition(position)&&range>0) {
        float3 coordinates=floor(position/float(range));
        if(all(coordinates<float3(g_Parameters.grid.xyz)))brick=flatten3(uint3(coordinates),g_Parameters.grid.xyz);
    }
    g_OutputMarkers[i]=brick;
}
[shader("compute")][numthreads(256,1,1)]
void clearUniqueCounts(uint3 id:SV_DispatchThreadID) {
    if(id.x<=g_Parameters.counts.y)g_Counts[id.x]=0;
}
// Dispatch after sorting the key array. Every marker is written, including zero.
// g_Counts[0] includes the invalid sentinel, matching the legacy unique count.
[shader("compute")][numthreads(256,1,1)]
void markUniqueKeys(uint3 id:SV_DispatchThreadID) {
    uint i=invocation(id);[branch] if(i>=g_Parameters.counts.z)return;
    uint2 key=g_Keys[i].key;
    bool unique=i==0;
    if(i>0)unique=any(key!=g_Keys[i-1].key);
    g_OutputMarkers[i]=unique?1:0;
    [branch] if(!unique)return;
    InterlockedAdd(g_Counts[0],1);
    uint level=(key.y>>16)&255u;
    [branch] if(level>=g_Parameters.counts.y)return;
    InterlockedAdd(g_Counts[level+1],1);
}
// Scan the markers globally before dispatch. The first particle payload for
// each key survives because the radix sort and compaction are both stable.
[shader("compute")][numthreads(256,1,1)]
void compactUniqueKeys(uint3 id:SV_DispatchThreadID) {
    uint i=invocation(id);[branch] if(i>=g_Parameters.counts.z)return;
    [branch] if(g_Markers[i]==0)return;
    g_OutputKeys[g_Offsets[i]]=g_Keys[i];
}
