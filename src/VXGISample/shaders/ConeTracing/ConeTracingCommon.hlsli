#ifndef VXGISAMPLE_SHADERS_CONETRACING_CONETRACINGCOMMON_HLSLI
#define VXGISAMPLE_SHADERS_CONETRACING_CONETRACINGCOMMON_HLSLI

#if USE_SAVE_SAMPLES
#define VXGI_MONITOR_CONE_TRACING
#endif
#include "../Common/AbstractConeTracing.hlsli"
#include "ConeTracingConstants.hlsli"

struct GBufferParameters
{
float4x4    viewProjMatrix;
float4x4    viewProjMatrixInv;
float4x4    viewMatrix;
float4      cameraPosition;
float4      uvToView;
float2      gbufferSize;
float2      gbufferSizeInv;
float2      viewportOrigin;
float2      viewportSize;
float2      viewportSizeInv;
float2      firstSamplePosition;
float       projectionA;
float       projectionB;
float       depthScale;
float       depthBias;
float       normalScale;
float       normalBias;
float       radiusToScreen;
};
#if MSAA_G_BUFFER
#define Load2D(tex, iCoords, i) tex.Load(iCoords, i)
Texture2DMS<float4> g_DepthBuffer : REGISTER_SRV(VXGI_CT_DEPTH_BUFFER_SRV_SLOT, 0);
Texture2DMS<float4> g_TargetNormal : REGISTER_SRV(VXGI_CT_NORMAL_BUFFER_SRV_SLOT, 0);
Texture2DMS<float4> g_TargetFlatNormal : REGISTER_SRV(VXGI_CT_FLAT_NORMAL_BUFFER_SRV_SLOT, 0);
Texture2DMS<uint2> g_TargetStencil : REGISTER_SRV(VXGI_CT_STENCIL_BUFFER_SRV_SLOT, 0);
#else
#define Load2D(tex, iCoords, i) tex[iCoords]
Texture2D g_DepthBuffer : REGISTER_SRV(VXGI_CT_DEPTH_BUFFER_SRV_SLOT, 0);
Texture2D g_TargetNormal : REGISTER_SRV(VXGI_CT_NORMAL_BUFFER_SRV_SLOT, 0);
Texture2D g_TargetFlatNormal: REGISTER_SRV(VXGI_CT_FLAT_NORMAL_BUFFER_SRV_SLOT, 0);
Texture2D<uint2> g_TargetStencil: REGISTER_SRV(VXGI_CT_STENCIL_BUFFER_SRV_SLOT, 0);
#endif
float RescaleDepth(float depthFromGBuffer, GBufferParameters gbufferParams)
{
return depthFromGBuffer * gbufferParams.depthScale + gbufferParams.depthBias;
}
float3 RescaleNormal(float3 normalFromBGuffer, GBufferParameters gbufferParams)
{
float3 scaled = normalFromBGuffer.xyz * gbufferParams.normalScale + gbufferParams.normalBias;
float len = length(scaled);
if(len > 0)
return scaled / len;
return float3(0, 0, 0);
}
float2 PixelToUV(float2 sampleScreenPosition, GBufferParameters gbufferParams)
{
float2 uv = (sampleScreenPosition - gbufferParams.viewportOrigin) * gbufferParams.viewportSizeInv;
return uv;
}
float3 DepthToWorldPos(float2 sampleScreenPosition, float depth, GBufferParameters gbufferParams)
{
float2 uv = PixelToUV(sampleScreenPosition, gbufferParams);
float4 clipCoord = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, depth, 1.0f);
float4 samplePosition = mul(clipCoord, gbufferParams.viewProjMatrixInv);
samplePosition.xyz /= samplePosition.w;
return samplePosition.xyz;
}
float DepthToViewSpaceDepth(float depth, GBufferParameters gbufferParams)
{
return gbufferParams.projectionB / (depth - gbufferParams.projectionA);
}
void GetGeometrySampleFromGBuffer(int2 sampleScreenPosition, int sampleIndex, GBufferParameters gbufferParams, out float depth, out float4 normal, out float3 smoothNormal)
{
depth = Load2D(g_DepthBuffer, sampleScreenPosition, sampleIndex).x;
normal = Load2D(g_TargetNormal, sampleScreenPosition, sampleIndex);
smoothNormal = Load2D(g_TargetFlatNormal, sampleScreenPosition, sampleIndex).xyz;
depth = RescaleDepth(depth, gbufferParams);
normal.xyz = RescaleNormal(normal.xyz, gbufferParams);
smoothNormal.xyz = RescaleNormal(smoothNormal.xyz, gbufferParams);
}
bool IsInfOrNaN(float x)
{
uint exponent = asuint(x) & 0x7f800000;
return exponent == 0x7f800000;
}
bool IsInfOrNaN(float4 v)
{
return IsInfOrNaN(v.x) || IsInfOrNaN(v.y) || IsInfOrNaN(v.z) || IsInfOrNaN(v.w);
}
bool IsInvalidNormal(float3 normal)
{
uint3 normalExp = asuint(normal) & 0x7f800000;
return all(normalExp == uint3(0, 0, 0)) || any(normalExp == uint3(0x7f800000, 0x7f800000, 0x7f800000));
}
float GetDepthDiscontinuity(float refW, float w, float rMinSampleSize)
{
return saturate(abs(refW - w) * rMinSampleSize * 0.05);
}
float GetNormalDiscontinuity(float3 refNormal, float3 normal)
{
return saturate(1.0 - pow(saturate(dot(refNormal, normal)), 20));
}
#define USE_ROTATED_GRID 1
cbuffer cBuiltinTracingParameters: REGISTER_CBUFFER(VXGI_CT_BUILTIN_CBV_SLOT, 0)
{
GBufferParameters g_GBuffer;
GBufferParameters g_PreviousGBuffer;
float4x4    g_ReprojectionMatrix;
float4      g_AmbientColor;
float4      g_DownsampleScale;
float4      g_DebugParams;
float4      g_EnvironmentMapTint;
float4      g_RefinementGridResolution;
float4      g_BackgroundColor;
int2        g_PixelToSave;
int2        g_RandomOffset;
float2      g_GridOrigin;
float       g_ConeFactor;
float       g_TracingStep;
float       g_OpacityCorrectionFactor;
int         g_MaxSamples;
int         g_NumCones;
float       g_rNumCones;
float       g_EmittanceScale;
float       g_EnvironmentMapResolution;
float       g_MaxEnvironmentMapMipLevel;
float       g_NormalOffsetFactor;
float       g_AmbientAttenuationFactor;
uint        g_FlipOpacityDirections;
float       g_InitialOffsetBias;
float       g_InitialOffsetDistanceFactor;
uint        g_EnableSpecularRandomOffsets;
uint        g_NumDiscontinuityLevels;
float       g_TemporalReprojectionWeight;
float       g_TangentJitterScale;
float       g_DepthDeltaSign;
float       g_ReprojectionDepthWeightScale;
float       g_ReprojectionNormalWeightExponent;
float       g_InterpolationWeightThreshold;
uint        g_EnableRefinement;
float       g_AmbientScale;
float       g_AmbientBias;
float       g_AmbientPower;
float       g_AmbientDistanceDarkening;
int         g_AltSettingsStencilMask;
int         g_AltSettingsStencilRefValue;
float       g_AltInitialOffsetBias;
float       g_AltInitialOffsetDistanceFactor;
float       g_AltNormalOffsetFactor;
float       g_AltTracingStep;
float       g_SSAO_SurfaceBias;
float       g_SSAO_RadiusWorld;
float       g_SSAO_rBackgroundViewDepth;
float       g_SSAO_CoarseAO;
float       g_SSAO_PowerExponent;
};
float4 GetColorFromPreviousFrame(
float2 uv,
float newDepth,
float3 newNormal,
#if MSAA_G_BUFFER
Texture2DMS<float4> prevDepthBuffer,
Texture2DMS<float4> prevNormalBuffer,
#else
Texture2D<float4> prevDepthBuffer,
Texture2D<float4> prevNormalBuffer,
#endif
Texture2D<float4> prevColorBuffer,
out float totalWeight
)
{
totalWeight = 0;
float4 clipCoord = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, newDepth, 1.0f);
float4 previousClipCoord = mul(clipCoord, g_ReprojectionMatrix);
if(any(abs(previousClipCoord.xyz) >= previousClipCoord.www) || previousClipCoord.w <= 0)
return float4(0, 0, 0, 0);
previousClipCoord.xyz /= previousClipCoord.w;
float2 oldPos;
oldPos.x = g_PreviousGBuffer.viewportOrigin.x + (previousClipCoord.x * 0.5 + 0.5) * g_PreviousGBuffer.viewportSize.x;
oldPos.y = g_PreviousGBuffer.viewportOrigin.y + (0.5 - previousClipCoord.y * 0.5) * g_PreviousGBuffer.viewportSize.y;
float expectedW = DepthToViewSpaceDepth(previousClipCoord.z, g_PreviousGBuffer);
float4 colorSum = float4(0, 0, 0, 0);
[unroll] for(float x = -0.5; x < 1; x++)
[unroll] for(float y = -0.5; y < 1; y++)
{
float2 samplePos = floor(oldPos + float2(x, y));
int2 isamplePos = int2(samplePos);
float oldW = DepthToViewSpaceDepth(RescaleDepth(Load2D(prevDepthBuffer, isamplePos, 0).x, g_PreviousGBuffer), g_PreviousGBuffer);
float3 oldNormal = RescaleNormal(Load2D(prevNormalBuffer, isamplePos, 0).xyz, g_PreviousGBuffer);
float4 oldColor = prevColorBuffer[isamplePos].rgba;
float depthWeight = saturate(1.0 - abs(expectedW - oldW) * g_ReprojectionDepthWeightScale);
float normalWeight = saturate(pow(saturate(dot(newNormal, oldNormal)), g_ReprojectionNormalWeightExponent));
float bilerpWeight = saturate((1.0 - abs(oldPos.x - samplePos.x - 0.5)) * (1.0 - abs(oldPos.y - samplePos.y - 0.5)));
float weight = depthWeight * normalWeight * bilerpWeight;
if (!(IsInfOrNaN(weight) || IsInfOrNaN(oldColor)))
{
colorSum += oldColor.rgba * weight;
totalWeight += weight;
}
}
return colorSum;
}
float2 GetRotatedGridOffset(float2 pixelCoord)
{
#if USE_ROTATED_GRID
float2 remainders = floor(frac(pixelCoord * g_DownsampleScale.zw) * g_DownsampleScale.xy);
return float2(remainders.y, g_DownsampleScale.x - remainders.x - 1);
#else
return float2(0, 0);
#endif
}
float2 CoarsePosToGBufferPos(float2 coarsePixelPos)
{
return
coarsePixelPos * g_DownsampleScale.xy +
GetRotatedGridOffset(coarsePixelPos + g_RandomOffset.xy) +
g_GBuffer.firstSamplePosition;
}
float FindDepthIntersection(float3 startPt, float3 endPt, float numSteps, float depthThreshold, out float3 samplePt)
{
float4 startClip = mul(float4(startPt, 1), g_GBuffer.viewProjMatrix);
float4 endClip = mul(float4(endPt, 1), g_GBuffer.viewProjMatrix);
startClip.xyz /= startClip.w;
endClip.xyz /= endClip.w;
float4 windowTransform = float4(0.5, -0.5, 0.5, 0.5);
startClip.xy = startClip.xy * windowTransform.xy + windowTransform.zw;
endClip.xy = endClip.xy * windowTransform.xy + windowTransform.zw;
float3 stepSize = (endClip.xyz - startClip.xyz) / numSteps;
float threshold = abs(stepSize.z) * 0.1;
float depth = 1.0;
bool hit = false;
for (float i = 0; i < numSteps; ++i)
{
samplePt = startClip.xyz + stepSize * i;
depth = RescaleDepth(Load2D(g_DepthBuffer, int2(samplePt.xy * g_GBuffer.viewportSize + g_GBuffer.viewportOrigin), 0).x, g_GBuffer);
float depthDiff = (samplePt.z - depth) * g_DepthDeltaSign;
hit = depthDiff > threshold;
if (hit) i = numSteps;
}
float occlusion = 0;
if (hit)
{
float4 clipTransform = float4(2.0, -2.0, -1.0, 1.0);
samplePt.xy = samplePt.xy * clipTransform.xy + clipTransform.zw;
float4 posWS_0 = mul(float4(samplePt, 1), g_GBuffer.viewProjMatrixInv);
posWS_0.xyz /= posWS_0.w;
float4 posWS_1 = mul(float4(samplePt.xy, depth, 1), g_GBuffer.viewProjMatrixInv);
posWS_1.xyz /= posWS_1.w;
samplePt = posWS_0.xyz;
float delta = length(posWS_0.xyz - posWS_1.xyz);
if (delta < depthThreshold)
{
occlusion = 1.0 - pow(saturate(delta / depthThreshold), 0.8);
occlusion *= 1.0 - length(startPt - posWS_0.xyz) / length(startPt - endPt);
occlusion = saturate(occlusion);
}
}
return occlusion;
}
void AdjustConePosition(
float3 surfacePosition,
float4 initialOffsetParams,
float4 sampleNormalParams,
inout VxgiConeTracingArguments args
)
{
float t = args.tracingStep * 5.0f;
float tStep, fLevel, sampleSize, initialOffset;
float minSampleSize = VxgiGetMinSampleSizeInVoxels(surfacePosition);
VxgiCalculateSampleParameters(t, args.coneFactor, args.tracingStep, minSampleSize, tStep, fLevel, sampleSize);
initialOffset = minSampleSize * initialOffsetParams.y + initialOffsetParams.x;
initialOffset += initialOffsetParams.x * sampleNormalParams.w * 5.0;
initialOffset += initialOffsetParams.w;
args.firstSampleT = initialOffset;
args.firstSamplePosition = surfacePosition + normalize(lerp(args.direction, sampleNormalParams.xyz, initialOffsetParams.z)) * (VxgiGetFinestVoxelSize() * initialOffset);
}
float InfNaNOutputGuard(float x)
{
return IsInfOrNaN(x) ? 0.f : x;
}
float4 InfNaNOutputGuard(float4 v)
{
return float4(
InfNaNOutputGuard(v.x),
InfNaNOutputGuard(v.y),
InfNaNOutputGuard(v.z),
InfNaNOutputGuard(v.w));
}

#endif /* VXGISAMPLE_SHADERS_CONETRACING_CONETRACINGCOMMON_HLSLI */
