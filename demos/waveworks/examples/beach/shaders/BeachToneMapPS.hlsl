#ifndef WW_REGISTER
#ifdef WAVEWORKS_SPACE
#define WW_REGISTER(reg) register(reg, WAVEWORKS_SPACE)
#else
#define WW_REGISTER(reg) register(reg)
#endif
#endif
Texture2D g_hdrScene : WW_REGISTER(t0);
SamplerState g_linearClamp : WW_REGISTER(s0);

struct PSInput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float3 AcesFilm(float3 value)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return saturate((value * (a * value + b)) / (value * (c * value + d) + e));
}

float4 main(PSInput input) : SV_Target
{
    float3 hdr = max(g_hdrScene.Sample(g_linearClamp, input.uv).rgb, 0.0);
    float3 mapped = AcesFilm(hdr * 0.90);
    mapped = pow(mapped, 1.0 / 2.2);
    return float4(mapped, 1.0);
}
