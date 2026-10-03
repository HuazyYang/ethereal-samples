#include "VoxelTextureCommon.hlsli"
#if EMITTANCE_FORMAT == FLOAT32
RWTexture3D<uint> u_EmittanceR: register(u7);
RWTexture3D<uint> u_EmittanceG: register(u8);
RWTexture3D<uint> u_EmittanceB: register(u9);
#elif EMITTANCE_FORMAT == FLOAT16 || EMITTANCE_FORMAT == FLOAT16_NVAPI
RWTexture3D<float4> u_Emittance: register(u7);
#else
RWTexture3D<uint> u_Emittance: register(u7);
#endif
[numthreads(8, 8, 1)]
void main(in uint3 gfsdk_GroupIdx: SV_GroupID, in uint3 gfsdk_GroupThreadIdx: SV_GroupThreadID, in uint3 gfsdk_GlobalIdx: SV_DispatchThreadID)
{
if (all(gfsdk_GlobalIdx.xy < int2scalar(g_LevelSizeCurrent + 2)))
{
int3 dstPosition;
dstPosition.x = int(gfsdk_GlobalIdx.x);
dstPosition.y = int(gfsdk_GlobalIdx.y - 1) & int(g_TextureSize.y - 1);
dstPosition.z = gfsdk_GlobalIdx.z == 0
? int(g_EmittancePackingOffsetCurrent) - 1
: int(g_EmittancePackingOffsetCurrent + g_LevelSizeCurrent);
int3 srcPosition;
srcPosition.x = (int(gfsdk_GlobalIdx.x - 1) & int(g_LevelSizeCurrent - 1)) + 1;
srcPosition.y = int(gfsdk_GlobalIdx.y - 1) & int(g_LevelSizeCurrent - 1);
srcPosition.z = gfsdk_GlobalIdx.z == 0
? int(g_EmittancePackingOffsetCurrent + g_LevelSizeCurrent) - 1
: int(g_EmittancePackingOffsetCurrent);
for(int direction = 0; direction < EMITTANCE_DIRECTIONS; direction++)
{
EMITTANCE_STORAGE_TYPE emittance = EMITTANCE_STORAGE_TYPE(LOAD_EMITTANCE_FROM_UAV(u_Emittance, srcPosition));
STORE_EMITTANCE_TO_UAV(u_Emittance, dstPosition, emittance);
srcPosition.x += int(g_PackingStride);
dstPosition.x += int(g_PackingStride);
}
}
if (gfsdk_GlobalIdx.x < g_LevelSizeCurrent + 2 && gfsdk_GlobalIdx.y < g_LevelSizeCurrent)
{
int3 dstPosition;
dstPosition.x = int(gfsdk_GlobalIdx.x);
dstPosition.z = int(gfsdk_GlobalIdx.y + g_EmittancePackingOffsetCurrent);
dstPosition.y = gfsdk_GlobalIdx.z == 0
? int(g_LevelSizeCurrent)
: int(g_TextureSize.y) - 1;
int3 srcPosition;
srcPosition.x = (int(gfsdk_GlobalIdx.x - 1) & int(g_LevelSizeCurrent - 1)) + 1;
srcPosition.z = int(gfsdk_GlobalIdx.y + g_EmittancePackingOffsetCurrent);
srcPosition.y = gfsdk_GlobalIdx.z == 0
? 0
: int(g_LevelSizeCurrent) - 1;
for(int direction = 0; direction < EMITTANCE_DIRECTIONS; direction++)
{
EMITTANCE_STORAGE_TYPE emittance = EMITTANCE_STORAGE_TYPE(LOAD_EMITTANCE_FROM_UAV(u_Emittance, srcPosition));
STORE_EMITTANCE_TO_UAV(u_Emittance, dstPosition, emittance);
srcPosition.x += int(g_PackingStride);
dstPosition.x += int(g_PackingStride);
}
}
if (all(gfsdk_GlobalIdx.xy < int2scalar(g_LevelSizeCurrent)))
{
int3 dstPosition;
dstPosition.y = int(gfsdk_GlobalIdx.x);
dstPosition.z = int(gfsdk_GlobalIdx.y + g_EmittancePackingOffsetCurrent);
dstPosition.x = gfsdk_GlobalIdx.z == 0
? int(g_LevelSizeCurrent) + 1
: 0;
int3 srcPosition;
srcPosition.y = int(gfsdk_GlobalIdx.x);
srcPosition.z = int(gfsdk_GlobalIdx.y + g_EmittancePackingOffsetCurrent);
srcPosition.x = gfsdk_GlobalIdx.z == 0
? 1
: int(g_LevelSizeCurrent);
for(int direction = 0; direction < EMITTANCE_DIRECTIONS; direction++)
{
EMITTANCE_STORAGE_TYPE emittance = EMITTANCE_STORAGE_TYPE(LOAD_EMITTANCE_FROM_UAV(u_Emittance, srcPosition));
STORE_EMITTANCE_TO_UAV(u_Emittance, dstPosition, emittance);
srcPosition.x += int(g_PackingStride);
dstPosition.x += int(g_PackingStride);
}
}
}
