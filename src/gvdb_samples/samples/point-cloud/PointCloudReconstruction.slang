// Legacy normalized ushort3 subcells and explicit volume publication.
// Copyright 2017 NVIDIA Corporation. SPDX-License-Identifier: Apache-2.0
struct Parameters {uint4 counts;float4 minimum;float4 range;};
ConstantBuffer<Parameters> g_Parameters:register(b0);
ByteAddressBuffer g_Packed:register(t0);
StructuredBuffer<float> g_Source:register(t1);
RWStructuredBuffer<float4> g_Positions:register(u0);
RWStructuredBuffer<float> g_Atlas:register(u1);
uint ushortAt(uint byteOffset){return (g_Packed.Load(byteOffset&~3u)>>((byteOffset&2u)*8))&65535u;}
[shader("compute")][numthreads(256,1,1)]
void decodeSubcellPoints(uint3 id:SV_DispatchThreadID){
    uint i=id.x+id.y*(65535u*256u);[branch]if(i>=g_Parameters.counts.x)return;
    uint offset=i*6;float3 q=float3(ushortAt(offset),ushortAt(offset+2),ushortAt(offset+4));
    g_Positions[i]=float4((q/65535.0)*g_Parameters.range.xyz+g_Parameters.minimum.xyz,1);
}
[shader("compute")][numthreads(256,1,1)]
void publishPointCloudAtlas(uint3 id:SV_DispatchThreadID){
    uint i=id.x+id.y*(65535u*256u);if(i<g_Parameters.counts.x)g_Atlas[i]=g_Source[i];
}
