#ifndef VXGISAMPLE_SHADERS_CONETRACINGDEBUG_CONETRACINGDEBUGCOMMON_HLSLI
#define VXGISAMPLE_SHADERS_CONETRACINGDEBUG_CONETRACINGDEBUGCOMMON_HLSLI
#include "../Common/ShaderCommon.hlsli"

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
RWStructuredBuffer<SampleData> u_SamplePositions: register(u3);
RWStructuredBuffer<ConeData> u_ConeDirections: register(u4);
RWBuffer<uint> u_AppendCounters: register(u5);
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
gfsdk_AtomicAdd(u_AppendCounters[1], 1, slot);
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
gfsdk_AtomicAdd(u_AppendCounters[0], 1, slot);
u_SamplePositions[slot] = SD;
}
}
#endif


#endif /* VXGISAMPLE_SHADERS_CONETRACINGDEBUG_CONETRACINGDEBUGCOMMON_HLSLI */
