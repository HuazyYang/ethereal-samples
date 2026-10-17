#ifndef WW_REGISTER
#ifdef WAVEWORKS_SPACE
#define WW_REGISTER(reg) register(reg, WAVEWORKS_SPACE)
#else
#define WW_REGISTER(reg) register(reg)
#endif
#endif
#ifndef BEACH_LIGHTING_HLSLI
#define BEACH_LIGHTING_HLSLI
#include "BeachEnvironment.hlsli"
#include "BeachAerialPerspective.hlsli"

cbuffer BeachLightingConstants : WW_REGISTER(b2)
{
    float4x4 g_worldToShadow;
    float4 g_shadowParameters;
    float4 g_environmentParameters;
    float4 g_waterExtinction;
    float4x4 g_worldToAerialClip;
    float4 g_aerialHorizon;
};
#ifndef BEACH_SHADOW_VERTEX
Texture2D g_diffuseIrradiance : WW_REGISTER(t10);
Texture2D g_specularEnvironment : WW_REGISTER(t11);
Texture2D g_environmentBrdf : WW_REGISTER(t12);
Texture2D<float> g_directionalShadow : WW_REGISTER(t13);
Texture2D g_aerialRadiance : WW_REGISTER(t16);
Texture2D g_aerialTransmittance : WW_REGISTER(t17);
SamplerState g_environmentSampler : WW_REGISTER(s3);

float3 BeachDiffuseIrradiance(float3 normalZUp)
{
    return g_diffuseIrradiance.SampleLevel(g_environmentSampler,
        BeachEnvironmentUv(normalZUp), 0).rgb;
}

float3 BeachSpecularEnvironment(float3 N, float3 V, float roughness, float3 f0,
    float f90 = 1.0)
{
    float NoV = saturate(dot(N, V));
    // Keep the LUT away from the U wrap seam. Its domain is [NoV, roughness],
    // whereas the environment uses the same sampler's periodic longitude.
    float2 lookupUv = clamp(float2(NoV, roughness), 0.5 / 128.0, 1.0 - 0.5 / 128.0);
    float2 dfg = g_environmentBrdf.SampleLevel(g_environmentSampler, lookupUv, 0).rg;
    float3 radiance = g_specularEnvironment.SampleLevel(g_environmentSampler,
        BeachEnvironmentUv(reflect(-V, N)), roughness * g_environmentParameters.x).rgb;
    return radiance * (f0 * dfg.x + f90 * dfg.y);
}

float3 BeachAerialPerspective(float3 radiance, float3 worldZUp, float3 eyeZUp,
    float sunIntensity)
{
    float4 clip = mul(g_worldToAerialClip, float4(worldZUp.xzy, 1));
    float2 screen = clip.xy / max(clip.w, 1e-6) * float2(.5, -.5) + .5;
    float slice = BeachAerialDistanceSlice(length(worldZUp - eyeZUp), g_environmentParameters.y);
    float low = floor(slice), high = min(low + 1, float(BeachAerialSlices - 1));
    float2 uv0 = BeachAerialAtlasUv(screen, low, g_aerialHorizon.xy);
    float2 uv1 = BeachAerialAtlasUv(screen, high, g_aerialHorizon.xy);
    // Trilinear reconstruction of integrated volume samples, never a fog-color
    // ramp. RGB throughput and inscattering come from the same sky medium.
    float3 scattering = lerp(g_aerialRadiance.SampleLevel(g_environmentSampler, uv0, 0).rgb,
        g_aerialRadiance.SampleLevel(g_environmentSampler, uv1, 0).rgb, frac(slice));
    float3 transmittance = lerp(g_aerialTransmittance.SampleLevel(g_environmentSampler, uv0, 0).rgb,
        g_aerialTransmittance.SampleLevel(g_environmentSampler, uv1, 0).rgb, frac(slice));
    return radiance * transmittance + scattering * sunIntensity;
}

float BeachSunVisibility(float3 worldYUp, float3 geometricNormalYUp)
{
    if (g_shadowParameters.w < 0.5) return 1;
    float4 projected = mul(g_worldToShadow,
        float4(worldYUp + geometricNormalYUp * g_shadowParameters.z, 1));
    if (abs(projected.w) < 1e-5) return 1;
    float3 shadow = projected.xyz / projected.w;
    float2 uv = shadow.xy * float2(0.5, -0.5) + 0.5;
    if (any(uv <= 0) || any(uv >= 1) || shadow.z <= 0 || shadow.z >= 1) return 1;
    uint width, height;
    g_directionalShadow.GetDimensions(width, height);
    float2 pixel = uv * float2(width, height) - 0.5;
    int2 base = int2(floor(pixel));
    float2 f = frac(pixel);
    float depth = shadow.z - g_shadowParameters.y;
    float visibility = 0;
    // Tent-filtered 3x3 PCF, with bilinear comparison at each tap. Filtering
    // comparisons (not depth) prevents leaks across occluder boundaries.
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
    {
        float tap = 0;
        [unroll] for (int j = 0; j <= 1; ++j)
        [unroll] for (int i = 0; i <= 1; ++i)
        {
            int2 p = clamp(base + int2(x + i, y + j), int2(0, 0), int2(width, height) - 1);
            float w = (i == 0 ? 1.0 - f.x : f.x) * (j == 0 ? 1.0 - f.y : f.y);
            tap += (depth <= g_directionalShadow.Load(int3(p, 0)) ? 1.0 : 0.0) * w;
        }
        visibility += tap;
    }
    return visibility / 9.0;
}
#endif
#endif
