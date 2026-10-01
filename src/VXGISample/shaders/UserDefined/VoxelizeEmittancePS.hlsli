#include "VoxelizationCommon.hlsli"
static const int VxgiIsEmissiveVoxelizationPass = 1;
RWTexture3D<uint> u_AllocationMap : REGISTER_UAV(VXGI_ALLOCATION_MAP_UAV_SLOT, VXGI_RESOURCE_SPACE);
#if EMITTANCE_FORMAT == UNORM8
RWTexture3D<uint> u_EmittanceEvenR : REGISTER_UAV(VXGI_EMITTANCE_EVEN_R_UAV_SLOT, VXGI_RESOURCE_SPACE);
RWTexture3D<uint> u_EmittanceEvenG : REGISTER_UAV(VXGI_EMITTANCE_EVEN_G_UAV_SLOT, VXGI_RESOURCE_SPACE);
RWTexture3D<uint> u_EmittanceEvenB : REGISTER_UAV(VXGI_EMITTANCE_EVEN_B_UAV_SLOT, VXGI_RESOURCE_SPACE);
RWTexture3D<uint> u_EmittanceOddR : REGISTER_UAV(VXGI_EMITTANCE_ODD_R_UAV_SLOT, VXGI_RESOURCE_SPACE);
RWTexture3D<uint> u_EmittanceOddG : REGISTER_UAV(VXGI_EMITTANCE_ODD_G_UAV_SLOT, VXGI_RESOURCE_SPACE);
RWTexture3D<uint> u_EmittanceOddB : REGISTER_UAV(VXGI_EMITTANCE_ODD_B_UAV_SLOT, VXGI_RESOURCE_SPACE);
#elif EMITTANCE_FORMAT == FLOAT16 || EMITTANCE_FORMAT == FLOAT16_NVAPI
RWTexture3D<float4> u_EmittanceEven : REGISTER_UAV(VXGI_EMITTANCE_EVEN_R_UAV_SLOT, VXGI_RESOURCE_SPACE);
RWTexture3D<float4> u_EmittanceOdd : REGISTER_UAV(VXGI_EMITTANCE_ODD_R_UAV_SLOT, VXGI_RESOURCE_SPACE);
RWTexture3D<uint> u_Opacity_Pos : REGISTER_UAV(VXGI_COVERAGE_POS_UAV_SLOT, VXGI_RESOURCE_SPACE);
#endif
RWStructuredBuffer<VxgiScissorStats> u_ScissorStats : REGISTER_UAV(VXGI_SCISSOR_STATS_UAV_SLOT, VXGI_RESOURCE_SPACE);
SamplerState s_IrradianceMapSampler : REGISTER_SAMPLER(VXGI_IRRADIANCE_MAP_SAMPLER_SLOT, VXGI_RESOURCE_SPACE);
Texture3D<float4> t_IrradianceMap : REGISTER_SRV(VXGI_IRRADIANCE_MAP_SRV_SLOT, VXGI_RESOURCE_SPACE);
#if COVERAGE_WITH_EDGE_EQUATIONS
float3 VxgiAdjustEdgeEquationForViewportSpace(float3 equation, int level, float resolutionFactor)
{
    return float3(
        equation.x,
        -equation.y,
        (equation.z * (1 << (g_VxgiVoxelizationCB.MaxClipLevel - level)) + equation.y - equation.x) * float(g_VxgiVoxelizationCB.ClipLevelSize) * 0.5 * resolutionFactor);
}
float4 VxgiAdjustBoundingBoxForViewportSpaceAndClip(float4 bbox, int level, float2 sv_position, float resolutionFactor, out bool withinPixel)
{
    bbox.yw = -bbox.wy;
    bbox = (bbox * float(1 << (g_VxgiVoxelizationCB.MaxClipLevel - level)) + 1) * (float(g_VxgiVoxelizationCB.ClipLevelSize) * 0.5 * resolutionFactor);
    float4 clipped;
    clipped.xy = max(bbox.xy, sv_position.xy - 0.5);
    clipped.zw = min(bbox.zw, sv_position.xy + 0.5);
    withinPixel = all(clipped == bbox);
    return clipped;
}
float3 VxgiGetLayeredEmittance(float inputZ, float2 sv_position, float3 e1, float3 e2, float3 e3, float4 bbox, uint clipLevel)
{
    float3 layeredEmittance = float3(0, 0, 0);
    float2 dZ = float2(ddx(inputZ), ddy(inputZ));
    float fracZ = frac(inputZ);
    float2 bboxSize = saturate(bbox.zw - bbox.xy);
    float sampleArea = bboxSize.x * bboxSize.y / EMITTANCE_SAMPLE_COUNT;
    if (sampleArea <= 0)
        return layeredEmittance;
    float3 minEdgeValues;
    const float minNormalizedFloat = asfloat(0x00800000);
    minEdgeValues.x = (e1.x > 0 || e1.x == 0 && e1.y > 0) ? minNormalizedFloat : 0;
    minEdgeValues.y = (e2.x > 0 || e2.x == 0 && e2.y > 0) ? minNormalizedFloat : 0;
    minEdgeValues.z = (e3.x > 0 || e3.x == 0 && e3.y > 0) ? minNormalizedFloat : 0;
    [unroll] for (int nSample = 0; nSample < EMITTANCE_SAMPLE_COUNT; nSample++)
    {
        float2 pos = (g_VxgiSamplePositions[nSample] + 0.5) * bboxSize + bbox.xy;
        float ee1 = (dot(e1.xy, pos) + e1.z);
        float ee2 = (dot(e2.xy, pos) + e2.z);
        float ee3 = (dot(e3.xy, pos) + e3.z);
        bool covered = ee1 >= minEdgeValues.x && ee2 >= minEdgeValues.y && ee3 >= minEdgeValues.z;
        if (covered) {
            float relativeZ = fracZ + dot(dZ, pos - sv_position.xy);
            if (g_VxgiVoxelizationMaterialCB.ProportionalEmittance != 0) {
                layeredEmittance.x += sampleArea * saturate(1 - abs(relativeZ + 0.5));
                layeredEmittance.y += sampleArea * saturate(1 - abs(relativeZ - 0.5));
                layeredEmittance.z += sampleArea * saturate(1 - abs(relativeZ - 1.5));
            } else {
                if (relativeZ < 0)
                    layeredEmittance.x += sampleArea;
                else if (relativeZ >= 1)
                    layeredEmittance.z += sampleArea;
                else
                    layeredEmittance.y += sampleArea;
            }
        }
    }
    return layeredEmittance;
}
#else
float3 VxgiGetLayeredEmittance(float inputZ, uint coverage)
{
    float3 layeredEmittance = float3(0, 0, 0);
    float2 dZ = float2(ddx(inputZ), ddy(inputZ));
    float fracZ = frac(inputZ);
    [unroll] for (int nSample = 0; nSample < EMITTANCE_SAMPLE_COUNT; nSample++)
    {
        if ((coverage & (1u << nSample)) != 0) {
            float relativeZ = fracZ + dot(dZ, g_VxgiSamplePositions[nSample]);
            const float delta = 1.0f / EMITTANCE_SAMPLE_COUNT;
            if (g_VxgiVoxelizationMaterialCB.ProportionalEmittance != 0) {
                layeredEmittance.x += delta * saturate(1 - abs(relativeZ + 0.5));
                layeredEmittance.y += delta * saturate(1 - abs(relativeZ - 0.5));
                layeredEmittance.z += delta * saturate(1 - abs(relativeZ - 1.5));
            } else {
                if (relativeZ < 0)
                    layeredEmittance.x += delta;
                else if (relativeZ >= 1)
                    layeredEmittance.z += delta;
                else
                    layeredEmittance.y += delta;
            }
        }
    }
    return layeredEmittance;
}
#endif
void VxgiWriteEmittance(int3 coordinates, int clipLevel, float perVoxelScale, float3 emittance, float3 normal, inout int prevPageCoordinatesHash, inout uint page)
{
    emittance = max(emittance * perVoxelScale, 0);
    if (!any(emittance != float3(0, 0, 0))) {
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
        if ((page & AMAP_LIGHTING_DIRTY_BIT) == 0)
            return;
        if ((page & AMAP_EMISSIVE_BIT) == 0) {
            page |= AMAP_EMISSIVE_BIT;
            u_AllocationMap[pageCoordinates] = page;
        }
    }
    int3 voxelCoords = int3(TOROIDAL_ADDRESS(coordinates, g_VxgiVoxelizationCB.ToroidalOffset.xyz >> clipLevel, g_VxgiVoxelizationCB.ClipLevelSize));
    int3 address = voxelCoords + int3(1, 0, int(g_VxgiVoxelizationCB.PackingStride) * (clipLevel >> 1) + 1);
    emittance.rgb *= g_VxgiVoxelizationCB.EmittanceStorageScale;
#if EMITTANCE_FORMAT == FLOAT16 || EMITTANCE_FORMAT == FLOAT16_NVAPI
    bool omni = bool(g_VxgiVoxelizationMaterialCB.OmnidirectionalLight) || all(normal == float3(0, 0, 0));
    [unroll] for (uint direction = 0; direction < EMITTANCE_DIRECTIONS; ++direction)
    {
        float multiplier = omni ? rcp(6.0) : VxgiGetNormalProjection(normal, direction);
        if (multiplier > 0) {
            f16vec4 packedEmittance = VxgiPackEmittanceForAtomic(float4(emittance * multiplier, 0));
            if (VxgiIsOdd(clipLevel))
                NvInterlockedAddFp16x4(u_EmittanceOdd, address, packedEmittance);
            else
                NvInterlockedAddFp16x4(u_EmittanceEven, address, packedEmittance);
        }
        address.x += int(g_VxgiVoxelizationCB.PackingStride);
    }
    MARK_OPACITY(voxelCoords + int3(0, 0, int(g_VxgiVoxelizationCB.PackingStride) * clipLevel + 1));
#else
    if (bool(g_VxgiVoxelizationCB.UseFP32Emittance)) {
        bool omni = bool(g_VxgiVoxelizationMaterialCB.OmnidirectionalLight) || all(normal == float3(0, 0, 0));
        [unroll] for (uint direction = 0; direction < EMITTANCE_DIRECTIONS; ++direction)
        {
            float multiplier = omni ? rcp(6.0) : VxgiGetNormalProjection(normal, direction);
            if (multiplier > 0) {
                uint3 packedEmittance = VxgiPackEmittanceForAtomic(emittance * multiplier);
                if (VxgiIsOdd(clipLevel)) {
                    InterlockedAdd(u_EmittanceOddR[address], packedEmittance.r);
                    InterlockedAdd(u_EmittanceOddG[address], packedEmittance.g);
                    InterlockedAdd(u_EmittanceOddB[address], packedEmittance.b);
                } else {
                    InterlockedAdd(u_EmittanceEvenR[address], packedEmittance.r);
                    InterlockedAdd(u_EmittanceEvenG[address], packedEmittance.g);
                    InterlockedAdd(u_EmittanceEvenB[address], packedEmittance.b);
                }
            }
            address.x += int(g_VxgiVoxelizationCB.PackingStride);
        }
    } else {
        float fixedPointScale = 1 << EMITTANCE_FIXED_POINT_BITS;
        uint data;
        if (emittance.r != 0) {
            data = uint(emittance.r * fixedPointScale);
            if (VxgiIsOdd(clipLevel))
                InterlockedAdd(u_EmittanceOddR[address], data);
            else
                InterlockedAdd(u_EmittanceEvenR[address], data);
        }
        address.x += int(g_VxgiVoxelizationCB.PackingStride);
        if (emittance.g != 0) {
            data = uint(emittance.g * fixedPointScale);
            if (VxgiIsOdd(clipLevel))
                InterlockedAdd(u_EmittanceOddR[address], data);
            else
                InterlockedAdd(u_EmittanceEvenR[address], data);
        }
        address.x += int(g_VxgiVoxelizationCB.PackingStride);
        if (emittance.b != 0) {
            data = uint(emittance.b * fixedPointScale);
            if (VxgiIsOdd(clipLevel))
                InterlockedAdd(u_EmittanceOddR[address], data);
            else
                InterlockedAdd(u_EmittanceEvenR[address], data);
        }
        address.x += int(g_VxgiVoxelizationCB.PackingStride);
        if (bool(g_VxgiVoxelizationMaterialCB.OmnidirectionalLight)) {
            if (VxgiIsOdd(clipLevel))
                InterlockedOr(u_EmittanceOddR[address], 1);
            else
                InterlockedOr(u_EmittanceEvenR[address], 1);
        } else {
            if (normal.x != 0) {
                data = uint(int(normal.x * fixedPointScale * perVoxelScale)) & ~1;
                if (VxgiIsOdd(clipLevel))
                    InterlockedAdd(u_EmittanceOddR[address], data);
                else
                    InterlockedAdd(u_EmittanceEvenR[address], data);
            }
            address.x += int(g_VxgiVoxelizationCB.PackingStride);
            if (normal.y != 0) {
                data = uint(int(normal.y * fixedPointScale * perVoxelScale));
                if (VxgiIsOdd(clipLevel))
                    InterlockedAdd(u_EmittanceOddR[address], data);
                else
                    InterlockedAdd(u_EmittanceEvenR[address], data);
            }
            address.x += int(g_VxgiVoxelizationCB.PackingStride);
            if (normal.z != 0) {
                data = uint(int(normal.z * fixedPointScale * perVoxelScale));
                if (VxgiIsOdd(clipLevel))
                    InterlockedAdd(u_EmittanceOddR[address], data);
                else
                    InterlockedAdd(u_EmittanceEvenR[address], data);
            }
        }
    }
#endif
}
void VoxelizeEmittancePS(VxgiVoxelizationPSInputData IN, float3 emissiveColor)
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
    float resolutionFactor = g_VxgiVoxelizationMaterialCB.ResolutionFactors[clipLevel].x;
    float rResolutionFactor = g_VxgiVoxelizationMaterialCB.ResolutionFactors[clipLevel].y;
    float3 texelSpace = float3(IN.gl_FragCoord.xy, inputZ);
    texelSpace.xy *= rResolutionFactor;
    texelSpace.y = g_VxgiVoxelizationCB.ClipLevelSize - texelSpace.y;
    if (VxgiIsVoxelInDiscardArea(texelSpace, clipLevel)) {
        discard;
        return;
    }
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
#if COVERAGE_WITH_EDGE_EQUATIONS
    float3 e1 = VxgiAdjustEdgeEquationForViewportSpace(IN.GSData.Edge1.xyz, clipLevel, resolutionFactor);
    float3 e2 = VxgiAdjustEdgeEquationForViewportSpace(IN.GSData.Edge2.xyz, clipLevel, resolutionFactor);
    float3 e3 = VxgiAdjustEdgeEquationForViewportSpace(IN.GSData.Edge3.xyz, clipLevel, resolutionFactor);
    bool withinPixel;
    float4 bbox = VxgiAdjustBoundingBoxForViewportSpaceAndClip(IN.GSData.ProjectedBoundingBox, clipLevel, IN.gl_FragCoord.xy, resolutionFactor, withinPixel);
    float3 layeredEmittance = VxgiGetLayeredEmittance(inputZ, IN.gl_FragCoord.xy, e1, e2, e3, bbox, clipLevel);
#else
    uint coverage = uint(IN.gl_SampleMaskIn);
    float3 layeredEmittance = VxgiGetLayeredEmittance(inputZ, coverage);
#endif
    float3 normal = normalize(cross(ddx(texelSpace), ddy(texelSpace)));
    if (!frontFacing)
        normal = -normal;
    float areaScale = rcp(max(abs(normal.x), max(abs(normal.y), abs(normal.z))));
    areaScale *= rResolutionFactor * rResolutionFactor;
    int pageCoordinatesHash = -1;
    uint page = 0;
    voxelCoordinates -= offsetDir;
    // [unroll]
    for (uint voxel = 0; voxel < 3; voxel++)
    {
        VxgiWriteEmittance(voxelCoordinates, clipLevel, layeredEmittance.x * areaScale, emissiveColor, normal, pageCoordinatesHash, page);
        voxelCoordinates += offsetDir;
        layeredEmittance.xy = layeredEmittance.yz;
    }
    discard;
}
float4 VxgiNormalizeIrradiance(float4 irradiance)
{
    if (irradiance.a <= 0)
        return float4scalar(0);
    return float4(irradiance.rgb / irradiance.a, 1);
}
float3 VxgiGetIndirectIrradiance(float3 worldPos, float3 normal)
{
    if (bool(g_VxgiVoxelizationCB.UseIrradianceMap)) {
        float3 relativePos = (worldPos - g_VxgiVoxelizationCB.GridCenterPrevious.xyz) * g_VxgiVoxelizationCB.GridCenterPrevious.w;
        float3 validities = saturate(g_VxgiVoxelizationCB.IrradianceMapSize * (float3scalar(1.0) - 2 * abs(relativePos.xyz)));
        float validity = validities.x * validities.y * validities.z;
        if (validity == 0)
            return float3scalar(0.0);
        relativePos += 0.5;
        const float zstep = rcp(6.0);
        relativePos.z *= zstep;
        float4 irradianceX = t_IrradianceMap.SampleLevel(s_IrradianceMapSampler, relativePos + float3(0, 0, zstep * (normal.x > 0 ? EMITTANCE_POSITIVE_X : EMITTANCE_NEGATIVE_X)), 0);
        float4 irradianceY = t_IrradianceMap.SampleLevel(s_IrradianceMapSampler, relativePos + float3(0, 0, zstep * (normal.y > 0 ? EMITTANCE_POSITIVE_Y : EMITTANCE_NEGATIVE_Y)), 0);
        float4 irradianceZ = t_IrradianceMap.SampleLevel(s_IrradianceMapSampler, relativePos + float3(0, 0, zstep * (normal.z > 0 ? EMITTANCE_POSITIVE_Z : EMITTANCE_NEGATIVE_Z)), 0);
        return (VxgiNormalizeIrradiance(irradianceX).rgb * abs(normal.x) + VxgiNormalizeIrradiance(irradianceY).rgb * abs(normal.y) + VxgiNormalizeIrradiance(irradianceZ).rgb * abs(normal.z)) *
               validity;
    }
    return float3scalar(0);
}
void VxgiStoreVoxelizationData(VxgiVoxelizationPSInputData inputData, float3 emissiveColor)
{
    VoxelizeEmittancePS(inputData, emissiveColor);
}
#ifdef TEST_COMPILE
void main(float4 position : SV_Position, VxgiVoxelizationPSInputData vxgiData)
{
    VxgiStoreVoxelizationData(vxgiData, float3(1, 1, 1));
}
#endif
