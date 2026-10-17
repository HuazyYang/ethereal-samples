#ifndef WW_REGISTER
#ifdef WAVEWORKS_SPACE
#define WW_REGISTER(reg) register(reg, WAVEWORKS_SPACE)
#else
#define WW_REGISTER(reg) register(reg)
#endif
#endif
#include "BeachEnvironment.hlsli"
cbuffer EnvironmentConstants : WW_REGISTER(b0)
{
    float3 g_atmosphereSun; float g_solarIrradiance;
    float g_filterRoughness; float3 g_environmentPadding;
};
Texture2D g_environment : WW_REGISTER(t0);
SamplerState g_environmentSampler : WW_REGISTER(s0);

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float3 N = BeachEnvironmentDirection(uv);
    float3 radiance = 0;
    float weight = 0;
    [loop] for (uint i = 0; i < 256; ++i)
    {
        float2 xi = BeachHammersley(i, 256);
        float3 direction;
        float NoL;
        if (g_filterRoughness < 0)
        {
            // Cosine-weighted hemisphere: irradiance = pi * mean radiance.
            float phi = 2.0 * BEACH_PI * xi.x;
            direction = BeachLocalDirection(float3(sqrt(xi.y) * cos(phi),
                sqrt(xi.y) * sin(phi), sqrt(1.0 - xi.y)), N);
            NoL = 1;
        }
        else
        {
            float3 H = BeachSampleGGX(xi, g_filterRoughness, N);
            direction = 2.0 * dot(N, H) * H - N;
            NoL = saturate(dot(N, direction));
        }
        radiance += g_environment.SampleLevel(g_environmentSampler,
            BeachEnvironmentUv(direction), 0).rgb * NoL;
        weight += NoL;
    }
    return float4(radiance / max(weight, 1e-5) *
        (g_filterRoughness < 0 ? BEACH_PI : 1.0), 1);
}
