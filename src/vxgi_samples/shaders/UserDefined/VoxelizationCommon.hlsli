#ifndef VOXELIZATIONCOMMON_HLSLI
#define VOXELIZATIONCOMMON_HLSLI
#include "../Common/ShaderCommon.hlsli"
#include "UserDefinedConstants.hlsli"

#define USE_SCISSOR_STATS 0
#define TEST_SCISSOR_REGIONS_IN_GS 1
#define CLIP_LEVELS_TO_INSTANCE 5
#define OPACITY_SAMPLE_COUNT 8
#define EMITTANCE_SAMPLE_COUNT 8
#define Z_CLIP_EXTENSION 0.03125
#if USE_SCISSOR_STATS
#define UPDATE_SCISSOR_STATS(stat) InterlockedAdd(u_ScissorStats[0].stat, 1)
#else
#define UPDATE_SCISSOR_STATS(stat)
#endif
struct VxgiVoxelizationConstants {
    float4 GridCenter;
    float4 GridCenterPrevious;
    int4 ToroidalOffset;
    VxgiBox4f ScissorRegionsClipSpace[MAX_STACK_LEVELS];
    int4 TextureToAmapTranslation[MAX_STACK_LEVELS];
    float IrradianceMapSize;
    uint ClipLevelSize;
    uint PackingStride;
    uint AllocationMapSize;
    uint ClipLevelMask;
    uint MaxClipLevel;
    int FirstLevelToDiscard;
    float DiscardLower;
    float DiscardUpper;
    float DiscardClipSpace;
    uint UseCullFunction;
    float EmittanceStorageScale;
    uint UseIrradianceMap;
    uint Use6DOpacity;
    uint UseFP32Emittance;
    uint PersistentVoxelData;
    uint UseInvalidateBitmap;
};
struct VxgiVoxelizationMaterialConstants {
    float4 ResolutionFactors[MAX_STACK_LEVELS];
    float NoiseScale;
    float NoiseBias;
    int TwoSided;
    int ProportionalEmittance;
    int DepthSamples;
    int FrontCCW;
    int OmnidirectionalLight;
};
cbuffer VoxelizationCB : REGISTER_CBUFFER(VXGI_VOXELIZE_CB_SLOT, VXGI_RESOURCE_SPACE)
{
    VxgiVoxelizationConstants g_VxgiVoxelizationCB;
};
cbuffer VoxelizationMaterialCB : REGISTER_CBUFFER(VXGI_VOXELIZE_MATERIAL_CB_SLOT, VXGI_RESOURCE_SPACE)
{
    VxgiVoxelizationMaterialConstants g_VxgiVoxelizationMaterialCB;
};
struct VxgiVoxelizationGSOutputData {
    nointerpolation uint gl_ViewportIndex : SV_ViewportArrayIndex;
#if COVERAGE_WITH_EDGE_EQUATIONS
    nointerpolation float3 Edge1 : EDGE1;
    nointerpolation float3 Edge2 : EDGE2;
    nointerpolation float3 Edge3 : EDGE3;
    nointerpolation float4 ProjectedBoundingBox : PROJECTED_BBOX;
#endif
};
// struct VxgiVoxelizationPSInputData {
//     float4 gl_FragCoord : SV_Position;
//     uint gl_SampleMaskIn : SV_Coverage;
//     VxgiVoxelizationGSOutputData GSData;
//     bool gl_FrontFacing : SV_IsFrontFace;
// };

struct VxgiVoxelizationPSInputData {
    float4 gl_FragCoord : SV_Position;
    VxgiVoxelizationGSOutputData GSData;
    uint gl_SampleMaskIn : SV_Coverage;
    bool gl_FrontFacing : SV_IsFrontFace;
};

struct VxgiScissorStats {
    uint pixelsRasterized;
    uint pixelsKilledByZViewport;
    uint pixelsKilledByScissorBox;
    uint pixelsKilledByFinerLOD;
    uint voxelsWithNoCoverage;
    uint voxelsKilledByScissorBox;
    uint voxelsKilledByAllocationMap;
};
static const float2 g_VxgiSamplePositions[] = {
    float2(0.0625, -0.1875), float2(-0.0625, 0.1875), float2(0.3125, 0.0625), float2(-0.1875, -0.3125),
    float2(-0.3125, 0.3125), float2(-0.4375, 0.0625), float2(0.1875, 0.4375), float2(0.4375, -0.4375),
    float2(0, 0)};
static const float g_VxgiSampleZOffsets[] = {
    0.9, 0.4, 0.5, 0.6, 0.1, 0.8, 0.7, 0.3, 0.2};
bool VxgiIsVoxelInDiscardArea(float3 voxelCoordinates, int clipLevel)
{
    if (clipLevel >= g_VxgiVoxelizationCB.FirstLevelToDiscard && all(voxelCoordinates >= float3scalar(g_VxgiVoxelizationCB.DiscardLower)) && all(voxelCoordinates <= float3scalar(g_VxgiVoxelizationCB.DiscardUpper))) {
        UPDATE_SCISSOR_STATS(pixelsKilledByFinerLOD);
        return true;
    }
    return false;
}
#endif /* VOXELIZATIONCOMMON_HLSLI */
