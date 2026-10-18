// SPDX-License-Identifier: Apache-2.0
ConstantBuffer<uint4> g_Counts : register(b0);
StructuredBuffer<float> g_Density : register(t0);
StructuredBuffer<uint> g_Color : register(t1);
RWStructuredBuffer<float> g_Atlas : register(u0);
RWByteAddressBuffer g_RawColor : register(u1);
[shader("compute")]
[numthreads(256,1,1)]
void publishFluidAtlas(uint3 group:SV_GroupID,uint lane:SV_GroupThreadID) {
    uint index=(group.y*65535+group.x)*256+lane;
    if(index>=g_Counts.x)return;
    g_Atlas[index]=g_Density[index];
    [branch] if(g_Counts.y!=0)g_RawColor.Store(index*4,g_Color[index]);
}
