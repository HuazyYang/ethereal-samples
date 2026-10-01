#ifndef ALLOCATIONMAPCB_HLSLI
#define ALLOCATIONMAPCB_HLSLI
cbuffer AllocationMapCB : register(b0)
{
    MultiListParams g_FilteredPageListParams[MAX_PAGED_LEVELS];
    MultiListParams g_IrradianceMapPageListParams;
    uint4 g_GridSizeParams[MAX_PAGED_LEVELS];
    uint4 g_IrradianceMapGridSizeParams;
    int4 g_ToroidalOffset;
    int4 g_ToroidalOffsetPrevious;
    float4 g_ClipmapOrigin;
    uint g_TextureSize;
    uint g_NumberOfRegions;
    uint g_MaxClipLevel;
    uint g_NumberOfFrusta;
    uint g_NumPagedLevels;
    uint g_OpacityDownsampleAllPresentPages;
    uint g_OpacityVoxelizeAllLevels;
    uint g_StoreEmittanceInFP16;
    uint g_EmittanceVoxelizeAllLevels;
    uint g_WriteIrradianceMapPages;
};
#endif /* ALLOCATIONMAPCB_HLSLI */
