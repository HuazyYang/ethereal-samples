#include "ConeTracingCommon.hlsli"

SamplerState SamplerLinearBorder : REGISTER_SAMPLER(VXGI_CT_LINEAR_BORDER_SAMPLER_SLOT, 0);
Texture2D g_CoarseSpecular : REGISTER_SRV(VXGI_CT_TEXTURE_SRV_SLOT_0, 0);
RWTexture2D<float4> u_InterpolatedTarget : REGISTER_UAV(VXGI_CT_COMMON_UAV_SLOT_0, 0);
#define FILTER_SPECULAR_TILE_SIZE_X 16
#define FILTER_SPECULAR_TILE_SIZE_Y 16
#define FILTER_SPECULAR_BUFFER_X (FILTER_SPECULAR_TILE_SIZE_X+4)
#define FILTER_SPECULAR_BUFFER_Y (FILTER_SPECULAR_TILE_SIZE_Y+4)
groupshared float  s_W         [FILTER_SPECULAR_BUFFER_Y][FILTER_SPECULAR_BUFFER_X];
groupshared float4 s_Normal    [FILTER_SPECULAR_BUFFER_Y][FILTER_SPECULAR_BUFFER_X];
groupshared float4 s_Unfiltered[FILTER_SPECULAR_BUFFER_Y][FILTER_SPECULAR_BUFFER_X];
[numthreads(FILTER_SPECULAR_TILE_SIZE_X, FILTER_SPECULAR_TILE_SIZE_Y, 1)]
void main(in uint3 gfsdk_GroupIdx: SV_GroupID, in uint3 gfsdk_GroupThreadIdx: SV_GroupThreadID, in uint3 gfsdk_GlobalIdx: SV_DispatchThreadID)
{
static const int radiusLeft = 1;
static const int radiusRight = 2;
float2 groupBase = gfsdk_GroupIdx.xy * float2(FILTER_SPECULAR_TILE_SIZE_X, FILTER_SPECULAR_TILE_SIZE_Y) - radiusLeft;
uint2 offset;
for(offset.x = gfsdk_GroupThreadIdx.x; offset.x < FILTER_SPECULAR_BUFFER_X; offset.x += FILTER_SPECULAR_TILE_SIZE_X)
{
for(offset.y = gfsdk_GroupThreadIdx.y; offset.y < FILTER_SPECULAR_BUFFER_Y; offset.y += FILTER_SPECULAR_TILE_SIZE_Y)
{
int2 gbufferSamplePos = int2((groupBase + offset - radiusLeft) + g_GridOrigin.xy + g_GBuffer.firstSamplePosition.xy);
float w = DepthToViewSpaceDepth(RescaleDepth(Load2D(g_DepthBuffer, gbufferSamplePos, 0).x, g_GBuffer), g_GBuffer);
float4 normal = Load2D(g_TargetNormal, gbufferSamplePos, 0);
normal.xyz = RescaleNormal(normal.xyz, g_GBuffer);
float4 unfiltered = g_CoarseSpecular[gbufferSamplePos];
s_W         [offset.y][offset.x] = w;
s_Normal    [offset.y][offset.x] = normal.xyzw;
s_Unfiltered[offset.y][offset.x] = unfiltered.rgba;
}
}
GroupMemoryBarrierWithGroupSync();
float2 gbufferSamplePos = gfsdk_GlobalIdx.xy + g_GridOrigin.xy + g_GBuffer.firstSamplePosition.xy;
float4 sampleSpecular = float4scalar(0);
float4 normal = Load2D(g_TargetNormal, int2(gbufferSamplePos), 0);
float depth = RescaleDepth(Load2D(g_DepthBuffer, int2(gbufferSamplePos), 0).x, g_GBuffer);
bool specularOn = normal.w > 0;
if(!IsInvalidNormal(normal.xyz) && (depth < 1.0) && specularOn)
{
normal.xyz = RescaleNormal(normal.xyz, g_GBuffer);
float3 position = DepthToWorldPos(gbufferSamplePos, depth, g_GBuffer);
float w = DepthToViewSpaceDepth(depth, g_GBuffer);
float sampleSize = VxgiGetMinSampleSizeInVoxels(position.xyz);
float sampleWeight = 0;
for (int i = 0; i <= radiusLeft + radiusRight; ++i)
{
for (int j = 0; j <= radiusLeft + radiusRight; ++j)
{
float2 neighbourGBufferPos = gbufferSamplePos + float2(i, j);
if(any(neighbourGBufferPos < g_GBuffer.viewportOrigin.xy) || any(neighbourGBufferPos > g_GBuffer.viewportOrigin.xy + g_GBuffer.viewportSize.xy))
break;
float  _w          = s_W         [gfsdk_GroupThreadIdx.y + j][gfsdk_GroupThreadIdx.x + i];
float4 _normal     = s_Normal    [gfsdk_GroupThreadIdx.y + j][gfsdk_GroupThreadIdx.x + i];
float4 _unfiltered = s_Unfiltered[gfsdk_GroupThreadIdx.y + j][gfsdk_GroupThreadIdx.x + i];
float distanceWeight = rcp(abs(w - _w) + sampleSize) * sampleSize;
float normalWeight = pow(saturate(dot(_normal.xyz, normal.xyz)), 100);
float weight = normalWeight * distanceWeight;
sampleSpecular.rgb += weight * _unfiltered.rgb;
sampleSpecular.a += weight * _unfiltered.a;
sampleWeight += weight;
}
}
if(sampleWeight > 0.00001)
{
sampleSpecular /= sampleWeight;
}
}
#if DEBUG_VISUALIZATIONS
if(g_DebugParams.x != -1)
u_InterpolatedTarget[int2(gbufferSamplePos)] = sampleSpecular;
#else
sampleSpecular = InfNaNOutputGuard(sampleSpecular);
u_InterpolatedTarget[int2(gbufferSamplePos)] = sampleSpecular;
#endif
}
