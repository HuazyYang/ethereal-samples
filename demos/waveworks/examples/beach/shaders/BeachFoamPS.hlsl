#include "BeachWaterCommon.hlsli"

float4 main(DSOutput input) : SV_Target
{
    if (g_oceanPadding0 > 0) return 0;
    BeachWaveSurface surface = BeachReadWaveSurface(input);
    float3 N = surface.normal;
    float3 V = normalize(g_eyePosition - input.worldDisplaced);
    float3 L = normalize(g_sunDirection);
    // Energy removed by the depth limit is a breaker proxy. Persistent offshore
    // foam comes from the solver's advected field, not a screen-depth color band.
    float lostAmplitude = input.shoreData.z * (1.0 - input.shoreData.y);
    float breakingEnergy = (1.0 - BeachShallowWeight(input.worldUndisplaced.xy)) *
        g_shallowWater * lostAmplitude * lostAmplitude /
        max(input.shoreData.x * input.shoreData.x, 0.01);
    float density = max(surface.foamEnergy * g_foamEnergyScale + breakingEnergy, 0.0);
    float pattern = g_foamPattern.Sample(g_anisotropic,
        input.worldDisplaced.xy * g_foamPatternScale).r;
    float bubbles = g_bubbles.Sample(g_anisotropic,
        input.worldDisplaced.xy * (4.0 * g_foamPatternScale)).r;
    float opticalDepth = density * pattern * bubbles * g_foamOpticalDepth *
        max(g_foamIntensity, 0.0) * g_shoreFoam;
    float alpha = 1.0 - exp(-opticalDepth / max(dot(N, V), 0.05));
    float roughness = BeachApplySpecularAA(N, g_foamRoughness);
    float3 color = BeachEvaluateDefaultLitDirect(g_foamBaseColor, 0, 0.5,
        roughness, N, V, L, g_sunRadiance * g_sunIntensity *
            BeachSunVisibility(input.worldDisplaced.xzy, N.xzy)) +
        BeachEvaluateLambertAmbient(g_foamBaseColor, 0, 0.5, 1,
            BeachDiffuseIrradiance(N) * g_sunIntensity);
    color = BeachAerialPerspective(color, input.worldDisplaced,
        g_eyePosition, g_sunIntensity);
    return float4(max(color, 0.0) * alpha, alpha);
}
