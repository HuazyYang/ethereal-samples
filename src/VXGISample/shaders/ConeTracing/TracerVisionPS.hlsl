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
void main(
VxgiFullScreenQuadOutput quadIn,
in float4 gl_FragCoord: SV_Position,
out float4 gl_FragColor: SV_Target
)
{
bool saveSamples = (int(gl_FragCoord.x) == g_PixelToSave.x) && (int(gl_FragCoord.y) == g_PixelToSave.y);
VxgiConeTracingArguments args = VxgiDefaultConeTracingArguments();
args.direction = VxgiProjToRay(quadIn.posProj, g_GBuffer.cameraPosition.xyz, g_GBuffer.viewProjMatrixInv);
args.firstSamplePosition = g_GBuffer.cameraPosition.xyz;
args.coneFactor = g_ConeFactor;
args.tracingStep = g_TracingStep;
args.opacityCorrectionFactor = g_OpacityCorrectionFactor;
args.emittanceScale = g_EmittanceScale;
args.maxSamples = g_MaxSamples;
args.enableSceneBoundsCheck = false;
args.flipOpacityDirections = bool(g_FlipOpacityDirections);
args.monitoringUserData.x = saveSamples ? 1 : 0;
args.monitoringUserData.y = -1;
args.monitoringUserData.z = g_ConeFactor;
VxgiConeTracingResults cone = VxgiTraceCone(args);
gl_FragColor = float4(cone.irradiance.rgb, 1.0);
}
