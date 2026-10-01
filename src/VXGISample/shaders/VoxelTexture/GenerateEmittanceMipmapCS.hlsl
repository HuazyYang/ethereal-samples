#include "VoxelTextureCommon.hlsli"
#if EMITTANCE_FORMAT == FLOAT32
Texture3D<float4> t_EmittanceSrcR: register(t2);
Texture3D<float4> t_EmittanceSrcG: register(t3);
Texture3D<float4> t_EmittanceSrcB: register(t4);
RWTexture3D<uint> u_EmittanceDstR: register(u7);
RWTexture3D<uint> u_EmittanceDstG: register(u8);
RWTexture3D<uint> u_EmittanceDstB: register(u9);
#elif EMITTANCE_FORMAT == FLOAT16
Texture3D<float4> t_EmittanceSrc: register(t2);
RWTexture3D<float4> u_EmittanceDst: register(u7);
#else
Texture3D<float4> t_EmittanceSrc: register(t2);
RWTexture3D<uint> u_EmittanceDst: register(u7);
#endif
RWTexture3D<uint> u_Opacity_Pos: register(u2);
SamplerState s_LinearWrapSampler: register(s0);
[numthreads(4, 4, 4)]
void main(in uint3 gfsdk_GroupIdx: SV_GroupID, in uint3 gfsdk_GroupThreadIdx: SV_GroupThreadID, in uint3 gfsdk_GlobalIdx: SV_DispatchThreadID)
{
if (any(gfsdk_GlobalIdx.xyz >= int3scalar(g_LevelSizeCurrent)))
return;
int3 readCoordinate = int3(gfsdk_GlobalIdx.xyz * 2) + int3(0, 0, g_EmittancePackingOffsetPrevious);
int3 writeCoordinate = int3(gfsdk_GlobalIdx.xyz) + int3(0, 0, g_EmittancePackingOffsetCurrent);
bool hasEmittance = false;
int directionOffset = 1;
for (uint direction=0; direction < EMITTANCE_DIRECTIONS; ++direction)
{
float3 readAddr = float3(readCoordinate) + float3(directionOffset, 0, 0) + 1.0;
readAddr *= g_rEmittanceTextureSize.xyz;
#if USE_TRIANGULAR_FILTER
float4 result = float4scalar(0);
[unroll]
for(int vertex = 0; vertex < 8; vertex++)
{
float3 offset = float3((vertex >> 2) & 1, (vertex >> 1) & 1, vertex & 1);
offset = (offset * 1.5 - 0.75) * g_rEmittanceTextureSize.xyz;
result.rgb += SAMPLE_EMITTANCE(t_EmittanceSrc, s_LinearWrapSampler, readAddr + offset).rgb;
}
result *= 0.25;
#else
float4 result = SAMPLE_EMITTANCE(t_EmittanceSrc, s_LinearWrapSampler, readAddr);
result *= 2;
#endif
int3 writeAddr = writeCoordinate + int3(directionOffset, 0, 0);
EMITTANCE_STORAGE_TYPE emittance = VxgiPackEmittance(result);
STORE_EMITTANCE_TO_UAV(u_EmittanceDst, writeAddr, emittance);
if(any(result.rgb != float3scalar(0)))
hasEmittance = true;
directionOffset += int(g_PackingStride);
}
if(hasEmittance)
{
int3 address = int3(gfsdk_GlobalIdx.xyz) + int3(0, 0, g_OpacityPackingOffsetCurrent);
MARK_OPACITY(address);
}
}
