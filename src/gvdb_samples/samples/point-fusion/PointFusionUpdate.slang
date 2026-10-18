// Persistent atlas remapping independent of root, leaf and atlas slot changes.
// SPDX-License-Identifier: Apache-2.0
#include "GVDBTypes.slang"
struct UpdateParameters {VolumeInfo previous,current;uint4 operation;};
ConstantBuffer<UpdateParameters> g_Parameters:register(b0);
ByteAddressBuffer g_OldNodes:register(t0);
ByteAddressBuffer g_OldChildren:register(t1);
StructuredBuffer<LevelInfo> g_OldLevels:register(t2);
StructuredBuffer<float> g_OldScalar:register(t3);
ByteAddressBuffer g_OldColor:register(t4);
StructuredBuffer<int4> g_Map:register(t5);
StructuredBuffer<float> g_Scalar:register(t6);
StructuredBuffer<uint> g_Color:register(t7);
RWStructuredBuffer<float> g_OutputScalar:register(u0);
RWStructuredBuffer<uint> g_OutputColor:register(u1);
RWByteAddressBuffer g_RawColor:register(u2);
[shader("compute")][numthreads(256,1,1)]
void remapFusionAtlas(uint3 id:SV_DispatchThreadID) {
    uint index=id.x+id.y*(65535u*256u);[branch] if(index>=g_Parameters.operation.z)return;
    float value=3;uint color=0;
    float3 world;uint3 voxel=unflatten3(index,g_Parameters.current.atlasResolutionAndApron.xyz);
    bool active=atlasToWorld(g_Map,g_Parameters.current,voxel,world);
    [branch] if(active&&g_Parameters.operation.x!=0){
        VDBNode leaf;uint leafIndex;
        if(findLeaf(g_OldNodes,g_OldChildren,g_OldLevels,g_Parameters.previous,world,leaf,leafIndex)){
            uint3 old=uint3(leaf.atlasPosition+int3(floor(world))-leaf.position);
            uint source=flatten3(old,g_Parameters.previous.atlasResolutionAndApron.xyz);value=g_OldScalar[source];
            if(g_Parameters.operation.y!=0)color=g_OldColor.Load(source*4);
        }
    }
    g_OutputScalar[index]=value;g_OutputColor[index]=color;
}
[shader("compute")][numthreads(256,1,1)]
void publishFusionAtlas(uint3 id:SV_DispatchThreadID) {
    uint index=id.x+id.y*(65535u*256u);[branch] if(index>=g_Parameters.operation.z)return;
    g_OutputScalar[index]=g_Scalar[index];
    [branch] if(g_Parameters.operation.y==0)return;
    g_RawColor.Store(index*4,g_Color[index]);
}
