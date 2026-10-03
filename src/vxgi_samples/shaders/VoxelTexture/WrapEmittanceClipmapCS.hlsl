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
if(gfsdk_GlobalIdx.x < g_LevelSizeCurrent + 2 && gfsdk_GlobalIdx.y < g_LevelSizeCurrent)
{
int3 dstPosition;
dstPosition.xy = int2(gfsdk_GlobalIdx.xy);
dstPosition.z = int(g_EmittancePackingOffsetCurrent) - 1;
int3 srcPosition;
srcPosition.x = int(((gfsdk_GlobalIdx.x - 1) & (g_LevelSizeCurrent - 1)) + 1);
srcPosition.yz = dstPosition.yz;
for(uint direction = 0; direction < EMITTANCE_DIRECTIONS; direction++)
{
int3 srcPosition1 = srcPosition+int3(0,0,g_LevelSizeCurrent);
int3 srcPosition2 = srcPosition+int3(0,0,1);
EMITTANCE_STORAGE_TYPE emittance1 = EMITTANCE_STORAGE_TYPE(LOAD_EMITTANCE_FROM_UAV(u_Emittance, srcPosition1));
EMITTANCE_STORAGE_TYPE emittance2 = EMITTANCE_STORAGE_TYPE(LOAD_EMITTANCE_FROM_UAV(u_Emittance, srcPosition2));
int3 dstPosition1 = dstPosition;
int3 dstPosition2 = dstPosition+int3(0,0,g_LevelSizeCurrent+1);
STORE_EMITTANCE_TO_UAV(u_Emittance, dstPosition1, emittance1);
STORE_EMITTANCE_TO_UAV(u_Emittance, dstPosition2, emittance2);
srcPosition.x += int(g_PackingStride);
dstPosition.x += int(g_PackingStride);
}
}
if(gfsdk_GlobalIdx.x < g_LevelSizeCurrent && gfsdk_GlobalIdx.y < g_LevelSizeCurrent)
{
int3 dstPosition;
dstPosition.x = 0;
dstPosition.y = int(gfsdk_GlobalIdx.x);
dstPosition.z = int(gfsdk_GlobalIdx.y) + int(g_EmittancePackingOffsetCurrent);
for(uint direction = 0; direction < EMITTANCE_DIRECTIONS; direction++)
{
int3 srcPosition1 = dstPosition+int3(g_LevelSizeCurrent,0,0);
int3 srcPosition2 = dstPosition+int3(1,0,0);
EMITTANCE_STORAGE_TYPE emittance1 = EMITTANCE_STORAGE_TYPE(LOAD_EMITTANCE_FROM_UAV(u_Emittance, srcPosition1));
EMITTANCE_STORAGE_TYPE emittance2 = EMITTANCE_STORAGE_TYPE(LOAD_EMITTANCE_FROM_UAV(u_Emittance, srcPosition2));
int3 dstPosition1 = dstPosition;
int3 dstPosition2 = dstPosition+int3(g_LevelSizeCurrent+1,0,0);
STORE_EMITTANCE_TO_UAV(u_Emittance, dstPosition1, emittance1);
STORE_EMITTANCE_TO_UAV(u_Emittance, dstPosition2, emittance2);
dstPosition.x += int(g_PackingStride);
}
}
}
