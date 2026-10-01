#include "VoxelTextureCommon.hlsli"
Buffer<uint> t_PagesToProcess: register(t0);
#if EMITTANCE_FORMAT == FLOAT32
RWTexture3D<uint> u_EmittanceR: register(u7);
RWTexture3D<uint> u_EmittanceG: register(u8);
RWTexture3D<uint> u_EmittanceB: register(u9);
#elif EMITTANCE_FORMAT == UNORM8
RWTexture3D<uint> u_Emittance: register(u7);
#endif
RWTexture3D<uint> u_Opacity_Pos: register(u2);
void ConvertEmissivePagesBody(PageCoordinates pageCoords, uint3 offset)
{
int3 levelAddress = GetLevelAddress(int3(pageCoords.coords), g_TranslationParamsCurrent, g_ToroidalOffsetCurrent.xyz, g_LevelSizeCurrent);
int3 voxelPosition = levelAddress + int3(offset) + int3(1, 0, g_EmittancePackingOffsetCurrent);
float3 color;
float colorScale = pow(0.5, EMITTANCE_FIXED_POINT_BITS);
bool hasEmittance = false;
#if EMITTANCE_FORMAT == FLOAT32
for (uint direction = 0; direction < EMITTANCE_DIRECTIONS; ++direction)
{
color.r = float(u_EmittanceR[voxelPosition].x) * colorScale;
color.g = float(u_EmittanceG[voxelPosition].x) * colorScale;
color.b = float(u_EmittanceB[voxelPosition].x) * colorScale;
if(any(color.rgb != float3scalar(0)))
{
u_EmittanceR[voxelPosition] = STORE_EMITTANCE(asuint(color.r));
u_EmittanceG[voxelPosition] = STORE_EMITTANCE(asuint(color.g));
u_EmittanceB[voxelPosition] = STORE_EMITTANCE(asuint(color.b));
hasEmittance = true;
}
voxelPosition.x += int(g_PackingStride);
}
#else
color.r = float(u_Emittance[voxelPosition + int3(g_PackingStride * EMITTANCE_POSITIVE_X, 0, 0)].x) * colorScale;
color.g = float(u_Emittance[voxelPosition + int3(g_PackingStride * EMITTANCE_POSITIVE_Y, 0, 0)].x) * colorScale;
color.b = float(u_Emittance[voxelPosition + int3(g_PackingStride * EMITTANCE_POSITIVE_Z, 0, 0)].x) * colorScale;
EMITTANCE_STORAGE_TYPE encoding = 0;
bool omni = true;
float3 normal;
uint normalX = u_Emittance[voxelPosition + int3(g_PackingStride * EMITTANCE_NEGATIVE_X, 0, 0)].x;
omni = (normalX & 1) != 0;
normal.x = float(int(normalX & ~1));
normal.y = float(int(u_Emittance[voxelPosition + int3(g_PackingStride * EMITTANCE_NEGATIVE_Y, 0, 0)].x));
normal.z = float(int(u_Emittance[voxelPosition + int3(g_PackingStride * EMITTANCE_NEGATIVE_Z, 0, 0)].x));
normal.xyz *= colorScale;
float rcpNormalLength = rsqrt(dot(normal, normal));
if(rcpNormalLength > 100)
omni = true;
if(!omni)
{
normal *= rcpNormalLength;
[unroll]
for (uint direction = 0; direction < EMITTANCE_DIRECTIONS; ++direction)
{
float multiplier = VxgiGetNormalProjection(normal, direction);
encoding = VxgiPackEmittance(float4(color * multiplier, 0));
u_Emittance[voxelPosition + int3(g_PackingStride * direction, 0, 0)] = STORE_EMITTANCE(encoding);
if(encoding != 0)
hasEmittance = true;
}
}
else
{
encoding = VxgiPackEmittance(float4(color / 6.0, 0));
for (uint direction = 0; direction < EMITTANCE_DIRECTIONS; ++direction)
{
u_Emittance[voxelPosition + int3(g_PackingStride * direction, 0, 0)] = STORE_EMITTANCE(encoding);
}
if(encoding != 0)
hasEmittance = true;
}
#endif
if(hasEmittance)
{
int3 opacityAddress = levelAddress + int3(offset) + int3(0, 0, g_OpacityPackingOffsetCurrent);
MARK_OPACITY(opacityAddress);
}
}
PAGE_PROCESSING_CS_GROUP(ConvertEmissivePagesBody,t_PagesToProcess,g_ListParams)
