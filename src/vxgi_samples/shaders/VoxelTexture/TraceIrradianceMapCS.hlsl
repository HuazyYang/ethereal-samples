#include "IrradianceMapCB.hlsli"
#include "../Common/AbstractConeTracing.hlsli"

Buffer<uint> t_PagesToProcess : register(t0);
RWTexture3D<float4> u_IrradianceMap : register(u3);
RWBuffer<uint> u_IrradianceNormalization : register(u4);
static const float RSQ3 = 0.57735027;
float3 GetIrradianceFromDirection(float3 voxelCenter, float3 directionVector, float scaleMultiplier)
{
    VxgiConeTracingArguments args = VxgiDefaultConeTracingArguments();
    args.firstSamplePosition = voxelCenter + directionVector * g_IrradianceVoxelSize.xyz * 0.5;
    args.direction = directionVector;
    args.firstSampleT = g_IrradianceVoxelSize.w;
    args.maxSamples = g_TracingMaxSamples;
    args.tracingStep = g_TracingStep;
    args.opacityCorrectionFactor = g_TracingOpacityCorrectionFactor;
    args.emittanceScale = g_TracingIrradianceScale * scaleMultiplier;
    args.flipOpacityDirections = bool(g_TracingFlipOpacityDirections);
    args.coneFactor = g_TracingConeFactor;
    VxgiConeTracingResults cone = VxgiTraceCone(args);
    return cone.irradiance;
}
void TraceIrradiancePagesBody(PageCoordinates pageCoords, uint3 offset)
{
    int3 voxelPosition = int3(pageCoords.coords.xyz << g_LogPageSize) + int3(offset);
    float3 voxelCenter = (float3(voxelPosition.xyz) + 0.5) * g_IrradianceVoxelSize.xyz + g_ClipmapOrigin.xyz;
    int3 clipmapAddress = TOROIDAL_ADDRESS(voxelPosition.xyz >> g_ToroidalOffsetForIrradiance.w, g_ToroidalOffsetForIrradiance.xyz, g_SourceLevelSize.xyz);
    clipmapAddress.z += g_SourceLevelSize.w;
    float3 opacityPos = t_OpacityMap_Pos[clipmapAddress].xyz;
    float3 opacityNeg = bool(g_VxgiAbstractTracingCB.Use6DOpacity) ? t_OpacityMap_Neg[clipmapAddress].xyz : opacityPos;
    float alpha = 0;
    float3 results[6];
    uint direction;
    [unroll]
    for (direction = 0; direction < 6; direction++) {
        results[direction] = float3scalar(0);
    }
    if (any(opacityPos + opacityNeg > float3scalar(0))) {
        alpha = 1;
        float normalizationFactor = 1.0;
        if (bool(g_UseIrradianceNormalization)) {
            normalizationFactor = asfloat(u_IrradianceNormalization[STRUCT_IRRADIANCE_MULTIPLIER]);
            if (normalizationFactor <= 0)
                normalizationFactor = 1;
        }
        [unroll]
        for (direction = 0; direction < 6; direction++) {
            float3 directionVector;
            float opacity;
            switch (direction) {
            case EMITTANCE_POSITIVE_X:
                directionVector = float3(1, 0, 0);
                opacity = opacityPos.x;
                break;
            case EMITTANCE_NEGATIVE_X:
                directionVector = float3(-1, 0, 0);
                opacity = opacityNeg.x;
                break;
            case EMITTANCE_POSITIVE_Y:
                directionVector = float3(0, 1, 0);
                opacity = opacityPos.y;
                break;
            case EMITTANCE_NEGATIVE_Y:
                directionVector = float3(0, -1, 0);
                opacity = opacityNeg.y;
                break;
            case EMITTANCE_POSITIVE_Z:
                directionVector = float3(0, 0, 1);
                opacity = opacityPos.z;
                break;
            case EMITTANCE_NEGATIVE_Z:
                directionVector = float3(0, 0, -1);
                opacity = opacityNeg.z;
                break;
            }
            if (opacity == 0)
                continue;
            results[direction] = GetIrradianceFromDirection(voxelCenter, directionVector, normalizationFactor);
        }
        for (int n = 0; n < 8; n++) {
            float3 directionVector = float3(
                (n & 1) != 0 ? RSQ3 : -RSQ3,
                (n & 2) != 0 ? RSQ3 : -RSQ3,
                (n & 4) != 0 ? RSQ3 : -RSQ3);
            if (bool(g_VxgiAbstractTracingCB.Use6DOpacity) && VxgiProjectDirectionalOpacities(opacityPos, directionVector) + VxgiProjectDirectionalOpacities(opacityNeg, directionVector) == 0)
                continue;
            float3 irradiance = GetIrradianceFromDirection(voxelCenter, directionVector, normalizationFactor);
            [unroll]
            for (direction = 0; direction < 6; direction++) {
                float projection;
                switch (direction) {
                case EMITTANCE_POSITIVE_X:
                    projection = saturate(directionVector.x);
                    break;
                case EMITTANCE_NEGATIVE_X:
                    projection = saturate(-directionVector.x);
                    break;
                case EMITTANCE_POSITIVE_Y:
                    projection = saturate(directionVector.y);
                    break;
                case EMITTANCE_NEGATIVE_Y:
                    projection = saturate(-directionVector.y);
                    break;
                case EMITTANCE_POSITIVE_Z:
                    projection = saturate(directionVector.z);
                    break;
                case EMITTANCE_NEGATIVE_Z:
                    projection = saturate(-directionVector.z);
                    break;
                }
                results[direction] += projection * irradiance;
            }
        }
    }
    float total = 0;
    [unroll]
    for (direction = 0; direction < 6; direction++) {
        float3 result = results[direction].rgb;
        result = max(float3scalar(0), min(float3scalar(g_IrradianceClampValue), result));
        u_IrradianceMap[voxelPosition.xyz + int3(0, 0, g_IrradianceMapSize * direction)] = float4(result, alpha);
        total += result.r + result.g + result.b;
    }
    if (bool(g_UseIrradianceNormalization)) {
        uint sumAddress = (voxelPosition.z * g_IrradianceMapSize + voxelPosition.y) * g_IrradianceMapSize + voxelPosition.x;
        sumAddress &= (STRUCT_NUM_PARTIAL_SUMS - 1);
        uint previousValue;
        InterlockedAdd(u_IrradianceNormalization[STRUCT_PARTIAL_SUMS + sumAddress], uint(total * INJECTION_FIXED_POINT_SCALE), previousValue);
        InterlockedAdd(u_IrradianceNormalization[STRUCT_VOXEL_COUNT], 1, previousValue);
    }
}
PAGE_PROCESSING_CS_GROUP(TraceIrradiancePagesBody, t_PagesToProcess, g_ListParams)
