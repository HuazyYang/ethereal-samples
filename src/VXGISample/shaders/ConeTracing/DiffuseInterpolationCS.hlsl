#include "ConeTracingCommon.hlsli"

SamplerState SamplerLinearBorder : REGISTER_SAMPLER(VXGI_CT_LINEAR_BORDER_SAMPLER_SLOT, 0);
Texture2D g_CoarseDiffuseX : REGISTER_SRV(VXGI_CT_TEXTURE_SRV_SLOT_0, 0);
Texture2D g_CoarseDiffuseY : REGISTER_SRV(VXGI_CT_TEXTURE_SRV_SLOT_1, 0);
Texture2D g_CoarseDiffuseZ : REGISTER_SRV(VXGI_CT_TEXTURE_SRV_SLOT_2, 0);
#if MSAA_G_BUFFER
Texture2DMS<float4> g_PrevDepthBuffer : REGISTER_SRV(VXGI_CT_PREV_DEPTH_BUFFER_SRV_SLOT, 0);
Texture2DMS<float4> g_PrevTargetNormal : REGISTER_SRV(VXGI_CT_PREV_NORMAL_BUFFER_SRV_SLOT, 0);
#else
Texture2D g_PrevDepthBuffer : REGISTER_SRV(VXGI_CT_PREV_DEPTH_BUFFER_SRV_SLOT, 0);
Texture2D g_PrevTargetNormal : REGISTER_SRV(VXGI_CT_PREV_NORMAL_BUFFER_SRV_SLOT, 0);
#endif
Texture2D g_PrevDiffuse : REGISTER_SRV(VXGI_CT_TEXTURE_SRV_SLOT_3, 0);
Texture2D t_ScreenSpaceOcclusion : REGISTER_SRV(VXGI_CT_TEXTURE_SRV_SLOT_4, 0);
RWTexture2D<float4> u_InterpolatedTarget : REGISTER_UAV(VXGI_CT_COMMON_UAV_SLOT_0, 0);
RWTexture2D<float> u_RefinementControl : REGISTER_UAV(VXGI_CT_COMMON_UAV_SLOT_1, 0);
RWTexture2D<float> u_RefinementGrid : REGISTER_UAV(VXGI_CT_COMMON_UAV_SLOT_2, 0);
#ifndef DOWNSAMPLE_SCALE
#define DOWNSAMPLE_SCALE 3
#endif
#define INTERPOLATE_RADIUS_LEFT  1
#define INTERPOLATE_RADIUS_RIGHT 1
#define INTERPOLATE_TILE_SIZE_X 16
#define INTERPOLATE_TILE_SIZE_Y 16
#define INTERPOLATE_BUFFER_X ((INTERPOLATE_TILE_SIZE_X + DOWNSAMPLE_SCALE - 1) / DOWNSAMPLE_SCALE + INTERPOLATE_RADIUS_LEFT + INTERPOLATE_RADIUS_RIGHT + 1)
#define INTERPOLATE_BUFFER_Y ((INTERPOLATE_TILE_SIZE_Y + DOWNSAMPLE_SCALE - 1) / DOWNSAMPLE_SCALE + INTERPOLATE_RADIUS_LEFT + INTERPOLATE_RADIUS_RIGHT + 1)
#if AMBIENT_OCCLUSION_MODE
groupshared float4 s_CoarseDiffuse   [INTERPOLATE_BUFFER_Y][INTERPOLATE_BUFFER_X];
groupshared float4 s_CoarseScreenPos [INTERPOLATE_BUFFER_Y][INTERPOLATE_BUFFER_X];
groupshared float2 s_CoarsePacked  [INTERPOLATE_BUFFER_Y][INTERPOLATE_BUFFER_X];
#else
groupshared float4 s_CoarseDiffuseX  [INTERPOLATE_BUFFER_Y][INTERPOLATE_BUFFER_X];
groupshared float4 s_CoarseDiffuseY  [INTERPOLATE_BUFFER_Y][INTERPOLATE_BUFFER_X];
groupshared float4 s_CoarseDiffuseZ  [INTERPOLATE_BUFFER_Y][INTERPOLATE_BUFFER_X];
groupshared float4 s_CoarsePacked  [INTERPOLATE_BUFFER_Y][INTERPOLATE_BUFFER_X];
#endif
#if DOWNSAMPLE_SCALE == 2
#define SCREEN_FILTER_FACTOR 0.20
#elif DOWNSAMPLE_SCALE == 3
#define SCREEN_FILTER_FACTOR 0.12
#else
#define SCREEN_FILTER_FACTOR 0.10
#endif
[numthreads(INTERPOLATE_TILE_SIZE_X, INTERPOLATE_TILE_SIZE_Y, 1)]
void main(in uint3 gfsdk_GroupIdx: SV_GroupID, in uint3 gfsdk_GroupThreadIdx: SV_GroupThreadID, in uint3 gfsdk_GlobalIdx: SV_DispatchThreadID)
{
int2 groupBase = int2(floor((gfsdk_GroupIdx.xy * float2(INTERPOLATE_TILE_SIZE_X, INTERPOLATE_TILE_SIZE_Y) + g_GridOrigin.xy) * g_DownsampleScale.zw) - INTERPOLATE_RADIUS_LEFT);
if(gfsdk_GroupThreadIdx.x < INTERPOLATE_BUFFER_X && gfsdk_GroupThreadIdx.y < INTERPOLATE_BUFFER_Y)
{
int2 coarsePixelPos = groupBase + int2(gfsdk_GroupThreadIdx.xy);
float2 gbufferSamplePos = CoarsePosToGBufferPos(coarsePixelPos);
float4 illumination = float4(0, 0, 0, 0), normal = float4(0, 0, 0, 0);
float3 smoothNormal = float3(0, 0, 0);
float depth = 0, w = 0;
if(all(gbufferSamplePos.xy >= g_GBuffer.viewportOrigin.xy)
&& all(gbufferSamplePos.xy <= g_GBuffer.viewportOrigin.xy + g_GBuffer.viewportSize.xy))
{
GetGeometrySampleFromGBuffer(int2(gbufferSamplePos), 0, g_GBuffer, depth, normal, smoothNormal);
w = DepthToViewSpaceDepth(depth, g_GBuffer);
}
#if AMBIENT_OCCLUSION_MODE
float4 illum = g_CoarseDiffuseX[coarsePixelPos].rgba;
s_CoarsePacked[gfsdk_GroupThreadIdx.y][gfsdk_GroupThreadIdx.x] = float2(w, illum.a);
s_CoarseDiffuse[gfsdk_GroupThreadIdx.y][gfsdk_GroupThreadIdx.x] = float4(illum.rgb, smoothNormal.x);
s_CoarseScreenPos[gfsdk_GroupThreadIdx.y][gfsdk_GroupThreadIdx.x] = float4(gbufferSamplePos.xy, smoothNormal.yz);
#else
float4 illumX = g_CoarseDiffuseX[coarsePixelPos].rgba;
float3 illumY = g_CoarseDiffuseY[coarsePixelPos].rgb;
float3 illumZ = g_CoarseDiffuseZ[coarsePixelPos].rgb;
s_CoarsePacked[gfsdk_GroupThreadIdx.y][gfsdk_GroupThreadIdx.x] = float4(w, illumX.a, gbufferSamplePos.xy);
s_CoarseDiffuseX[gfsdk_GroupThreadIdx.y][gfsdk_GroupThreadIdx.x] = float4(illumX.rgb, smoothNormal.x);
s_CoarseDiffuseY[gfsdk_GroupThreadIdx.y][gfsdk_GroupThreadIdx.x] = float4(illumY.rgb, smoothNormal.y);
s_CoarseDiffuseZ[gfsdk_GroupThreadIdx.y][gfsdk_GroupThreadIdx.x] = float4(illumZ.rgb, smoothNormal.z);
#endif
}
GroupMemoryBarrierWithGroupSync();
float4 sampleNormal;
float3 sampleSmoothNormal;
float sampleDepth;
float2 gbufferSamplePos = gfsdk_GlobalIdx.xy + g_GridOrigin.xy + g_GBuffer.firstSamplePosition;
GetGeometrySampleFromGBuffer(int2(gbufferSamplePos), 0, g_GBuffer, sampleDepth, sampleNormal, sampleSmoothNormal);
float3 samplePosition = DepthToWorldPos(gbufferSamplePos, sampleDepth, g_GBuffer);
float sampleW = DepthToViewSpaceDepth(sampleDepth, g_GBuffer);
float rSampleSize = rcp(VxgiGetMinSampleSizeInVoxels(samplePosition.xyz) * VxgiGetFinestVoxelSize());
float4 combinedIllumination = float4(0, 0, 0, 0);
float combinedWeight = 0;
bool isSubmittedForRefinement = false;
if(!IsInvalidNormal(sampleNormal.xyz) && (sampleDepth < 1.0))
{
for (float i = -INTERPOLATE_RADIUS_LEFT; i <= INTERPOLATE_RADIUS_RIGHT; ++i)
{
for (float j = -INTERPOLATE_RADIUS_LEFT; j <= INTERPOLATE_RADIUS_RIGHT; ++j)
{
float2 coarsePixelPos = floor(gbufferSamplePos * g_DownsampleScale.zw + float2(i, j));
int2 sharedPos = int2(coarsePixelPos - groupBase);
#if AMBIENT_OCCLUSION_MODE
float4 illumDiffuse = s_CoarseDiffuse[sharedPos.y][sharedPos.x];
float4 illumPos = s_CoarseScreenPos[sharedPos.y][sharedPos.x];
float3 illumSmoothNormal = float3(illumDiffuse.w, illumPos.z, illumPos.w);
float2 packedData = s_CoarsePacked[sharedPos.y][sharedPos.x];
float illumW = packedData.x;
float confidence = packedData.y;
#else
float4 illumDiffuseX = s_CoarseDiffuseX[sharedPos.y][sharedPos.x];
float4 illumDiffuseY = s_CoarseDiffuseY[sharedPos.y][sharedPos.x];
float4 illumDiffuseZ = s_CoarseDiffuseZ[sharedPos.y][sharedPos.x];
float3 illumSmoothNormal = float3(illumDiffuseX.w, illumDiffuseY.w, illumDiffuseZ.w);
float4 packedData = s_CoarsePacked[sharedPos.y][sharedPos.x];
float illumW = packedData.x;
float confidence = packedData.y;
float2 illumPos = packedData.zw;
#endif
float distanceWeight = rcp(abs(illumW - sampleW) + VxgiGetFinestVoxelSize()) * VxgiGetFinestVoxelSize();
float screenWeight = saturate(1 - SCREEN_FILTER_FACTOR * length(illumPos.xy - gbufferSamplePos.xy));
float normalWeight = pow(max(dot(illumSmoothNormal.xyz, sampleSmoothNormal.xyz), 0), 20);
float weight = normalWeight * distanceWeight * screenWeight;
#if AMBIENT_OCCLUSION_MODE
combinedIllumination.rgb += weight * max(float3(0, 0, 0), dot(illumDiffuse.xyz, sampleNormal.xyz)) * g_AmbientColor.rgb;
#else
combinedIllumination.rgb += weight * max(float3(0, 0, 0),
illumDiffuseX.rgb * sampleNormal.x +
illumDiffuseY.rgb * sampleNormal.y +
illumDiffuseZ.rgb * sampleNormal.z);
#endif
combinedIllumination.a += confidence * weight;
combinedWeight += weight;
}
}
if (g_SSAO_RadiusWorld > 0)
{
combinedIllumination.rgb *= t_ScreenSpaceOcclusion[int2(gbufferSamplePos.xy)].rrr;
}
float4 reprojectedColor = float4(0, 0, 0, 0);
float reprojectedWeight = 0;
if(g_TemporalReprojectionWeight > 0)
{
float2 uv = PixelToUV(gbufferSamplePos, g_GBuffer);
reprojectedColor = GetColorFromPreviousFrame(uv, sampleDepth, sampleNormal.xyz, g_PrevDepthBuffer, g_PrevTargetNormal, g_PrevDiffuse, reprojectedWeight);
if(reprojectedWeight > g_InterpolationWeightThreshold)
{
const float processedPixels = VxgiSqr(INTERPOLATE_RADIUS_LEFT + INTERPOLATE_RADIUS_RIGHT + 1);
float factor = g_TemporalReprojectionWeight * processedPixels / reprojectedWeight;
combinedIllumination += reprojectedColor * factor;
combinedWeight += reprojectedWeight * factor;
}
}
if(combinedWeight > g_InterpolationWeightThreshold)
{
combinedIllumination /= combinedWeight;
}
else if (bool(g_EnableRefinement))
{
float value = 1;
u_RefinementControl[int2(gbufferSamplePos)] = value;
u_RefinementGrid[int2(gbufferSamplePos / TRACING_REFINEMENT_GRID_SIZE)] = value;
combinedIllumination = float4(0, 0, 0, 0);
isSubmittedForRefinement = true;
}
}
if (!isSubmittedForRefinement && any(g_BackgroundColor.rgb!= float3scalar(0)))
{
combinedIllumination.rgb = lerp(g_BackgroundColor.rgb, combinedIllumination.rgb, combinedIllumination.a);
}
combinedIllumination = InfNaNOutputGuard(combinedIllumination);
u_InterpolatedTarget[int2(gbufferSamplePos)] = combinedIllumination;
}
