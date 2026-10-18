// Sparse MAC grid sampling and PIC/FLIP velocity transfer.
// SPDX-License-Identifier: Apache-2.0
#include "GVDBTypes.slang"
struct VelocityParameters {VolumeInfo volume;uint4 counts;float4 motion;};
ConstantBuffer<VelocityParameters> g_Parameters:register(b0);
StructuredBuffer<float4> g_Positions:register(t0);
StructuredBuffer<float4> g_Velocities:register(t1);
ByteAddressBuffer g_Nodes:register(t2);
ByteAddressBuffer g_Children:register(t3);
StructuredBuffer<LevelInfo> g_Levels:register(t4);
StructuredBuffer<float> g_X:register(t5);
StructuredBuffer<float> g_Y:register(t6);
StructuredBuffer<float> g_Z:register(t7);
StructuredBuffer<float> g_OldX:register(t8);
StructuredBuffer<float> g_OldY:register(t9);
StructuredBuffer<float> g_OldZ:register(t10);
RWStructuredBuffer<float4> g_OutputVelocities:register(u0);
RWStructuredBuffer<float4> g_OutputPositions:register(u1);
RWStructuredBuffer<float4> g_SampledVelocity:register(u2);
float component(uint index,uint axis,bool previous) {
    if(previous){if(axis==0)return g_OldX[index];if(axis==1)return g_OldY[index];return g_OldZ[index];}
    if(axis==0)return g_X[index];if(axis==1)return g_Y[index];return g_Z[index];
}
float sampleComponent(float3 atlasPosition,uint axis,bool previous) {
    // MAC faces: x=(i,j+.5,k+.5), y=(i+.5,j,k+.5), z=(i+.5,j+.5,k).
    float3 offset=axis==0?float3(0,.5,.5):(axis==1?float3(.5,0,.5):float3(.5,.5,0));
    float3 coordinate=atlasPosition-offset;int3 lower=int3(floor(coordinate));float3 fraction=coordinate-float3(lower);
    uint3 resolution=g_Parameters.volume.atlasResolutionAndApron.xyz;float result=0;
    [unroll] for(uint corner=0;corner<8;++corner) {
        uint3 bit=uint3(corner&1u,(corner>>1)&1u,(corner>>2)&1u);
        float3 weight=lerp(1-fraction,fraction,float3(bit));
        uint3 voxel=uint3(clamp(lower+int3(bit),int3(0),int3(resolution)-1));
        result+=weight.x*weight.y*weight.z*component(flatten3(voxel,resolution),axis,previous);
    }
    return result;
}
bool sampleVelocity(float3 position,bool previous,out float3 velocity) {
    velocity=float3(0);
    if(any((asuint(position)&uint3(0x7fffffffu))>=uint3(0x7f800000u)))return false;
    VDBNode leaf;uint leafIndex;
    if(!findLeaf(g_Nodes,g_Children,g_Levels,g_Parameters.volume,position,leaf,leafIndex))return false;
    float3 atlasPosition=float3(leaf.atlasPosition)+position-float3(leaf.position);
    velocity=float3(sampleComponent(atlasPosition,0,previous),sampleComponent(atlasPosition,1,previous),sampleComponent(atlasPosition,2,previous));return true;
}
[shader("compute")][numthreads(256,1,1)]
void sampleGridVelocity(uint3 id:SV_DispatchThreadID) {
    uint i=id.x+id.y*(65535u*256u);[branch] if(i>=g_Parameters.counts.x)return;
    float3 sampled;bool valid=sampleVelocity(g_Positions[i].xyz,false,sampled);
    g_SampledVelocity[i]=float4(sampled,valid?1:0);
}
[shader("compute")][numthreads(256,1,1)]
void updateFlipParticles(uint3 id:SV_DispatchThreadID) {
    uint i=id.x+id.y*(65535u*256u);[branch] if(i>=g_Parameters.counts.x)return;
    float4 position=g_Positions[i],oldVelocity=g_Velocities[i];float3 current,previous;
    bool valid=sampleVelocity(position.xyz,false,current);
    sampleVelocity(position.xyz,true,previous);
    float3 velocity=valid?lerp(current,oldVelocity.xyz+current-previous,g_Parameters.motion.y):oldVelocity.xyz;
    if(g_Parameters.counts.y!=0&&valid) {
        float3 midpointVelocity;sampleVelocity(position.xyz+current*(.5*g_Parameters.motion.x),false,midpointVelocity);
        position.xyz+=midpointVelocity*g_Parameters.motion.x;
    }
    g_OutputVelocities[i]=float4(velocity,oldVelocity.w);g_OutputPositions[i]=position;
}
