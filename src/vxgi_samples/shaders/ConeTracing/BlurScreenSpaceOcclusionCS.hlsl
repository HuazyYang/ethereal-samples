#include "ConeTracingCommon.hlsli"

SamplerState s_Point : REGISTER_SAMPLER(VXGI_CT_POINT_SAMPLER_SLOT, 0);
Texture2DArray t_DeinterleavedDepth : REGISTER_SRV(VXGI_CT_TEXTURE_SRV_SLOT_0, 0);
Texture2DArray t_DeinterleavedSSAO : REGISTER_SRV(VXGI_CT_TEXTURE_SRV_SLOT_1, 0);
RWTexture2D<float> u_RenderTarget : REGISTER_UAV(VXGI_CT_COMMON_UAV_SLOT_0, 0);
void divrem(float a, float b, out float div, out float rem)
{
a = (a + 0.5) / b;
div = floor(a);
rem = floor(frac(a) * b);
}
groupshared float2 s_DepthAndSSAO[24][24];
[numthreads(16, 16, 1)]
void main(in uint3 gfsdk_GroupIdx: SV_GroupID, in uint3 gfsdk_GroupThreadIdx: SV_GroupThreadID, in uint3 gfsdk_GlobalIdx: SV_DispatchThreadID)
{
int linearIdx = int((gfsdk_GroupThreadIdx.y << 4) + gfsdk_GroupThreadIdx.x);
if (linearIdx < 144)
{
float2 offsetUVf;
float a, slice;
divrem(linearIdx, 3.0, a, offsetUVf.x);
divrem(a, 3.0, slice, offsetUVf.y);
offsetUVf *= 8;
int2 offsetUV = int2(offsetUVf);
float2 UV = (float2(gfsdk_GroupIdx.xy) * 16 + offsetUVf) * g_GBuffer.gbufferSizeInv.xy;
float4 depths = t_DeinterleavedDepth.Gather(s_Point, float3(UV, slice), int2(0, 0));
float4 occlusions = t_DeinterleavedSSAO.Gather(s_Point, float3(UV, slice), int2(0, 0));
offsetUV.x += int(slice) & 3;
offsetUV.y += int(slice) >> 2;
s_DepthAndSSAO[offsetUV.y + 4][offsetUV.x + 0] = float2(depths.x, occlusions.x);
s_DepthAndSSAO[offsetUV.y + 4][offsetUV.x + 4] = float2(depths.y, occlusions.y);
s_DepthAndSSAO[offsetUV.y + 0][offsetUV.x + 4] = float2(depths.z, occlusions.z);
s_DepthAndSSAO[offsetUV.y + 0][offsetUV.x + 0] = float2(depths.w, occlusions.w);
}
GroupMemoryBarrierWithGroupSync();
float totalAO = 0;
float totalWeight = 0;
float pixelDepth = s_DepthAndSSAO[gfsdk_GroupThreadIdx.y + 4][gfsdk_GroupThreadIdx.x + 4].x;
float rcpPixelDepth = rcp(pixelDepth);
int2 filterOffset;
for (filterOffset.y = 3; filterOffset.y <= 6; filterOffset.y++)
for (filterOffset.x = 3; filterOffset.x <= 6; filterOffset.x++)
{
float2 sampleDAO = s_DepthAndSSAO[gfsdk_GroupThreadIdx.y + filterOffset.y][gfsdk_GroupThreadIdx.x + filterOffset.x].xy;
float weight = saturate(1.0 - abs(pixelDepth - sampleDAO.x) * rcpPixelDepth * 50);
totalAO += sampleDAO.y * weight;
totalWeight += weight;
}
totalAO *= rcp(totalWeight);
u_RenderTarget[int2(gfsdk_GlobalIdx.xy)] = totalAO;
}
