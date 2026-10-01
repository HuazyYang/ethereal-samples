#include "VoxelTextureCommon.hlsli"

Buffer<uint> t_PagesToProcess: register(t0);
RWTexture3D<uint> u_CoverageTexture: register(u0);
#if USE_6D_OPACITY
RWTexture3D<uint> u_CoverageTextureNeg: register(u1);
#endif
void ClearOpacityPagesBody(PageCoordinates pageCoords, uint3 offset)
{
int3 levelAddress = GetLevelAddress(int3(pageCoords.coords), g_TranslationParamsCurrent, g_ToroidalOffsetCurrent.xyz, g_LevelSizeCurrent);
int3 voxelPosition = levelAddress + int3(offset) + int3(0, 0, g_OpacityPackingOffsetCurrent);
u_CoverageTexture[voxelPosition] = 0;
#if USE_6D_OPACITY
u_CoverageTextureNeg[voxelPosition] = 0;
#endif
}
PAGE_PROCESSING_CS_GROUP(ClearOpacityPagesBody, t_PagesToProcess, g_ListParams)
