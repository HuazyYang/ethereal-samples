#ifndef WW_REGISTER
#ifdef WAVEWORKS_SPACE
#define WW_REGISTER(reg) register(reg, WAVEWORKS_SPACE)
#else
#define WW_REGISTER(reg) register(reg)
#endif
#endif
cbuffer EnvironmentConstants : WW_REGISTER(b0)
{
    float3 g_atmosphereSun; float g_solarIrradiance;
    float g_sourceMip; float g_channels; float2 g_readbackPadding;
};
Texture2D g_source : WW_REGISTER(t0);

// Preserve exact float bits in a portable RGBA8 readback target, avoiding a
// dependency on native floating-point staging formats. This is test-only
// data encoding, never scene shading.
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    uint channels = uint(g_channels);
    uint channel = uint(position.x) % channels;
    float4 value = g_source.Load(int3(uint(position.x) / channels,
        uint(position.y), uint(g_sourceMip)));
    uint bits = asuint(value[channel]);
    return float4(bits & 255u, (bits >> 8u) & 255u,
        (bits >> 16u) & 255u, bits >> 24u) / 255.0;
}
