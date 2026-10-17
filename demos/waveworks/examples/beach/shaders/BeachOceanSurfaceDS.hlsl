#ifndef WW_REGISTER
#ifdef WAVEWORKS_SPACE
#define WW_REGISTER(reg) register(reg, WAVEWORKS_SPACE)
#else
#define WW_REGISTER(reg) register(reg)
#endif
#endif
#define BEACH_SHALLOW_DOMAIN
#include "BeachShallowWater.hlsli"

struct HS_CONSTANTOUTPUT
{
    float edgeTessFactors[3] : SV_TessFactor;
    float insideTessFactor : SV_InsideTessFactor;
};

struct HS_OUTPUT
{
    float4 worldspacePosition : HSO;
};

struct DS_OUTPUT
{
    float4 clipSpacePosition : SV_Position;
    centroid float4 UVForCascade01 : TEXCOORD0;
    centroid float4 UVForCascade23 : TEXCOORD1;
    centroid float2 UVForLocalWaves : TEXCOORD2;
    centroid float4 cascadeBlendingFactors : TEXCOORD3;
    float3 worldspacePositionDisplaced : TEXCOORD4;
    float3 worldspacePositionUndisplaced : TEXCOORD5;
    float3 shoreData : TEXCOORD6; // still-water depth, displacement scale, raw wave amplitude
};

cbuffer OCEAN_VS_HS_DS_CBUFFER : WW_REGISTER(b0)
{
    float4x4 g_matViewProj;
    float3 g_eyePos;
    float g_meanOceanLevel;
    float g_useDiamondPattern;
    float g_dynamicTesselationAmount;
    float g_staticTesselationOffset;
    float g_cascade0UVScale;
    float g_cascade1UVScale;
    float g_cascade2UVScale;
    float g_cascade3UVScale;
    float g_cascade0UVOffset;
    float g_cascade1UVOffset;
    float g_cascade2UVOffset;
    float g_cascade3UVOffset;
    float g_UVWarpingAmplitude;
    float g_UVWarpingFrequency;
    float g_localWavesSimulationDomainWorldspaceSize;
    float2 g_localWavesSimulationDomainWorldspaceCenter;
    float g_beachScene;
    float3 g_beachPadding;
    float2 g_bathymetryOrigin;
    float2 g_bathymetryInverseExtent;
    float g_breakingWaveRatio;
    float g_shorelineFadeDepth;
    float2 g_bathymetryPadding;
};

Texture2DArray g_displacementTextureArrayWindWaves : WW_REGISTER(t0);
Texture2D g_displacementTextureLocalWaves : WW_REGISTER(t1);
Texture2D<float> g_bathymetry : WW_REGISTER(t2);
SamplerState g_bilinearSampler : WW_REGISTER(s0);
SamplerState g_bilinearSamplerClamp : WW_REGISTER(s1);

DS_OUTPUT GetBeachDisplacedVertex(float4 input0, float4 input1, float4 input2,
    float3 barycentricCoordinates)
{
    precise float3 undisplaced = input0.xyz * barycentricCoordinates.x +
        input1.xyz * barycentricCoordinates.y + input2.xyz * barycentricCoordinates.z;
    float distanceToEye = length(g_eyePos - undisplaced);
    float4 cascadeWorldSizes = float4(1.0 / g_cascade0UVScale,
        1.0 / g_cascade1UVScale, 1.0 / g_cascade2UVScale, 1.0 / g_cascade3UVScale);
    float4 cascadeBlend = float4(1.0,
        saturate(0.033 * (cascadeWorldSizes.yzw * 30.0 - distanceToEye) /
            cascadeWorldSizes.yzw));

    // These UVs address authored WaveWorks simulation fields directly. The
    // local field is clamped at its border, so fade it before sampling to keep
    // the edge of the finite cove domain from producing a visible hard seam.
    float2 uv0 = undisplaced.xy * g_cascade0UVScale + g_cascade0UVOffset.xx;
    float2 uv1 = undisplaced.xy * g_cascade1UVScale + g_cascade1UVOffset.xx;
    float2 uv2 = undisplaced.xy * g_cascade2UVScale + g_cascade2UVOffset.xx;
    float2 uv3 = undisplaced.xy * g_cascade3UVScale + g_cascade3UVOffset.xx;
    float2 localUv = float2(0.5, 0.5) +
        (undisplaced.xy - g_localWavesSimulationDomainWorldspaceCenter) /
            g_localWavesSimulationDomainWorldspaceSize;
    float2 localDistance = abs(localUv - 0.5) * 2.0;
    float localInterior = 1.0 - max(localDistance.x, localDistance.y);
    float localWeight = smoothstep(0.0, 0.10, localInterior);

    float3 displacement = cascadeBlend.x *
        g_displacementTextureArrayWindWaves.SampleLevel(g_bilinearSampler,
            float3(uv0, 0.0), 0.0).xyz;
    displacement += cascadeBlend.y == 0.0 ? float3(0.0, 0.0, 0.0) :
        cascadeBlend.y * g_displacementTextureArrayWindWaves.SampleLevel(
            g_bilinearSampler, float3(uv1, 1.0), 0.0).xyz;
    displacement += cascadeBlend.z == 0.0 ? float3(0.0, 0.0, 0.0) :
        cascadeBlend.z * g_displacementTextureArrayWindWaves.SampleLevel(
            g_bilinearSampler, float3(uv2, 2.0), 0.0).xyz;
    displacement += cascadeBlend.w == 0.0 ? float3(0.0, 0.0, 0.0) :
        cascadeBlend.w * g_displacementTextureArrayWindWaves.SampleLevel(
            g_bilinearSampler, float3(uv3, 3.0), 0.0).xyz;
    displacement += g_beachScene > 0.5
        ? g_localWavesSimulationDomainWorldspaceSize > 0.0
            ? g_displacementTextureLocalWaves.SampleLevel(
                g_bilinearSamplerClamp, localUv, 0.0).xyz * localWeight
            : float3(0.0, 0.0, 0.0)
        : g_displacementTextureLocalWaves.SampleLevel(
            g_bilinearSamplerClamp, localUv, 0.0).xyz;

    float sweWeight = BeachShallowWeight(undisplaced.xy);
    float4 swe = g_sweSurface.SampleLevel(g_bilinearSamplerClamp,
        BeachShallowUv(undisplaced.xy), 0);
    uint waveWidth, waveHeight, waveLayers;
    g_displacementTextureArrayWindWaves.GetDimensions(waveWidth, waveHeight, waveLayers);
    float4 scales = 1.0 / cascadeWorldSizes;
    float2 waveUv[4] = {uv0, uv1, uv2, uv3};
    float3 resolvedFft = 0;
    [unroll] for (uint cascade = 0; cascade < 4; ++cascade)
        resolvedFft += cascadeBlend[cascade] *
            g_displacementTextureArrayWindWaves.SampleLevel(g_bilinearSampler,
                float3(waveUv[cascade], cascade),
                BeachShallowLowPassLod(scales[cascade], float(waveWidth))).xyz;
    // The legacy local wavemaker is suspended while the PDE drives the cove.
    // Remove its retained displacement here as well, including after toggling.
    resolvedFft += g_displacementTextureLocalWaves.SampleLevel(
        g_bilinearSamplerClamp, localUv, 0).xyz * localWeight;
    displacement -= sweWeight * resolvedFft;

    DS_OUTPUT output;
    uint bathymetryWidth, bathymetryHeight;
    g_bathymetry.GetDimensions(bathymetryWidth, bathymetryHeight);
    float2 bathymetryUv = (undisplaced.xy - g_bathymetryOrigin) * g_bathymetryInverseExtent;
    float2 dimensions = float2(bathymetryWidth, bathymetryHeight);
    float bed = g_bathymetry.SampleLevel(g_bilinearSamplerClamp,
        (saturate(bathymetryUv) * (dimensions - 1.0) + 0.5) / dimensions, 0.0);
    bool onSurvey = all(bathymetryUv >= 0.0) && all(bathymetryUv <= 1.0);
    float depth = onSurvey ? max(g_meanOceanLevel - bed, 0.0) : 10000.0;
    depth = lerp(depth, swe.y, sweWeight);
    float amplitude = abs(displacement.z);
    // Limit only the unresolved FFT detail, NOT the PDE's free-surface height.
    // H <= gamma*h prevents those short waves from penetrating a drying bed.
    float displacementScale = g_breakingWaveRatio > 0.0
        ? min(1.0, 0.5 * g_breakingWaveRatio * depth / max(amplitude, 1e-5)) : 1.0;
    displacement *= displacementScale;
    displacement.z += sweWeight * (swe.x - g_meanOceanLevel);
    output.shoreData = float3(depth, displacementScale, amplitude);
    output.UVForCascade01 = float4(uv0, uv1);
    output.UVForCascade23 = float4(uv2, uv3);
    output.UVForLocalWaves = localUv;
    output.cascadeBlendingFactors = cascadeBlend;
    output.worldspacePositionUndisplaced = undisplaced;
    output.worldspacePositionDisplaced = undisplaced + displacement;
    return output;
}

[domain("tri")]
DS_OUTPUT main(HS_CONSTANTOUTPUT constants, const OutputPatch<HS_OUTPUT, 3> patch,
    float3 barycentricCoordinates : SV_DomainLocation)
{
    DS_OUTPUT output = GetBeachDisplacedVertex(patch[0].worldspacePosition,
        patch[1].worldspacePosition, patch[2].worldspacePosition,
        barycentricCoordinates);
    output.clipSpacePosition = mul(g_matViewProj,
        float4(output.worldspacePositionDisplaced, 1.0));
    return output;
}
