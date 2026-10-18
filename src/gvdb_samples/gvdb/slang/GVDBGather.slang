// Deterministic point gathers replacing CUDA subcell texture writes.
// Copyright 2017 NVIDIA Corporation. SPDX-License-Identifier: Apache-2.0
#include "GVDBTypes.slang"
struct GatherCell {int4 worldOriginAndWidth;uint4 atlasOrigin;uint4 points;};
struct GatherParameters {uint4 counts;float4 kernel;uint4 atlasResolution;};
ConstantBuffer<GatherParameters> g_Parameters:register(b0);
StructuredBuffer<float4> g_Positions:register(t0);
StructuredBuffer<uint> g_Colors:register(t1);
StructuredBuffer<GatherCell> g_Cells:register(t2);
StructuredBuffer<float> g_Source:register(t3);
StructuredBuffer<uint> g_SourceColors:register(t4);
RWStructuredBuffer<float> g_Destination:register(u0);
RWStructuredBuffer<uint> g_DestinationColors:register(u1);
float4 unpackColor(uint packed){return float4(packed&255u,(packed>>8)&255u,(packed>>16)&255u,packed>>24)/255.0;}
uint packColor(float4 color){uint4 bytes=uint4(color*255.0)&uint4(255u);return bytes.x|(bytes.y<<8)|(bytes.z<<16)|(bytes.w<<24);}
[shader("compute")][numthreads(256,1,1)]
void initializeGather(uint3 id:SV_DispatchThreadID) {
    uint index=id.x+id.y*(65535u*256u);if(index>=g_Parameters.counts.y)return;
    g_Destination[index]=g_Parameters.counts.w!=0?g_Source[index]:g_Parameters.kernel.y;
    g_DestinationColors[index]=g_Parameters.counts.w!=0?g_SourceColors[index]:0;
}
void gatherCell(uint3 local,uint cellIndex,bool levelSet) {
    [branch] if(cellIndex>=g_Parameters.counts.x)return;
    GatherCell cell=g_Cells[cellIndex];
    [branch] if(any(local>=uint3(cell.worldOriginAndWidth.w)))return;
    uint index=flatten3(cell.atlasOrigin.xyz+local,g_Parameters.atlasResolution.xyz);
    float3 position=float3(cell.worldOriginAndWidth.xyz)+float3(local);
    bool accumulate=g_Parameters.counts.w!=0;
    float value=accumulate?g_Source[index]:g_Parameters.kernel.y;
    float4 color=accumulate?unpackColor(g_SourceColors[index]):float4(0,0,0,1);
    for(uint j=0;j<cell.points.y;++j){
        uint pointIndex=cell.points.x+j;
        float distance=length(g_Positions[pointIndex].xyz-position);
        if(levelSet)value=min(value,distance-g_Parameters.kernel.x);
        else value=max(value,g_Parameters.kernel.x-distance);
        if(g_Parameters.counts.z!=0)color+=unpackColor(g_Colors[pointIndex]);
    }
    g_Destination[index]=value;
    uint contributors=cell.points.y+(accumulate?1u:0u);
    [branch] if(g_Parameters.counts.z==0)return;
    [branch] if(contributors==0)return;
    g_DestinationColors[index]=packColor(color/float(contributors));
}
[shader("compute")][numthreads(8,8,8)]
void gatherDensity(uint3 local:SV_GroupThreadID,uint3 group:SV_GroupID){gatherCell(local,group.x+group.y*65535u,false);}
[shader("compute")][numthreads(8,8,8)]
void gatherLevelSet(uint3 local:SV_GroupThreadID,uint3 group:SV_GroupID){gatherCell(local,group.x+group.y*65535u,true);}
