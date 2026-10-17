#ifndef WW_REGISTER
#ifdef WAVEWORKS_SPACE
#define WW_REGISTER(reg) register(reg, WAVEWORKS_SPACE)
#else
#define WW_REGISTER(reg) register(reg)
#endif
#endif
#include "BeachLighting.hlsli"
#include "BeachShallowWater.hlsli"

struct DSOutput
{
    float4 clipPosition : SV_Position;
    centroid float4 uvCascade01 : TEXCOORD0;
    centroid float4 uvCascade23 : TEXCOORD1;
    centroid float2 uvLocal : TEXCOORD2;
    centroid float4 cascadeBlend : TEXCOORD3;
    float3 worldDisplaced : TEXCOORD4;
    float3 worldUndisplaced : TEXCOORD5;
    float3 shoreData : TEXCOORD6;
};
// Keep this prefix compatible with OCEAN_PS_CBUFFER in ocean_renderer.cpp.
cbuffer WaterConstants : WW_REGISTER(b0)
{
    float g_cascadeScale; float g_windTextureSize; float g_uvWarpAmplitude; float g_uvWarpFrequency;
    float g_foamThreshold; float g_localTextureSize; float g_localFoamThreshold; float g_localDomainSize;
    float2 g_localDomainCenter; float g_roughness; float g_showCascades;
    float3 g_sunDirection; float g_sunIntensity;
    float3 g_eyePosition; float g_useFresnel;
    float g_useSpecular; float g_useReflection; float g_oceanPadding0; float g_oceanPadding1;
    float g_sceneIsBeach; float g_shallowWater; float g_shoreFoam; float g_foamIntensity;
    float2 g_inverseViewport; float g_nearClip; float g_farClip;
    float3 g_absorption; float g_waterIOR;
    float3 g_scattering; float g_maxOpticalDepth;
    float3 g_sedimentAbsorption; float g_sedimentDepth;
    float3 g_sedimentScattering; float g_sedimentConcentration;
    float3 g_sunRadiance; float g_refractionDistanceLimit;
    float3 g_foamBaseColor; float g_foamRoughness;
    float g_breakingWaveRatio; float g_foamOpticalDepth; float g_foamEnergyScale; float g_foamPatternScale;
    float4x4 g_worldToClip;
};
Texture2DArray g_gradients : WW_REGISTER(t0);
Texture2DArray g_moments : WW_REGISTER(t1);
Texture2D g_localDisplacement : WW_REGISTER(t2);
Texture2D g_localGradients : WW_REGISTER(t3);
Texture2D g_foamPattern : WW_REGISTER(t4);
Texture2D g_bubbles : WW_REGISTER(t5);
Texture2D g_windGusts : WW_REGISTER(t6);
Texture2D g_dynamicSky : WW_REGISTER(t7);
Texture2D g_opaqueScene : WW_REGISTER(t8);
Texture2D g_linearDepth : WW_REGISTER(t9);
SamplerState g_linear : WW_REGISTER(s0);
SamplerState g_anisotropic : WW_REGISTER(s1);
SamplerState g_linearClamp : WW_REGISTER(s2);

float3 BeachSkyRadiance(float3 direction)
{
    return g_dynamicSky.SampleLevel(g_environmentSampler,
        BeachEnvironmentUv(direction), 0).rgb * g_sunIntensity;
}
float3 BeachSkyIrradiance()
{
    return BeachDiffuseIrradiance(float3(0, 0, 1)) * g_sunIntensity;
}

float3 BeachRoughSkyReflection(float3 N, float3 V, float roughness, float f0)
{
    return BeachSpecularEnvironment(N, V, roughness, f0.xxx,
        g_useFresnel > 0.5 ? 1.0 : f0) * g_sunIntensity;
}
struct BeachWaveSurface
{
    float3 normal;
    float variance;
    float foamEnergy;
};
BeachWaveSurface BeachReadWaveSurface(DSOutput input)
{
    float sweWeight = BeachShallowWeight(input.worldUndisplaced.xy);
    float4 swe = g_sweSurface.SampleLevel(g_linearClamp,
        BeachShallowUv(input.worldUndisplaced.xy), 0);
    // True wet/dry coverage, shared by transmissive water and foam. Dry cells
    // must not leave a zero-depth mirror floating over exposed terrain.
    if (sweWeight > 0.999) clip(swe.y - 0.003);
    float4 gradients[4];
    gradients[0] = g_gradients.Sample(g_anisotropic, float3(input.uvCascade01.xy, 0));
    gradients[1] = g_gradients.Sample(g_anisotropic, float3(input.uvCascade01.zw, 1));
    gradients[2] = g_gradients.Sample(g_anisotropic, float3(input.uvCascade23.xy, 2));
    gradients[3] = g_gradients.Sample(g_anisotropic, float3(input.uvCascade23.zw, 3));
    float2 moments[4];
    moments[0] = g_moments.Sample(g_anisotropic, float3(input.uvCascade01.xy, 0)).xy;
    moments[1] = g_moments.Sample(g_anisotropic, float3(input.uvCascade01.zw, 1)).xy;
    moments[2] = g_moments.Sample(g_anisotropic, float3(input.uvCascade23.xy, 2)).xy;
    moments[3] = g_moments.Sample(g_anisotropic, float3(input.uvCascade23.zw, 3)).xy;
    BeachWaveSurface surface;
    surface.variance = 0;
    surface.foamEnergy = 0;
    float2 slope = 0;
    // UV derivatives recover the cascade world scale without another CB ABI.
    float2 uv[4] = {input.uvCascade01.xy, input.uvCascade01.zw,
                    input.uvCascade23.xy, input.uvCascade23.zw};
    float worldDerivative = max(length(ddx(input.worldUndisplaced.xy)), 1e-6);
    [unroll] for (int i = 0; i < 4; ++i)
    {
        float weight = input.cascadeBlend[i];
        float scale = length(ddx(uv[i])) / worldDerivative;
        float2 resolved = g_gradients.SampleLevel(g_linear, float3(uv[i], i),
            BeachShallowLowPassLod(scale, g_windTextureSize)).xy;
        slope += (gradients[i].xy - sweWeight * resolved) * weight;
        // Var(wX) = w*w Var(X), not w Var(X).
        surface.variance += max(0.0, moments[i].x + moments[i].y -
            dot(gradients[i].xy, gradients[i].xy)) * weight * weight;
        surface.foamEnergy += max(gradients[i].w, 0.0) * weight * (1.0 - sweWeight);
    }
    float2 localDistance = abs(input.uvLocal - 0.5) * 2;
    float localWeight = smoothstep(0.0, 0.10, 1.0 - max(localDistance.x, localDistance.y));
    float4 localGradient = g_localGradients.Sample(g_linearClamp, input.uvLocal);
    slope += localGradient.xy * localWeight * (1.0 - sweWeight);
    surface.foamEnergy += max(localGradient.w, 0.0) * localWeight * (1.0 - sweWeight);
    float2 sweSlope = g_sweFlow.SampleLevel(g_linearClamp,
        BeachShallowUv(input.worldUndisplaced.xy), 0).xy;
    surface.foamEnergy += swe.z * sweWeight;
    // WaveWorks stores negative height derivatives. Do not negate them again.
    surface.normal = normalize(float3(slope * input.shoreData.y + sweSlope * sweWeight, 1));
    // The depth limiter acts on the resolved wave, not the capillary material
    // lobe: its roughness must not collapse to a perfect mirror at the shoreline.
    surface.variance = max(g_roughness, 0.0) +
        surface.variance * input.shoreData.y * input.shoreData.y;
    return surface;
}
