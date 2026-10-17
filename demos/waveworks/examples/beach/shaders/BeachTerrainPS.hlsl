#ifndef WW_REGISTER
#ifdef WAVEWORKS_SPACE
#define WW_REGISTER(reg) register(reg, WAVEWORKS_SPACE)
#else
#define WW_REGISTER(reg) register(reg)
#endif
#endif
#include "BeachLighting.hlsli"
#include "BeachShallowWater.hlsli"
#include "BeachTextureSampling.hlsli"

cbuffer BeachFrameConstants : WW_REGISTER(b0)
{
    float4x4 g_viewProjection;
    float3 g_sunDirection; float g_sunIntensity;
    float3 g_eyePosition; float g_haze;
    float3 g_sunRadiance; float g_time;
    float3 g_skyIrradiance; float g_meanSeaLevel;
}

cbuffer BeachTerrainMaterialConstants : WW_REGISTER(b1)
{
    float4 g_layerUvScale;
    float4 g_layerRoughness;
    float4 g_layerNormalStrength;
    float4 g_layerSpecular;
    float4 g_layerAo;
    float4 g_layerAlbedoScale;
    float4 g_sandReflectanceScale;
    float4 g_waterCoat;
    float4 g_vegetationReflectanceScale;
    float4 g_groundVisibilityBounds;
}

Texture2D g_cliffAlbedo : WW_REGISTER(t0);
Texture2D g_sandAlbedo : WW_REGISTER(t1);
Texture2D g_sandNormal : WW_REGISTER(t2);
Texture2D g_vegetationAlbedo : WW_REGISTER(t3);
Texture2D g_vegetationNormal : WW_REGISTER(t4);
Texture2D g_groundNormal : WW_REGISTER(t5);
Texture2D g_groundArm : WW_REGISTER(t6);
Texture2D g_sandArm : WW_REGISTER(t7);
Texture2D g_vegetationArm : WW_REGISTER(t8);
Texture2D<float> g_groundVisibility : WW_REGISTER(t9);
Texture2D<float> g_groundLuminance : WW_REGISTER(t18);
SamplerState g_materialSampler : WW_REGISTER(s0);

struct PSInput
{
    float4 position : SV_Position;
    float3 worldPosition : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float distanceToEye : TEXCOORD2;
    float2 uv : TEXCOORD3;
    float4 materialWeights : TEXCOORD4;
};

struct TerrainOutput
{
    float4 color : SV_Target0;
    float depth : SV_Target1;
};

float3 ApplyBeachCotangentFrame(float3 geometricNormal, float3 worldPosition,
    float2 materialUv, float3 tangentNormal)
{
    float3 positionDx = ddx(worldPosition);
    float3 positionDy = ddy(worldPosition);
    float2 uvDx = ddx(materialUv);
    float2 uvDy = ddy(materialUv);
    float3 perpendicularX = cross(positionDy, geometricNormal);
    float3 perpendicularY = cross(geometricNormal, positionDx);
    float3 tangent = perpendicularX * uvDx.x + perpendicularY * uvDy.x;
    float3 bitangent = perpendicularX * uvDx.y + perpendicularY * uvDy.y;
    float inverseScale = rsqrt(max(max(dot(tangent, tangent),
        dot(bitangent, bitangent)), 1e-8));
    return normalize(tangent * (tangentNormal.x * inverseScale) +
        bitangent * (tangentNormal.y * inverseScale) +
        geometricNormal * tangentNormal.z);
}

float3 NormalizeMaterialWeights(float3 weights)
{
    weights = max(weights, 0.0);
    float weightSum = weights.x + weights.y + weights.z;
    return weightSum > 1e-5 ? weights / weightSum : float3(0.0, 0.0, 1.0);
}

TerrainOutput main(PSInput input)
{
    float3 geometricNormal = normalize(input.normal);
    float3 materialWeights = NormalizeMaterialWeights(input.materialWeights.rgb);
    float2 worldUv = input.worldPosition.xz;

    // Albedo resources are bound through sRGB views. Filtering and decoding
    // therefore happen in the correct order before material-layer composition.
    bool stochastic = g_layerAlbedoScale.w > 0.5;
    BeachTexturePatches rockPatches = BeachMakeTexturePatches(input.uv * g_layerUvScale.z, stochastic);
    BeachTexturePatches sandPatches = BeachMakeTexturePatches(worldUv * g_layerUvScale.x, stochastic);
    BeachTexturePatches grassPatches = BeachMakeTexturePatches(worldUv * g_layerUvScale.y, stochastic);
    BeachTexturePatches grainPatches = BeachMakeTexturePatches(worldUv * g_layerUvScale.w, stochastic);
    float3 rockSample = stochastic ? BeachSampleContrastColor(g_cliffAlbedo,
        g_groundLuminance, g_materialSampler, g_environmentSampler, rockPatches,
        input.uv * g_layerUvScale.z) : BeachSampleMaterialColor(g_cliffAlbedo, g_materialSampler, rockPatches);
    float3 rockBaseColor = saturate(rockSample * g_layerAlbedoScale.z);
    float3 sandBaseColor = saturate(BeachSampleMaterialColor(g_sandAlbedo,
        g_materialSampler, sandPatches) * g_sandReflectanceScale.rgb * g_layerAlbedoScale.x);
    float3 vegetationBaseColor = saturate(BeachSampleMaterialColor(g_vegetationAlbedo,
        g_materialSampler, grassPatches) * g_layerAlbedoScale.y * g_vegetationReflectanceScale.rgb);
    float3 baseColor = sandBaseColor * materialWeights.x +
        vegetationBaseColor * materialWeights.y + rockBaseColor * materialWeights.z;

    float sandVariance, grainVariance, vegetationVariance, rockVariance = 0;
    float3 sandNormal = BeachSampleMaterialNormal(g_sandNormal,
        g_materialSampler, sandPatches, g_layerNormalStrength.x, sandVariance);
    float3 grainNormal = BeachSampleMaterialNormal(g_sandNormal,
        g_materialSampler, grainPatches, g_layerNormalStrength.w, grainVariance);
    // Compose independent macro/micro height slopes, not shaded colors.
    sandNormal = normalize(float3(sandNormal.xy / sandNormal.z +
        grainNormal.xy / grainNormal.z, 1));
    float3 vegetationNormal = BeachSampleMaterialNormal(g_vegetationNormal,
        g_materialSampler, grassPatches, g_layerNormalStrength.y, vegetationVariance);
    float3 rockNormal = float3(0,0,1);
    if (stochastic) rockNormal = BeachSampleMaterialNormal(g_groundNormal,
        g_materialSampler, rockPatches, g_layerNormalStrength.z, rockVariance);
    float3 tangentNormal = normalize(sandNormal * materialWeights.x +
        vegetationNormal * materialWeights.y + rockNormal * materialWeights.z);
    float3 N = ApplyBeachCotangentFrame(geometricNormal, input.worldPosition,
        worldUv, tangentNormal);

    float roughness = dot(materialWeights, g_layerRoughness.xyz);
    float ambientOcclusion = saturate(dot(materialWeights, g_layerAo.xyz));
    if (stochastic)
    {
        float3 groundArm = BeachSampleMaterialColor(g_groundArm,g_materialSampler,rockPatches);
        float3 sandArm = BeachSampleMaterialColor(g_sandArm,g_materialSampler,sandPatches);
        float3 grassArm = BeachSampleMaterialColor(g_vegetationArm,g_materialSampler,grassPatches);
        roughness = max(.08, sandArm.g*materialWeights.x + grassArm.g*materialWeights.y + groundArm.g*materialWeights.z);
        ambientOcclusion = sandArm.r*materialWeights.x + grassArm.r*materialWeights.y + groundArm.r*materialWeights.z;
    }
    float2 visibilityUv = (worldUv - g_groundVisibilityBounds.xy) / g_groundVisibilityBounds.zw;
    float skyVisibility = 1;
    if (all(visibilityUv >= 0) && all(visibilityUv <= 1))
        skyVisibility = g_groundVisibility.SampleLevel(g_environmentSampler, visibilityUv, 0);
    ambientOcclusion *= skyVisibility;
    float mipVariance = materialWeights.x * (sandVariance + grainVariance) +
        materialWeights.y * vegetationVariance + materialWeights.z * rockVariance;
    roughness = sqrt(sqrt(saturate(pow(roughness, 4.0) + mipVariance)));
    roughness = BeachApplySpecularAA(N, saturate(roughness));
    float specular = saturate(dot(materialWeights, g_layerSpecular.xyz));
    float3 V = normalize(g_eyePosition - input.worldPosition);
    float3 L = normalize(g_sunDirection);
    float3 incidentRadiance = max(g_sunRadiance, 0.0) * max(g_sunIntensity, 0.0);
    incidentRadiance *= BeachSunVisibility(input.worldPosition, geometricNormal);
    float3 skyIrradiance = BeachDiffuseIrradiance(N.xzy) * g_sunIntensity;
    // The opaque scene is also the underwater bottom. Attenuate its incoming
    // solar path before the water pass attenuates the outgoing camera path.
    float submergedDepth = max(g_meanSeaLevel - input.worldPosition.y, 0.0);
    float waterSunCos = sqrt(1.0 - (1.0 - L.y * L.y) / (g_waterExtinction.w * g_waterExtinction.w));
    float3 bottomTransmission = exp(-g_waterExtinction.rgb *
        submergedDepth / max(waterSunCos, 0.01));
    incidentRadiance *= bottomTransmission;
    skyIrradiance *= exp(-g_waterExtinction.rgb * submergedDepth * 2.0);

    float3 substrateDirect = BeachEvaluateDefaultLitDirect(baseColor, 0.0,
        specular, roughness, N, V, L, incidentRadiance);
    float3 substrateAmbient = BeachEvaluateLambertAmbient(baseColor, 0.0,
        specular, ambientOcclusion, skyIrradiance);
    // A submerged substrate has no additional air/water environment interface.
    if (submergedDepth == 0)
        substrateAmbient += BeachSpecularEnvironment(N.xzy, V.xzy, roughness,
            BeachDielectricF0(specular)) * g_sunIntensity * ambientOcclusion;

    // Vertex alpha is an authored fractional area coverage for a thin water
    // film. The interface reflects as a dielectric and attenuates both the
    // incoming and outgoing paths before the substrate contribution.
    // Submerged sand has no separate air/water film interface. Its overlying
    // water interface and volume are evaluated by BeachWaterPS.
    float coatCoverage = input.worldPosition.y >= g_meanSeaLevel
        ? saturate(input.materialWeights.a) : 0.0;
    float sweWeight = BeachShallowWeight(input.worldPosition.xz);
    float4 swe = g_sweSurface.SampleLevel(g_environmentSampler,
        BeachShallowUv(input.worldPosition.xz), 0);
    // Run-up leaves an actual time-dependent wet film. Its reflectance still
    // comes from the dielectric coat BRDF; there is no wet-sand color palette.
    float exposedWetSand = swe.y < 0.008 ? swe.w * materialWeights.x : 0.0;
    coatCoverage = lerp(coatCoverage, exposedWetSand, sweWeight);
    float coatIor = max(g_waterCoat.x, 1.0001);
    float coatF0Scalar = (coatIor - 1.0) / (coatIor + 1.0);
    coatF0Scalar *= coatF0Scalar;
    float3 coatF0 = coatF0Scalar.xxx;
    float coatRoughness = BeachApplySpecularAA(N,
        sqrt(sqrt(saturate(pow(g_waterCoat.y, 4.0) + mipVariance))));
    float coatNoL = saturate(dot(N, L));
    float coatNoV = saturate(dot(N, V));
    float3 coatIncomingFresnel = BeachFresnelSchlick(coatNoL, coatF0);
    float3 coatOutgoingFresnel = BeachFresnelSchlick(coatNoV, coatF0);
    float3 coatPathTransmission = (1.0 - coatIncomingFresnel) *
        (1.0 - coatOutgoingFresnel) * exp(-max(g_waterCoat.z, 0.0) *
            (rcp(max(coatNoL, 0.01)) + rcp(max(coatNoV, 0.01))));
    float3 directAreaTransmission = (1.0 - coatCoverage).xxx +
        coatCoverage * coatPathTransmission;
    float3 coatDirect = BeachSpecularBRDF(N, V, L, coatF0,
        coatRoughness) * incidentRadiance * coatNoL * coatCoverage;

    float3 ambientPathTransmission = (1.0 - coatOutgoingFresnel) *
        (1.0 - coatOutgoingFresnel) * exp(-max(g_waterCoat.z, 0.0) *
            (2.0 + rcp(max(coatNoV, 0.01))));
    float3 ambientAreaTransmission = (1.0 - coatCoverage).xxx +
        coatCoverage * ambientPathTransmission;
    float3 color = substrateDirect * directAreaTransmission + coatDirect +
        substrateAmbient * ambientAreaTransmission;
    color += coatCoverage * BeachSpecularEnvironment(N.xzy, V.xzy,
        coatRoughness, coatF0) * g_sunIntensity;

    // The submerged bed is refracted by the water pass, which applies the
    // camera-to-interface air segment once. Do not fog its underwater path.
    float radialDepth = distance(g_eyePosition, input.worldPosition);
    if (submergedDepth == 0)
        color = BeachAerialPerspective(color, input.worldPosition.xzy,
            g_eyePosition.xzy, g_sunIntensity);

    TerrainOutput output;
    // Explicit inspection views only; these never contribute to lit shading.
    if (g_waterCoat.w == 1.0) color = baseColor;
    if (g_waterCoat.w == 2.0) color = N * 0.5 + 0.5;
    if (g_waterCoat.w == 3.0) color = materialWeights;
    if (g_waterCoat.w == 4.0) color = skyVisibility.xxx;
    output.color = float4(max(color, 0.0), 1.0);
    output.depth = radialDepth;
    return output;
}
