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
#define USE_SPECULAR_SS_CORRECTION 0
Texture2D t_Randoms : REGISTER_SRV(VXGI_CT_TEXTURE_SRV_SLOT_0, 0);
TextureCube t_EnvironmentMap : REGISTER_SRV(VXGI_CT_TEXTURE_SRV_SLOT_1, 0);
SamplerState s_EnvironmentMapSampler : REGISTER_SAMPLER(VXGI_CT_ENV_MAP_SAMPLER_SLOT, 0);
#if MSAA_G_BUFFER
Texture2DMS<float4> g_PrevDepthBuffer : REGISTER_SRV(VXGI_CT_PREV_DEPTH_BUFFER_SRV_SLOT, 0);
Texture2DMS<float4> g_PrevTargetNormal : REGISTER_SRV(VXGI_CT_PREV_NORMAL_BUFFER_SRV_SLOT, 0);
#else
Texture2D g_PrevDepthBuffer : REGISTER_SRV(VXGI_CT_PREV_DEPTH_BUFFER_SRV_SLOT, 0);
Texture2D g_PrevTargetNormal : REGISTER_SRV(VXGI_CT_PREV_NORMAL_BUFFER_SRV_SLOT, 0);
#endif
Texture2D g_PrevSpecular : REGISTER_SRV(VXGI_CT_TEXTURE_SRV_SLOT_2, 0);
float rand(float2 co)
{
return frac(sin(dot(co.xy, float2(12.9898,78.233))) * 43758.5453);
}
void main(
VxgiFullScreenQuadOutput quadIn,
in float4 gl_FragCoord: SV_Position,
out float4 gl_FragColor: SV_Target
)
{
float3 color = float3(0.0, 0.0, 0.0);
uint s = 0;
float sampleDepth;
float4 sampleNormal;
float3 sampleSmoothNormal;
GetGeometrySampleFromGBuffer(int2(gl_FragCoord.xy), 0, g_GBuffer, sampleDepth, sampleNormal, sampleSmoothNormal);
float3 samplePosition = DepthToWorldPos(gl_FragCoord.xy, sampleDepth, g_GBuffer);
float roughness = sampleNormal.w;
if(roughness <= 0)
{
gl_FragColor = float4(0.0, 0.0, 0.0, 0.0);
return;
}
float3 incident = samplePosition - g_GBuffer.cameraPosition.xyz;
float3 rayDir = normalize(reflect(incident, sampleNormal.xyz));
float minSampleSize = VxgiGetMinSampleSizeInVoxels(samplePosition.xyz);
float3 V = normalize(g_GBuffer.cameraPosition.xyz - samplePosition.xyz);
float Inv_NDotV_Factor = pow(saturate(1.0f - abs(dot(sampleNormal.xyz, V))), 4.0f);
float perPixelOffset = (g_EnableSpecularRandomOffsets != 0)
? t_Randoms[int2(gl_FragCoord.xy + g_RandomOffset.xy) & 3].x
: 0.5f;
float4 offsetParams = float4(g_InitialOffsetBias, g_InitialOffsetDistanceFactor, 0, minSampleSize * perPixelOffset * g_TracingStep);
roughness = min(roughness, 0.75);
float alpha = VxgiSqr(roughness);
float coneFactor = alpha * sqrt(VxgiPI);
float opacity = 0;
#if USE_SPECULAR_SS_CORRECTION
if (g_UseScreenSpaceCorrection != 0)
{
const float voxelSize = VxgiGetFinestVoxelSize();
const float numSteps = (0.5 + 0.5 * Inv_NDotV_Factor) * 60.0;
const float tracingDistance = voxelSize * (Inv_NDotV_Factor + 0.1) * 20;
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
bool saveSamples = all(floor(gl_FragCoord.xy) == g_PixelToSave.xy);
VxgiConeTracingArguments args = VxgiDefaultConeTracingArguments();
args.direction = rayDir;
args.coneFactor = coneFactor;
args.tracingStep = g_TracingStep;
args.opacityCorrectionFactor = g_OpacityCorrectionFactor;
args.emittanceScale = g_EmittanceScale;
args.initialOpacity = opacity;
args.ambientAttenuationFactor = 0.0;
args.maxSamples = g_MaxSamples;
args.enableSceneBoundsCheck = true;
args.flipOpacityDirections = (g_FlipOpacityDirections != 0);
args.randomSeed = rand(gl_FragCoord.xy + g_RandomOffset.xy);
args.tangentJitterScale = g_TangentJitterScale;
args.monitoringUserData.x = saveSamples ? 1 : 0;
args.monitoringUserData.y = -1;
args.monitoringUserData.z = coneFactor;
AdjustConePosition(samplePosition.xyz, offsetParams, float4(sampleNormal.xyz, Inv_NDotV_Factor), args);
VxgiConeTracingResults cone = VxgiTraceCone(args);
float3 radiosity = cone.irradiance;
float finalOpacity = cone.finalOpacity;
float3 envMapScale = g_EnvironmentMapTint.rgb * (1-cone.finalOpacity);
if(any(envMapScale > float3scalar(0)))
{
float envMip = min(g_MaxEnvironmentMapMipLevel, max(0, log2(coneFactor * g_EnvironmentMapResolution) - 1));
radiosity.rgb += envMapScale * t_EnvironmentMap.SampleLevel(s_EnvironmentMapSampler, rayDir, envMip).rgb * VxgiSqr(coneFactor);
}
float3 radiance = radiosity / (VxgiPI * VxgiSqr(alpha));
if(g_TemporalReprojectionWeight > 0)
{
float reprojectedWeight = 0;
float4 reprojectedColor = GetColorFromPreviousFrame(quadIn.uv, sampleDepth, sampleNormal.xyz,
g_PrevDepthBuffer, g_PrevTargetNormal, g_PrevSpecular, reprojectedWeight);
float4 result = float4(radiance, finalOpacity) * (1 - reprojectedWeight * g_TemporalReprojectionWeight)
+ reprojectedColor.rgba          * g_TemporalReprojectionWeight;
radiance = result.rgb;
finalOpacity = result.a;
}
gl_FragColor.rgb = radiance;
gl_FragColor.a = finalOpacity;
gl_FragColor = InfNaNOutputGuard(gl_FragColor);
}
