#ifndef WW_REGISTER
#ifdef WAVEWORKS_SPACE
#define WW_REGISTER(reg) register(reg, WAVEWORKS_SPACE)
#else
#define WW_REGISTER(reg) register(reg)
#endif
#endif
cbuffer BeachFrameConstants : WW_REGISTER(b0)
{
    float4x4 g_viewProjection;
    float3 g_sunDirection; float g_sunIntensity;
    float3 g_eyePosition; float g_haze;
    float3 g_sunRadiance; float g_time;
    float3 g_skyIrradiance; float g_meanSeaLevel;
}

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 materialWeights : COLOR0;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 worldPosition : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float distanceToEye : TEXCOORD2;
    float2 uv : TEXCOORD3;
    float4 materialWeights : TEXCOORD4;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    output.position = mul(g_viewProjection, float4(input.position, 1.0));
    output.worldPosition = input.position;
    output.normal = input.normal;
    output.distanceToEye = distance(g_eyePosition, input.position);
    output.uv = input.uv;
    output.materialWeights = input.materialWeights;
    return output;
}
