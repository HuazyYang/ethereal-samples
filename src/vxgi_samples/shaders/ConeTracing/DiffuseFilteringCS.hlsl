#include "ConeTracingCommon.hlsli"

Texture2D g_CoarseDiffuseX : REGISTER_SRV(VXGI_CT_TEXTURE_SRV_SLOT_0, 0);
Texture2D g_CoarseDiffuseY : REGISTER_SRV(VXGI_CT_TEXTURE_SRV_SLOT_1, 0);
Texture2D g_CoarseDiffuseZ : REGISTER_SRV(VXGI_CT_TEXTURE_SRV_SLOT_2, 0);
RWTexture2D<float4> u_CoarseDiffuseX : REGISTER_UAV(VXGI_CT_COMMON_UAV_SLOT_0, 0);
RWTexture2D<float4> u_CoarseDiffuseY : REGISTER_UAV(VXGI_CT_COMMON_UAV_SLOT_1, 0);
RWTexture2D<float4> u_CoarseDiffuseZ : REGISTER_UAV(VXGI_CT_COMMON_UAV_SLOT_2, 0);
#define INTERPOLATE_RADIUS_LEFT  1
#define INTERPOLATE_RADIUS_RIGHT 2
#define INTERPOLATE_TILE_SIZE_X 8
#define INTERPOLATE_TILE_SIZE_Y 8
#define INTERPOLATE_BUFFER_X (INTERPOLATE_TILE_SIZE_X + INTERPOLATE_RADIUS_LEFT + INTERPOLATE_RADIUS_RIGHT)
#define INTERPOLATE_BUFFER_Y (INTERPOLATE_TILE_SIZE_Y + INTERPOLATE_RADIUS_LEFT + INTERPOLATE_RADIUS_RIGHT)
#if AMBIENT_OCCLUSION_MODE
groupshared float4 s_CoarsePosition  [INTERPOLATE_BUFFER_Y][INTERPOLATE_BUFFER_X];
groupshared float4 s_CoarseDiffuse   [INTERPOLATE_BUFFER_Y][INTERPOLATE_BUFFER_X];
#else
groupshared float2 s_CoarsePosition  [INTERPOLATE_BUFFER_Y][INTERPOLATE_BUFFER_X];
groupshared float4 s_CoarseDiffuseX  [INTERPOLATE_BUFFER_Y][INTERPOLATE_BUFFER_X];
groupshared float4 s_CoarseDiffuseY  [INTERPOLATE_BUFFER_Y][INTERPOLATE_BUFFER_X];
groupshared float4 s_CoarseDiffuseZ  [INTERPOLATE_BUFFER_Y][INTERPOLATE_BUFFER_X];
#endif
[numthreads(INTERPOLATE_TILE_SIZE_X, INTERPOLATE_TILE_SIZE_Y, 1)]
void main(in uint3 gfsdk_GroupIdx: SV_GroupID, in uint3 gfsdk_GroupThreadIdx: SV_GroupThreadID, in uint3 gfsdk_GlobalIdx: SV_DispatchThreadID)
{
int2 groupBase = int2(gfsdk_GlobalIdx.xy - gfsdk_GroupThreadIdx.xy)
+ int2(floor(g_GridOrigin.xy * g_DownsampleScale.zw));
float linearIdx = float(gfsdk_GroupThreadIdx.y) * INTERPOLATE_TILE_SIZE_X + float(gfsdk_GroupThreadIdx.x);
for(int n = 0; n < 2; n++)
{
float offsetLinearIdx = linearIdx + n * INTERPOLATE_TILE_SIZE_X * INTERPOLATE_TILE_SIZE_Y;
if(offsetLinearIdx >= INTERPOLATE_BUFFER_X * INTERPOLATE_BUFFER_Y) continue;
int2 reorderIdx;
offsetLinearIdx = (offsetLinearIdx + 0.25) / float(INTERPOLATE_BUFFER_X);   reorderIdx.x = int(frac(offsetLinearIdx) * INTERPOLATE_BUFFER_X);
offsetLinearIdx = floor(offsetLinearIdx);                                   reorderIdx.y = int(offsetLinearIdx);
int2 coarsePixelPos = groupBase + reorderIdx - INTERPOLATE_RADIUS_LEFT;
int2 gbufferSamplePos = int2(CoarsePosToGBufferPos(coarsePixelPos));
float4 normal = float4(0, 0, 0, 0);
float3 smoothNormal = float3(0, 0, 0);
float depth = 0, w = 0;
if(all(gbufferSamplePos.xy >= g_GBuffer.viewportOrigin.xy)
&& all(gbufferSamplePos.xy < g_GBuffer.viewportOrigin.xy + g_GBuffer.viewportSize.xy))
{
GetGeometrySampleFromGBuffer(gbufferSamplePos, 0, g_GBuffer, depth, normal, smoothNormal);
w = DepthToViewSpaceDepth(depth, g_GBuffer);
}
#if AMBIENT_OCCLUSION_MODE
float4 illum = g_CoarseDiffuseX[coarsePixelPos].rgba;
s_CoarsePosition[reorderIdx.y][reorderIdx.x] = float4(w, smoothNormal.xyz);
s_CoarseDiffuse[reorderIdx.y][reorderIdx.x] = illum.rgba;
#else
float4 illumX = g_CoarseDiffuseX[coarsePixelPos].rgba;
float3 illumY = g_CoarseDiffuseY[coarsePixelPos].rgb;
float3 illumZ = g_CoarseDiffuseZ[coarsePixelPos].rgb;
s_CoarsePosition[reorderIdx.y][reorderIdx.x] = float2(w, illumX.a);
s_CoarseDiffuseX[reorderIdx.y][reorderIdx.x] = float4(illumX.rgb, smoothNormal.x);
s_CoarseDiffuseY[reorderIdx.y][reorderIdx.x] = float4(illumY.rgb, smoothNormal.y);
s_CoarseDiffuseZ[reorderIdx.y][reorderIdx.x] = float4(illumZ.rgb, smoothNormal.z);
#endif
}
GroupMemoryBarrierWithGroupSync();
#if AMBIENT_OCCLUSION_MODE
float4 color = float4(0, 0, 0, 0);
#else
float4 colorX = float4(0, 0, 0, 0);
float4 colorY = float4(0, 0, 0, 0);
float4 colorZ = float4(0, 0, 0, 0);
#endif
int2 sharedIndex = int2(gfsdk_GroupThreadIdx.xy) + INTERPOLATE_RADIUS_LEFT;
float w = s_CoarsePosition[sharedIndex.y][sharedIndex.x].x;
float3 smoothNormal;
#if AMBIENT_OCCLUSION_MODE
smoothNormal.xyz = s_CoarsePosition[sharedIndex.y][sharedIndex.x].yzw;
#else
smoothNormal.x = s_CoarseDiffuseX[sharedIndex.y][sharedIndex.x].w;
smoothNormal.y = s_CoarseDiffuseY[sharedIndex.y][sharedIndex.x].w;
smoothNormal.z = s_CoarseDiffuseZ[sharedIndex.y][sharedIndex.x].w;
#endif
float combinedWeight = 0;
if (!IsInvalidNormal(smoothNormal.xyz))
{
for (int i = -INTERPOLATE_RADIUS_LEFT; i <= INTERPOLATE_RADIUS_RIGHT; ++i)
{
for (int j = -INTERPOLATE_RADIUS_LEFT; j <= INTERPOLATE_RADIUS_RIGHT; ++j)
{
float weight = 1;
if (i != 0 || j != 0)
{
float _w = s_CoarsePosition[sharedIndex.y + j][sharedIndex.x + i].x;
float3 _smoothNormal;
#if AMBIENT_OCCLUSION_MODE
_smoothNormal.xyz = s_CoarsePosition[sharedIndex.y + j][sharedIndex.x + i].yzw;
#else
_smoothNormal.x = s_CoarseDiffuseX[sharedIndex.y + j][sharedIndex.x + i].w;
_smoothNormal.y = s_CoarseDiffuseY[sharedIndex.y + j][sharedIndex.x + i].w;
_smoothNormal.z = s_CoarseDiffuseZ[sharedIndex.y + j][sharedIndex.x + i].w;
#endif
float distanceWeight = rcp(abs(w - _w) + VxgiGetFinestVoxelSize()) * VxgiGetFinestVoxelSize();
float normalWeight = pow(saturate(dot(_smoothNormal, smoothNormal)), 20);
weight = normalWeight * distanceWeight;
}
if (!IsInfOrNaN(weight) && weight > 0)
{
#if AMBIENT_OCCLUSION_MODE
color.rgba += s_CoarseDiffuse[sharedIndex.y + j][sharedIndex.x + i].xyzw * weight;
#else
colorX.rgb += s_CoarseDiffuseX[sharedIndex.y + j][sharedIndex.x + i].xyz * weight;
colorX.a   += s_CoarsePosition[sharedIndex.y + j][sharedIndex.x + i].y   * weight;
colorY.rgb += s_CoarseDiffuseY[sharedIndex.y + j][sharedIndex.x + i].xyz * weight;
colorZ.rgb += s_CoarseDiffuseZ[sharedIndex.y + j][sharedIndex.x + i].xyz * weight;
#endif
combinedWeight += weight;
}
}
}
}
if (combinedWeight > 0)
{
combinedWeight = rcp(combinedWeight);
}
else
{
#if AMBIENT_OCCLUSION_MODE
color.rgba = s_CoarseDiffuse[sharedIndex.y][sharedIndex.x].xyzw;
#else
colorX.rgb = s_CoarseDiffuseX[sharedIndex.y][sharedIndex.x].xyz;
colorX.a   = s_CoarsePosition[sharedIndex.y][sharedIndex.x].y;
colorY.rgb = s_CoarseDiffuseY[sharedIndex.y][sharedIndex.x].xyz;
colorZ.rgb = s_CoarseDiffuseZ[sharedIndex.y][sharedIndex.x].xyz;
#endif
combinedWeight = 1.0;
}
int2 coarsePixelPos = groupBase + int2(gfsdk_GroupThreadIdx.xy);
#if AMBIENT_OCCLUSION_MODE
color *= combinedWeight;
color = InfNaNOutputGuard(color);
u_CoarseDiffuseX[coarsePixelPos] = color;
#else
colorX *= combinedWeight;
colorY *= combinedWeight;
colorZ *= combinedWeight;
colorX = InfNaNOutputGuard(colorX);
colorY = InfNaNOutputGuard(colorY);
colorZ = InfNaNOutputGuard(colorZ);
u_CoarseDiffuseX[coarsePixelPos] = colorX;
u_CoarseDiffuseY[coarsePixelPos] = colorY;
u_CoarseDiffuseZ[coarsePixelPos] = colorZ;
#endif
}
