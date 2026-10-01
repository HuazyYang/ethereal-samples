#include "VoxelTextureCommon.hlsli"
Buffer<uint> t_PagesToProcess : register(t0);
#if EMITTANCE_FORMAT == FLOAT32
RWTexture3D<uint> u_EmittanceR : register(u7);
RWTexture3D<uint> u_EmittanceG : register(u8);
RWTexture3D<uint> u_EmittanceB : register(u9);
#elif EMITTANCE_FORMAT == FLOAT16
RWTexture3D<float4> u_Emittance : register(u7);
#else
RWTexture3D<uint> u_Emittance : register(u7);
#endif
RWTexture3D<uint> u_Opacity_Pos : register(u2);
void ClearEmittancePagesBody(PageCoordinates pageCoords, uint3 offset)
{
    int3 levelAddress = GetLevelAddress(int3(pageCoords.coords), g_TranslationParamsCurrent, g_ToroidalOffsetCurrent.xyz, g_LevelSizeCurrent);
    int3 voxelPosition = levelAddress + int3(offset) + int3(0, 0, g_EmittancePackingOffsetCurrent);
#if PERSISTENT_VOXEL_DATA
    {
        int3 opacityAddress = levelAddress + int3(offset) + int3(0, 0, g_OpacityPackingOffsetCurrent);
        CLEAR_OPACITY(opacityAddress);
    }
#endif
    voxelPosition.x += 1;
    for (uint direction = 0; direction < EMITTANCE_DIRECTIONS; ++direction) {
        EMITTANCE_STORAGE_TYPE emittance = VxgiPackEmittance(float4scalar(0));
        STORE_EMITTANCE_TO_UAV(u_Emittance, voxelPosition, emittance);
        voxelPosition.x += int(g_PackingStride);
    }
}
PAGE_PROCESSING_CS_GROUP(ClearEmittancePagesBody, t_PagesToProcess, g_ListParams)
