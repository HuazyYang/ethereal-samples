// Expanded sparse subcell bins for density/level-set point gathers.
// Copyright 2017 NVIDIA Corporation. SPDX-License-Identifier: Apache-2.0
#include "GVDBTypes.slang"
struct SubcellParameters {VolumeInfo volume;uint4 counts;float4 translationAndRadius;uint4 layout;};
ConstantBuffer<SubcellParameters> g_Parameters:register(b0);
StructuredBuffer<float4> g_Positions:register(t0);
StructuredBuffer<uint> g_Colors:register(t1);
ByteAddressBuffer g_Nodes:register(t2);
ByteAddressBuffer g_Children:register(t3);
StructuredBuffer<LevelInfo> g_Levels:register(t4);
StructuredBuffer<uint> g_CellOffsets:register(t5);
RWStructuredBuffer<uint> g_CellCounts:register(u0);
RWStructuredBuffer<float4> g_OutputPositions:register(u1);
RWStructuredBuffer<uint> g_OutputColors:register(u2);
[shader("compute")][numthreads(256,1,1)]
void clearSubcellCounts(uint3 id:SV_DispatchThreadID){uint index=id.x+id.y*(65535u*256u);if(index<g_Parameters.layout.y)g_CellCounts[index]=0;}
void binPoint(uint pointIndex,bool scatter){
    [branch] if(pointIndex>=g_Parameters.counts.x)return;
    float4 point=g_Positions[pointIndex];point.xyz+=g_Parameters.translationAndRadius.xyz;
    [branch] if(any((asuint(point.xyz)&uint3(0x7fffffffu))>=uint3(0x7f800000u)))return;
    float width=float(g_Parameters.counts.z),radius=g_Parameters.translationAndRadius.w;
    float3 first=max(floor((point.xyz-radius)/width),floor(g_Parameters.volume.boundsMin.xyz/width));
    float3 last=min(floor((point.xyz+1+radius)/width),ceil(g_Parameters.volume.boundsMax.xyz/width)-1);
    [branch] if(any(first>last)||any(first< -2147483000.0)||any(last>2147483000.0))return;
    int3 lower=int3(first),upper=int3(last);
    uint color=g_Parameters.layout.z!=0?g_Colors[pointIndex]:0;
    [loop] for(int z=lower.z;z<=upper.z;++z)[loop] for(int y=lower.y;y<=upper.y;++y)[loop] for(int x=lower.x;x<=upper.x;++x){
        float3 origin=float3(x,y,z)*width;VDBNode leaf;uint leafIndex;
        bool found=findLeaf(g_Nodes,g_Children,g_Levels,g_Parameters.volume,origin+.5,leaf,leafIndex);
        [branch] if(!found)continue;
        uint3 local=uint3((origin-float3(leaf.position))/width);
        uint cell=leafIndex*g_Parameters.counts.w+flatten3(local,uint3(g_Parameters.layout.x));
        uint offset;InterlockedAdd(g_CellCounts[cell],1,offset);
        if(scatter){uint destination=g_CellOffsets[cell]+offset;g_OutputPositions[destination]=point;g_OutputColors[destination]=color;}
    }
}
[shader("compute")][numthreads(256,1,1)]
void countSubcellPoints(uint3 id:SV_DispatchThreadID){binPoint(id.x+id.y*(65535u*256u),false);}
[shader("compute")][numthreads(256,1,1)]
void scatterSubcellPoints(uint3 id:SV_DispatchThreadID){binPoint(id.x+id.y*(65535u*256u),true);}
