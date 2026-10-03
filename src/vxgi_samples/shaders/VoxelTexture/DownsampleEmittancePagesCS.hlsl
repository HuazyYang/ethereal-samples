#include "VoxelTextureCommon.hlsli"
Buffer<uint> t_PagesToProcess: register(t0);
#if EMITTANCE_FORMAT == FLOAT32
Texture3D<float4> t_EmittanceSrcR: register(t2);
Texture3D<float4> t_EmittanceSrcG: register(t3);
Texture3D<float4> t_EmittanceSrcB: register(t4);
RWTexture3D<uint> u_EmittanceDstR: register(u7);
RWTexture3D<uint> u_EmittanceDstG: register(u8);
RWTexture3D<uint> u_EmittanceDstB: register(u9);
#elif EMITTANCE_FORMAT == FLOAT16 || EMITTANCE_FORMAT == FLOAT16_NVAPI
Texture3D<float4> t_EmittanceSrc: register(t2);
RWTexture3D<float4> u_EmittanceDst: register(u7);
#else
Texture3D<float4> t_EmittanceSrc: register(t2);
RWTexture3D<uint> u_EmittanceDst: register(u7);
#endif
RWTexture3D<uint> u_Opacity_Pos: register(u2);
SamplerState s_LinearWrapSampler: register(s0);
void DownsampleEmittancePagesBody(PageCoordinates pageCoords, uint3 offset)
{
int3 levelAddressCurrent = GetLevelAddress(int3(pageCoords.coords), g_TranslationParamsCurrent, g_ToroidalOffsetCurrent.xyz, g_LevelSizeCurrent);
int3 writeCoordinate = levelAddressCurrent + int3(offset) + int3(0, 0, g_EmittancePackingOffsetCurrent);
int3 levelAddressPrevious = GetLevelAddress(int3(pageCoords.coords), g_TranslationParamsPrevious, g_ToroidalOffsetPrevious.xyz, g_LevelSizePrevious);
int3 readCoordinate = levelAddressPrevious + int3(offset) * 2 + int3(0, 0, g_EmittancePackingOffsetPrevious);
bool hasEmittance = false;
float interpolationFactor = 0;
if(bool(g_UseEmittanceInterpolation) && g_LevelToProcess > 0)
{
int scale = int(1 << (int(g_StackSize) - int(g_LevelToProcess) - 1));
float rMapSize = rcp(float(g_LevelSizeCurrent));
float pageSize = (1 << g_AllocationLodBias) * rMapSize * scale;
float3 v = abs(float3(pageCoords.coords) * pageSize - 0.5 * scale + float3(offset) * rMapSize - g_ClipmapAnchorOffset.xyz) + pageSize;
interpolationFactor = saturate(8 * max(max(v.x, v.y), v.z) - 1);
}
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
if(interpolationFactor > 0)
result = lerp(result, VxgiUnpackEmittance(EMITTANCE_STORAGE_TYPE(LOAD_EMITTANCE_FROM_UAV(u_EmittanceDst, writeAddr))), interpolationFactor);
EMITTANCE_STORAGE_TYPE emittance = VxgiPackEmittance(result);
STORE_EMITTANCE_TO_UAV(u_EmittanceDst, writeAddr, emittance);
if(any(result != float4scalar(0)))
hasEmittance = true;
directionOffset += int(g_PackingStride);
}
if(hasEmittance)
{
int3 opacityAddress = levelAddressCurrent + int3(offset) + int3(0, 0, g_OpacityPackingOffsetCurrent);
MARK_OPACITY(opacityAddress);
}
}
PAGE_PROCESSING_CS_GROUP(DownsampleEmittancePagesBody,t_PagesToProcess,g_ListParams)
