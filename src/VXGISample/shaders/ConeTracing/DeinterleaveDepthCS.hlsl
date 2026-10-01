#include "ConeTracingCommon.hlsli"

SamplerState s_Point : REGISTER_SAMPLER(VXGI_CT_POINT_SAMPLER_SLOT, 0);
RWTexture2DArray<float> u_DeinterleavedDepth : REGISTER_UAV(VXGI_CT_COMMON_UAV_SLOT_0, 0);
[numthreads(8, 8, 1)]
void main(in uint3 gfsdk_GroupIdx: SV_GroupID, in uint3 gfsdk_GroupThreadIdx: SV_GroupThreadID, in uint3 gfsdk_GlobalIdx: SV_DispatchThreadID)
{
float depths[16];
#if MSAA_G_BUFFER
[unroll]
for (int y = 0; y < 4; y++)
{
[unroll]
for (int x = 0; x < 4; x++)
{
float depth = g_DepthBuffer.Load(int2(gfsdk_GlobalIdx.xy) * 4 + int2(x, y), 0).x;
depths[y * 4 + x] = depth;
}
}
#else
float2 UV = (float2(gfsdk_GlobalIdx.xy) * 4.0 + 1.0) * g_GBuffer.gbufferSizeInv.xy;
float4 group;
group = g_DepthBuffer.Gather(s_Point, UV, int2(0, 0)); depths[4]  = group.x; depths[5]  = group.y; depths[1]  = group.z; depths[0]  = group.w;
group = g_DepthBuffer.Gather(s_Point, UV, int2(2, 0)); depths[6]  = group.x; depths[7]  = group.y; depths[3]  = group.z; depths[2]  = group.w;
group = g_DepthBuffer.Gather(s_Point, UV, int2(0, 2)); depths[12] = group.x; depths[13] = group.y; depths[9]  = group.z; depths[8]  = group.w;
group = g_DepthBuffer.Gather(s_Point, UV, int2(2, 2)); depths[14] = group.x; depths[15] = group.y; depths[11] = group.z; depths[10] = group.w;
#endif
[unroll]
for(int index = 0; index < 16; index++)
{
float depth = DepthToViewSpaceDepth(RescaleDepth(depths[index], g_GBuffer), g_GBuffer);
u_DeinterleavedDepth[int3(gfsdk_GlobalIdx.xy, index)] = depth;
}
}
