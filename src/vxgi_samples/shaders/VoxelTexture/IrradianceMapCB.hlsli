#ifndef IRRADIANCEMAPCB_HLSLI
#define IRRADIANCEMAPCB_HLSLI
#include "../AllocationMap/Page.hlsli"

cbuffer IrradianceMapCB: register(b0)
{
    MultiListParams g_ListParams;
    float4 g_ClipmapOrigin;
    float4 g_IrradianceVoxelSize;
    int4 g_ToroidalOffsetForIrradiance;
    int4 g_SourceLevelSize;
    uint g_IrradianceMapSize;
    uint g_LogPageSize;
    uint g_TracingMaxSamples;
    float g_TracingStep;
    float g_TracingOpacityCorrectionFactor;
    float g_TracingIrradianceScale;
    uint g_TracingFlipOpacityDirections;
    float g_TracingConeFactor;
    uint g_UseIrradianceNormalization;
    float g_IrradianceClampValue;
};
#define STRUCT_NUM_PARTIAL_SUMS 0x400
#define STRUCT_NUM_DERIVATIVES 5
#define STRUCT_PARTIAL_SUMS 0x0
#define STRUCT_VOXEL_COUNT 0x400
#define STRUCT_IRRADIANCE_MULTIPLIER 0x401
#define STRUCT_LAST_AVERAGE_IRRADIANCE 0x402
#define STRUCT_NEXT_DERIVATIVE 0x403
#define STRUCT_DERIVATIVES 0x404

#endif /* IRRADIANCEMAPCB_HLSLI */
