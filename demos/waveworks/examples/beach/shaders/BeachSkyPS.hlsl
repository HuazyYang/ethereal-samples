#ifndef WW_REGISTER
#ifdef WAVEWORKS_SPACE
#define WW_REGISTER(reg) register(reg, WAVEWORKS_SPACE)
#else
#define WW_REGISTER(reg) register(reg)
#endif
#endif
#include "BeachEnvironment.hlsli"

struct VSOutput
{
    float4 clipPosition : SV_Position;
    float2 uv : TEXCOORD0;
    float3 eyeToPixel : TEXCOORD1;
};

cbuffer SkyConstants : WW_REGISTER(b0)
{
    float3 g_sunDirection; float g_sunIntensity;
    float g_skyRotation; float g_rayleighScattering;
    float g_mieScattering; float g_useDynamicSkyTexture;
};

Texture2D g_dynamicSky : WW_REGISTER(t0);
SamplerState g_linearSampler : WW_REGISTER(s0);

float4 main(VSOutput input) : SV_Target
{
    float3 viewDirection = normalize(input.eyeToPixel);
    float3 sky = g_dynamicSky.SampleLevel(g_linearSampler,
        BeachEnvironmentUv(viewDirection), 0).rgb;
    return float4(sky * g_sunIntensity, 1.0);
}
