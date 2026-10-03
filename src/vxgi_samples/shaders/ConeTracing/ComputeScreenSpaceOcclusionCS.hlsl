#include "ConeTracingCommon.hlsli"

/*
static const float2 g_SamplePositions[] = {
float2(-0.3935238f, 0.7530643f),
float2(-0.3022015f, 0.297664f),
float2(0.09813362f, 0.192451f),
float2(-0.7593753f, 0.518795f),
float2(0.2293134f, 0.7607011f),
float2(0.6505286f, 0.6297367f),
float2(0.5322764f, 0.2350069f),
float2(0.8581018f, -0.01624052f),
float2(-0.6928226f, 0.07119545f),
float2(-0.3114384f, -0.3017288f),
float2(0.2837671f, -0.179743f),
float2(-0.3093514f, -0.749256f),
float2(-0.7386893f, -0.5215692f),
float2(0.3988827f, -0.617012f),
float2(0.8114883f, -0.458026f),
float2(0.08265103f, -0.8939569f)
};
*/
static const float2 g_SamplePositions[] = {
float2(-0.016009523, -0.10995169),
float2(-0.159746436, 0.047527402),
float2(0.09339819, 0.201641995),
float2(0.232600698, 0.151846663),
float2(-0.220531935, -0.24995355),
float2(-0.251498143, 0.29661971),
float2(0.376870668, .23558303),
float2(0.201175979, 0.457742532),
float2(-0.535502966, -0.147913991),
float2(-0.076133435, 0.606350138),
float2(0.666537538, 0.013120791),
float2(-0.118107615, -0.712499494),
float2(-0.740973793, 0.236423582),
float2(0.365057451, .749117816),
float2(0.734614792, 0.500464349),
float2(-0.638657704, -0.695766948)
};
float ComputeAO(float3 P, float3 N, float3 S, float InvR2)
{
float3 V = S - P;
float VdotV = dot(V, V);
float NdotV = dot(N, V) * rsqrt(VdotV);
float lambertian = saturate(NdotV - g_SSAO_SurfaceBias);
float falloff = saturate(1 - VdotV * InvR2);
return saturate(1.0 - lambertian * falloff * g_SSAO_CoarseAO);
}
float rand(float2 co)
{
return frac(sin(dot(co.xy, float2(12.9898,78.233))) * 43758.5453);
}
float3 ViewDepthToViewPos(float2 UV, float viewDepth)
{
UV = UV * g_GBuffer.uvToView.xy + g_GBuffer.uvToView.zw;
return float3(UV * viewDepth, viewDepth);
}
Texture2DArray t_DeinterleavedDepth : REGISTER_SRV(VXGI_CT_TEXTURE_SRV_SLOT_0, 0);
Texture2D t_Randoms : REGISTER_SRV(VXGI_CT_TEXTURE_SRV_SLOT_1, 0);
SamplerState s_Point : REGISTER_SAMPLER(VXGI_CT_POINT_SAMPLER_SLOT, 0);
RWTexture2DArray<float> u_RenderTarget : REGISTER_UAV(VXGI_CT_COMMON_UAV_SLOT_0, 0);
[numthreads(8, 8, 1)]
void main(in uint3 gfsdk_GroupIdx: SV_GroupID, in uint3 gfsdk_GroupThreadIdx: SV_GroupThreadID, in uint3 gfsdk_GlobalIdx: SV_DispatchThreadID)
{
int2 quarterResPixelPos = int2(gfsdk_GlobalIdx.xy);
int sliceIndex = int(gfsdk_GlobalIdx.z);
int2 pixelPos = (quarterResPixelPos.xy << 2) + int2(sliceIndex & 3, sliceIndex >> 2);
float2 pixelUV = PixelToUV(float2(pixelPos + 0.5), g_GBuffer);
float3 pixelNormal = RescaleNormal(Load2D(g_TargetNormal, pixelPos, 0).xyz, g_GBuffer);
pixelNormal = normalize(mul(float4(pixelNormal, 0), g_GBuffer.viewMatrix).xyz);
float pixelViewDepth = t_DeinterleavedDepth[int3(quarterResPixelPos, sliceIndex)].x;
float3 pixelViewPos = ViewDepthToViewPos(pixelUV, pixelViewDepth);
float radiusWorld = g_SSAO_RadiusWorld * max(1.0, pixelViewDepth * g_SSAO_rBackgroundViewDepth);
float radiusPixels = radiusWorld * g_GBuffer.radiusToScreen / pixelViewDepth;
float result = 0;
if (radiusPixels > 1)
{
float2 radiusUV = radiusPixels * g_GBuffer.viewportSizeInv;
float invRadiusWorld2 = rcp(radiusWorld * radiusWorld);
float angle = t_Randoms[pixelPos.xy & 3].x * 3.1415;
float2 sincos = float2(sin(angle), cos(angle));
int numSamples = 16;
float numValidSamples = 0;
[unroll]
for (int nSample = 0; nSample < numSamples; nSample++)
{
float2 sampleOffset = g_SamplePositions[nSample];
sampleOffset = float2(sampleOffset.x * sincos.y - sampleOffset.y * sincos.x, sampleOffset.x * sincos.x + sampleOffset.y * sincos.y);
float2 sampleUV = pixelUV.xy + sampleOffset * radiusUV;
float2 textureUV = sampleUV;
float sampleViewDepth = t_DeinterleavedDepth.SampleLevel(s_Point, float3(textureUV, sliceIndex), 0).x;
if (sampleViewDepth > 0 && sampleViewDepth != pixelViewDepth)
{
float3 sampleViewPos = ViewDepthToViewPos(sampleUV, sampleViewDepth);
float AO = ComputeAO(pixelViewPos, pixelNormal, sampleViewPos, invRadiusWorld2);
result += AO;
numValidSamples += 1;
}
}
if (numValidSamples > 0)
result = saturate(result * rcp(numValidSamples));
else
result = 1;
result = pow(result, g_SSAO_PowerExponent);
}
else
{
result = 1;
}
u_RenderTarget[int3(quarterResPixelPos.xy, sliceIndex)] = result;
}
