#include "VoxelizationCommon.hlsli"
static const int VxgiIsEmissiveVoxelizationPass = 0;
RWTexture3D<uint> u_AllocationMap : REGISTER_UAV(VXGI_ALLOCATION_MAP_UAV_SLOT, VXGI_RESOURCE_SPACE);
RWTexture3D<uint> u_CoverageTextureXYZ_Pos : REGISTER_UAV(VXGI_COVERAGE_POS_UAV_SLOT, VXGI_RESOURCE_SPACE);
RWTexture3D<uint> u_CoverageTextureXYZ_Neg : REGISTER_UAV(VXGI_COVERAGE_NEG_UAV_SLOT, VXGI_RESOURCE_SPACE);
RWStructuredBuffer<VxgiScissorStats> u_ScissorStats : REGISTER_UAV(VXGI_SCISSOR_STATS_UAV_SLOT, VXGI_RESOURCE_SPACE);
StructuredBuffer<uint4> t_VoxelizationCoverageMasks : REGISTER_SRV(VXGI_COVERAGE_MASKS_SRV_SLOT, VXGI_RESOURCE_SPACE);
uint4 VxgiGetLayeredCoverage(float inputZ, int projectionIndex, uint coverage, float depthOffset)
{
    uint4 layeredCoverage = uint4(0, 0, 0, 0);
    float2 dZ = float2(ddx(inputZ), ddy(inputZ));
    float fracZ = frac(inputZ);
    if (depthOffset < 0)
        fracZ += 2 + depthOffset * g_VxgiVoxelizationMaterialCB.DepthSamples - g_VxgiVoxelizationMaterialCB.NoiseScale - g_VxgiVoxelizationMaterialCB.NoiseBias;
    else
        fracZ += 1 + g_VxgiVoxelizationMaterialCB.NoiseBias;
    int coverageTableBase = (g_VxgiVoxelizationMaterialCB.DepthSamples * 3 + projectionIndex) * 9 * OPACITY_SAMPLE_COUNT;
    [unroll]
    for (int nSample = 0; nSample < OPACITY_SAMPLE_COUNT; nSample++) {
        if ((coverage & (1u << nSample)) != 0) {
            float relativeZ = fracZ + dot(dZ, g_VxgiSamplePositions[nSample]);
            relativeZ += g_VxgiSampleZOffsets[nSample] * g_VxgiVoxelizationMaterialCB.NoiseScale;
            int integerZ = max(0, min(8, int(relativeZ * 3)));
            int coverageIndex = coverageTableBase + integerZ * OPACITY_SAMPLE_COUNT + nSample;
            layeredCoverage |= t_VoxelizationCoverageMasks[coverageIndex];
        }
    }
    return layeredCoverage;
}
void VxgiWriteCoverage(int3 coordinates, int clipLevel, uint coveragePos, uint coverageNeg, inout int prevPageCoordinatesHash, inout uint page)
{
    if ((coveragePos | coverageNeg) == 0) {
        UPDATE_SCISSOR_STATS(voxelsWithNoCoverage);
        return;
    }
    if (((coordinates.x | coordinates.y | coordinates.z) & ~(g_VxgiVoxelizationCB.ClipLevelSize - 1)) != 0) {
        return;
    }
    {
        int4 translation = g_VxgiVoxelizationCB.TextureToAmapTranslation[clipLevel];
        int3 pageCoordinates = int3(((coordinates >> translation.w) + translation.xyz) & (g_VxgiVoxelizationCB.AllocationMapSize - 1));
        int pageCoordinatesHash = pageCoordinates.x + pageCoordinates.y + pageCoordinates.z;
        if (pageCoordinatesHash != prevPageCoordinatesHash)
            page = u_AllocationMap[pageCoordinates].x;
        prevPageCoordinatesHash = pageCoordinatesHash;
        if ((page & AMAP_GEOMETRY_DIRTY_BIT) == 0) {
            UPDATE_SCISSOR_STATS(voxelsKilledByAllocationMap);
            return;
        }
        if ((page & AMAP_PRESENT_BIT) == 0) {
            page |= AMAP_PRESENT_BIT;
            u_AllocationMap[pageCoordinates] = page;
        }
    }
    int3 address = int3(TOROIDAL_ADDRESS(coordinates, g_VxgiVoxelizationCB.ToroidalOffset.xyz >> clipLevel, g_VxgiVoxelizationCB.ClipLevelSize));
    address += int3(0, 0, g_VxgiVoxelizationCB.PackingStride * clipLevel + 1);
    if (coveragePos != 0)
        InterlockedOr(u_CoverageTextureXYZ_Pos[address], coveragePos);
    if (coverageNeg != 0 && bool(g_VxgiVoxelizationCB.Use6DOpacity))
        InterlockedOr(u_CoverageTextureXYZ_Neg[address], coverageNeg);
}
void VxgiVoxelizeOpacityPS(VxgiVoxelizationPSInputData IN)
{
    UPDATE_SCISSOR_STATS(pixelsRasterized);
    bool frontFacing = IN.gl_FrontFacing;
    if (g_VxgiVoxelizationMaterialCB.FrontCCW != 0)
        frontFacing = !frontFacing;
    float fVP = float(IN.GSData.gl_ViewportIndex) * 0.334;
    int projectionIndex = int(floor(frac(fVP) * 3));
    int clipLevel = int(floor(fVP));
    float inputZ = IN.gl_FragCoord.z;
    float unscaledZ = inputZ;
    inputZ *= (1u << (g_VxgiVoxelizationCB.MaxClipLevel - clipLevel));
    inputZ = inputZ * 0.5 + 0.5;
    inputZ *= g_VxgiVoxelizationCB.ClipLevelSize;
    const float DesnapQuantum = 0.0078125;
    if (abs(inputZ - round(inputZ)) < DesnapQuantum)
        inputZ += 2 * DesnapQuantum;
    float3 texelSpace = float3(IN.gl_FragCoord.xy, inputZ);
    texelSpace.y = g_VxgiVoxelizationCB.ClipLevelSize - texelSpace.y;
    int3 offsetDir;
    bool kill = false;
    if (projectionIndex == 0) {
        kill = unscaledZ < g_VxgiVoxelizationCB.ScissorRegionsClipSpace[clipLevel].lower.z || unscaledZ > g_VxgiVoxelizationCB.ScissorRegionsClipSpace[clipLevel].upper.z;
        offsetDir = int3(0, 0, 1);
    } else if (projectionIndex == 1) {
        kill = unscaledZ < g_VxgiVoxelizationCB.ScissorRegionsClipSpace[clipLevel].lower.y || unscaledZ > g_VxgiVoxelizationCB.ScissorRegionsClipSpace[clipLevel].upper.y;
        texelSpace.zxy = texelSpace.xyz;
        offsetDir = int3(0, 1, 0);
    } else {
        kill = unscaledZ < g_VxgiVoxelizationCB.ScissorRegionsClipSpace[clipLevel].lower.x || unscaledZ > g_VxgiVoxelizationCB.ScissorRegionsClipSpace[clipLevel].upper.x;
        texelSpace.yzx = texelSpace.xyz;
        offsetDir = int3(1, 0, 0);
    }
    if (kill) {
        discard;
        return;
    }
    int3 voxelCoordinates = int3(floor(texelSpace));
    float depthOffset = frontFacing ? 0.334 : -0.334;
    if (!frontFacing)
        voxelCoordinates -= offsetDir;
    uint coverage = IN.gl_SampleMaskIn;
    uint4 layeredCoverage = VxgiGetLayeredCoverage(inputZ, projectionIndex, coverage, depthOffset);
    int pageCoordinatesHash = -1;
    uint page = 0;
    int positiveMask;
    int negativeMask;
    if (bool(g_VxgiVoxelizationMaterialCB.TwoSided) || !bool(g_VxgiVoxelizationCB.Use6DOpacity)) {
        positiveMask = 0x3FFFFFFF;
        negativeMask = 0x3FFFFFFF;
    } else {
        float3 normal = normalize(cross(ddx(texelSpace), ddy(texelSpace)));
        if (!frontFacing)
            normal = -normal;
        positiveMask = normal.x > -0.01f ? 0x000003FF : 0;
        positiveMask |= normal.y > -0.01f ? 0x000FFC00 : 0;
        positiveMask |= normal.z > -0.01f ? 0x3FF00000 : 0;
        negativeMask = normal.x < 0.01f ? 0x000003FF : 0;
        negativeMask |= normal.y < 0.01f ? 0x000FFC00 : 0;
        negativeMask |= normal.z < 0.01f ? 0x3FF00000 : 0;
    }
    voxelCoordinates -= offsetDir;
    // [unroll]
    for (uint voxel = 0; voxel < 4; voxel++) {
        VxgiWriteCoverage(voxelCoordinates, clipLevel, layeredCoverage.x & positiveMask, layeredCoverage.x & negativeMask, pageCoordinatesHash, page);
        voxelCoordinates += offsetDir;
        layeredCoverage.xyz = layeredCoverage.yzw;
    }
    discard;
}
float3 VxgiGetIndirectIrradiance(float3 worldPos, float3 normal)
{
    return float3scalar(0);
}
void VxgiStoreVoxelizationData(VxgiVoxelizationPSInputData inputData, float3 emissiveColor)
{
    VxgiVoxelizeOpacityPS(inputData);
}
#ifdef TEST_COMPILE
void main(float4 position: SV_Position, VxgiVoxelizationPSInputData vxgiData)
{
    VxgiStoreVoxelizationData(vxgiData, float3(1, 1, 1));
}
#endif 