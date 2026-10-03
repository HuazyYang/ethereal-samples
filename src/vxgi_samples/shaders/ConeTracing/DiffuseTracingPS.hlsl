#include "ConeTracingCommon.hlsli"

struct SampleData
{
float3      worldSamplePos;
float       mipLevel;
float3      sampledEmittance;
float       accumulatedOcclusion;
float3      direction;
int         coneIndex;
int         sampleIndex;
float       sampleT;
};
struct ConeData
{
float3      startPos;
float3      direction;
float       coneFactor;
int         coneIndex;
};
#if USE_SAVE_SAMPLES
RWStructuredBuffer<SampleData> u_SamplePositions : REGISTER_UAV(VXGI_CT_SAMPLE_POSITIONS_UAV_SLOT, 0);
RWStructuredBuffer<ConeData> u_ConeDirections : REGISTER_UAV(VXGI_CT_CONE_DIRECTIONS_UAV_SLOT, 0);
RWBuffer<uint> u_AppendCounters : REGISTER_UAV(VXGI_CT_APPEND_COUNTERS_UAV_SLOT, 0);
void VxgiOnBeginCone(float3 worldPos, float3 direction, float4 userData)
{
if(userData.x > 0)
{
ConeData CD;
CD.startPos = worldPos;
CD.direction = direction;
CD.coneIndex = int(userData.y);
CD.coneFactor = userData.z;
uint slot;
InterlockedAdd(u_AppendCounters[1], 1, slot);
u_ConeDirections[slot] = CD;
}
}
void VxgiOnConeSample(float t, float3 worldPos, float3 direction, float fLevel, float sampleIndex, float transparency, float3 emittance, float4 userData)
{
if(userData.x > 0)
{
SampleData SD;
SD.worldSamplePos = worldPos;
SD.mipLevel = fLevel;
SD.accumulatedOcclusion = 1 - transparency;
SD.sampledEmittance = emittance;
SD.coneIndex = int(userData.y);
SD.sampleIndex = int(sampleIndex + 1);
SD.direction = direction;
SD.sampleT = t;
uint slot;
InterlockedAdd(u_AppendCounters[0], 1, slot);
u_SamplePositions[slot] = SD;
}
}
#endif
#define USE_DIFFUSE_SS_CORRECTION                0
#define SINGLE_DIFFUSE_OUTPUT (EMITTANCE_FORMAT == NONE || !SPARSE_TRACING)
Texture2DArray t_ConeDirectionMap : REGISTER_SRV(VXGI_CT_TEXTURE_SRV_SLOT_0, 0);
TextureCube t_EnvironmentMap : REGISTER_SRV(VXGI_CT_TEXTURE_SRV_SLOT_1, 0);
SamplerState s_EnvironmentMapSampler : REGISTER_SAMPLER(VXGI_CT_ENV_MAP_SAMPLER_SLOT, 0);
Texture2D t_ScreenSpaceOcclusion : REGISTER_SRV(VXGI_CT_TEXTURE_SRV_SLOT_2, 0);

#if SINGLE_DIFFUSE_OUTPUT
void main(VxgiFullScreenQuadOutput quadIn,
in float4 gl_FragCoord: SV_Position,
out float4 color : SV_Target0)
#else
void main(VxgiFullScreenQuadOutput quadIn,
in float4 gl_FragCoord: SV_Position,
out float4 colorX : SV_Target0,
out float4 colorY : SV_Target1,
out float4 colorZ : SV_Target2)
#endif
{
#if SINGLE_DIFFUSE_OUTPUT
color = float4(0, 0, 0, 0);
#else
colorX = float4(0, 0, 0, 0);
colorY = float4(0, 0, 0, 0);
colorZ = float4(0, 0, 0, 0);
#endif
float2 gbufferSamplePosition;
if (all(g_DownsampleScale.xy > float2(1.0, 1.0)))
gbufferSamplePosition = CoarsePosToGBufferPos(floor(gl_FragCoord.xy));
else
gbufferSamplePosition = gl_FragCoord.xy;
float sampleDepth;
float4 sampleNormal;
float3 sampleSmoothNormal;
GetGeometrySampleFromGBuffer(int2(gbufferSamplePosition), 0, g_GBuffer, sampleDepth, sampleNormal, sampleSmoothNormal);
float3 samplePosition = DepthToWorldPos(gbufferSamplePosition, sampleDepth, g_GBuffer);
float3 illumination = float3(0, 0, 0);
float distanceFromAnchor = VxgiGetDistanceFromAnchor(samplePosition.xyz);
float distanceToBoundary = g_VxgiAbstractTracingCB.ClipmapAnchor.w - distanceFromAnchor;
float minLevelAtSamplePos = log2(VxgiGetMinSampleSizeInternal(distanceFromAnchor));
float confidence = saturate(distanceToBoundary * g_VxgiAbstractTracingCB.rClipmapSizeWorld * 4);
uint coneIndex = uint(quadIn.instanceID);
if(!IsInvalidNormal(sampleSmoothNormal) && confidence != 0)
{
sampleSmoothNormal = normalize(sampleSmoothNormal);
float3 tangent;
float3 cotangent;
VxgiGetTangentAndCotangent(sampleSmoothNormal, tangent, cotangent);
{
float4 tangentSpace = t_ConeDirectionMap[int3((int2(floor(gl_FragCoord.xy)) + g_RandomOffset.xy) & 3, coneIndex)];
float3 rayDir = normalize(tangentSpace.x * tangent + tangentSpace.y * sampleSmoothNormal + tangentSpace.z * cotangent);
float minSampleSize = VxgiGetMinSampleSizeInVoxels(samplePosition.xyz);
float4 offsetParams = float4(g_InitialOffsetBias, g_InitialOffsetDistanceFactor, g_NormalOffsetFactor, minSampleSize * tangentSpace.w * g_TracingStep);
float tracingStep = g_TracingStep;
if(g_AltSettingsStencilMask != 0)
{
int stencilValue = int(Load2D(g_TargetStencil, int2(gbufferSamplePosition), 0).g);
if((stencilValue & g_AltSettingsStencilMask) == g_AltSettingsStencilRefValue)
{
offsetParams.xyz = float3(g_AltInitialOffsetBias, g_AltInitialOffsetDistanceFactor, g_AltNormalOffsetFactor);
tracingStep = g_AltTracingStep;
}
}
float opacity = 0;
#if USE_DIFFUSE_SS_CORRECTION
if (g_UseScreenSpaceCorrection != 0)
{
const float voxelSize = VxgiGetFinestVoxelSize();
const float numSteps = 10.0;
const float tracingDistance = voxelSize * 2;
const float tracingOffset = voxelSize * 0.1;
float3 startPt = samplePosition.xyz + (sampleNormal.xyz + rayDir.xyz) * tracingOffset;
float3 endPt = samplePosition.xyz + rayDir.xyz * tracingDistance;
float3 samplePt;
opacity = FindDepthIntersection(startPt, endPt, numSteps, voxelSize, samplePt);
if (opacity > 0)
{
samplePosition.xyz = lerp(samplePosition.xyz, samplePt, opacity);
offsetParams *= (1.0 - opacity).xxxx;
}
}
#endif
bool saveSamples = all(floor(gl_FragCoord.xy) == floor(g_PixelToSave.xy * g_DownsampleScale.zw));
VxgiConeTracingArguments args = VxgiDefaultConeTracingArguments();
args.direction = rayDir;
args.coneFactor = g_ConeFactor;
args.tracingStep = tracingStep;
args.opacityCorrectionFactor = g_OpacityCorrectionFactor;
args.emittanceScale = g_EmittanceScale;
args.initialOpacity = opacity;
args.ambientAttenuationFactor = g_AmbientAttenuationFactor * pow(minSampleSize, g_AmbientDistanceDarkening);
args.maxSamples = g_MaxSamples;
args.enableSceneBoundsCheck = true;
args.flipOpacityDirections = g_FlipOpacityDirections != 0;
args.monitoringUserData.x = saveSamples ? 1 : 0;
args.monitoringUserData.y = float(coneIndex);
args.monitoringUserData.z = g_ConeFactor;
AdjustConePosition(samplePosition.xyz, offsetParams, float4(sampleSmoothNormal.xyz, 0), args);
VxgiConeTracingResults cone = VxgiTraceCone(args);
float3 radiance = float3scalar(0.0);
#if EMITTANCE_FORMAT != NONE
float3 radiosity = cone.irradiance;
float3 envMapScale = g_EnvironmentMapTint.rgb * (1-cone.finalOpacity);
if(any(envMapScale > float3scalar(0)))
{
float envMip = min(g_MaxEnvironmentMapMipLevel, max(0, log2(g_ConeFactor * g_EnvironmentMapResolution) - 1));
radiosity.rgb += envMapScale * t_EnvironmentMap.SampleLevel(s_EnvironmentMapSampler, rayDir, envMip).rgb * g_rNumCones;
}
radiance = radiosity / VxgiPI;
#endif
float correctedAmbient = pow(saturate(cone.ambient * g_AmbientScale + g_AmbientBias), g_AmbientPower);
radiance += correctedAmbient * g_AmbientColor.rgb * g_rNumCones;
#if !SPARSE_TRACING
color.rgb = radiance.rgb * saturate(dot(rayDir.xyz, sampleNormal.xyz));
#elif EMITTANCE_FORMAT == NONE
color.rgb = rayDir.xyz * (correctedAmbient * g_rNumCones);
#else
colorX.rgb = radiance.rgb * rayDir.x;
colorY.rgb = radiance.rgb * rayDir.y;
colorZ.rgb = radiance.rgb * rayDir.z;
#endif
}
}
#if SINGLE_DIFFUSE_OUTPUT
color.a = confidence * g_rNumCones;
#if !SPARSE_TRACING
if (g_SSAO_RadiusWorld > 0)
{
color.rgb *= t_ScreenSpaceOcclusion[int2(gl_FragCoord.xy)].rrr;
}
if (any(g_BackgroundColor.rgb != float3scalar(0)))
{
color.rgb = color.rgb * confidence + g_BackgroundColor.rgb * (g_rNumCones - color.a);
}
#endif
color = InfNaNOutputGuard(color);
#else
colorX.a = confidence * g_rNumCones;
colorX = InfNaNOutputGuard(colorX);
colorY = InfNaNOutputGuard(colorY);
colorZ = InfNaNOutputGuard(colorZ);
#endif
}
