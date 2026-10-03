#ifndef VOXELTEXTURECOMMON_HLSLI
#define VOXELTEXTURECOMMON_HLSLI
#include "../AllocationMap/Page.hlsli"
cbuffer ListProcessingCB: register(b0)
{
    MultiListParams g_ListParams;
    int4 g_ToroidalOffsetCurrent;
    int4 g_ToroidalOffsetPrevious;
    int4 g_TranslationParamsCurrent;
    int4 g_TranslationParamsPrevious;
    int4 g_TextureSize;
    float4 g_ClipmapAnchorOffset;
    float4 g_rEmittanceTextureSize;
    int g_LevelSizeCurrent;
    int g_LevelSizePrevious;
    uint g_AllocationLodBias;
    uint g_LevelToProcess;
    uint g_StackSize;
    uint g_NumPagedLevels;
    uint g_PackingStride;
    uint g_OpacityPackingOffsetCurrent;
    uint g_OpacityPackingOffsetPrevious;
    uint g_EmittancePackingOffsetCurrent;
    uint g_EmittancePackingOffsetPrevious;
    uint g_UseEmittanceInterpolation;
    uint g_PersistentVoxelData;
    float g_CoverageBitCountMultiplier;
};
int3 GetLevelAddress(int3 pageCoords, int4 translationParams, int3 toroidalOffset, int levelSize)
{
    int3 voxelCoord = (pageCoords.xyz - translationParams.xyz) << translationParams.w;
    int3 address = TOROIDAL_ADDRESS(voxelCoord, toroidalOffset.xyz, int3scalar(levelSize));
    return address;
}
void GetLevelAndBackupAddress(int3 pageCoords, uint level, uint3 offset, out int3 levelAddress, out int3 backupAddress, out bool isInBackup)
{
    int3 voxelCoord = ((pageCoords.xyz - g_TranslationParamsCurrent.xyz) << g_TranslationParamsCurrent.w) + int3(offset);
    levelAddress = TOROIDAL_ADDRESS(voxelCoord, g_ToroidalOffsetCurrent.xyz, int3scalar(g_LevelSizeCurrent));
    int backupOrigin = g_LevelSizeCurrent >> 2;
    int backupSize = g_LevelSizeCurrent >> 1;
    int3 backupCoord = voxelCoord - backupOrigin;
    isInBackup = (level > 0) && (level < g_StackSize) && all(backupCoord >= int3(0, 0, 0)) && all(backupCoord < int3scalar(backupSize));
    backupAddress = TOROIDAL_ADDRESS(backupCoord, g_ToroidalOffsetCurrent.xyz, backupSize);
    backupAddress.z += backupSize * (int(level) - 1);
}
#endif /* VOXELTEXTURECOMMON_HLSLI */
