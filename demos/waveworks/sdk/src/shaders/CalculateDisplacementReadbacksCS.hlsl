// Recovered from the matching DXBC assembly; low-level register flow is retained.

cbuffer CDCB : register(b0)
{
    uint g_numberOfReadbackSamples : packoffset(c0.x);
    float g_UVWarpingAmplitude : packoffset(c0.y);
    float g_UVWarpingFrequency : packoffset(c0.z);
    float pad0 : packoffset(c0.w);
    float g_UVScaleCascade0 : packoffset(c1.x);
    float g_UVScaleCascade1 : packoffset(c1.y);
    float g_UVScaleCascade2 : packoffset(c1.z);
    float g_UVScaleCascade3 : packoffset(c1.w);
    float g_UVShiftCascade0 : packoffset(c2.x);
    float g_UVShiftCascade1 : packoffset(c2.y);
    float g_UVShiftCascade2 : packoffset(c2.z);
    float g_UVShiftCascade3 : packoffset(c2.w);
};

SamplerState g_bilinearSampler : register(s0);
StructuredBuffer<float2> g_readbackPositions : register(t0);
Texture2D<float4> g_displacementTextureCascade0 : register(t1);
Texture2D<float4> g_displacementTextureCascade1 : register(t2);
Texture2D<float4> g_displacementTextureCascade2 : register(t3);
Texture2D<float4> g_displacementTextureCascade3 : register(t4);
RWStructuredBuffer<float4> g_readbackResults : register(u0);

[numthreads(128, 1, 1)]
void CalculateDisplacementReadbacksCS(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint sampleIndex = dispatchThreadID.x;
    if (sampleIndex < g_numberOfReadbackSamples)
    {
        float2 worldPosition = g_readbackPositions[sampleIndex];

        float2 cascade0 = worldPosition.yx * g_UVScaleCascade0 + g_UVShiftCascade0;
        float2 cascade1 = worldPosition.yx * g_UVScaleCascade1 + g_UVShiftCascade1;
        float2 cascade0Phase = cascade0 * g_UVWarpingFrequency;
        float2 cascade1Phase = cascade1 * g_UVWarpingFrequency;
        float2 warpCos01 = g_UVWarpingAmplitude * float2(cos(cascade0Phase.x), cos(cascade1Phase.x));
        float2 warpSin01 = g_UVWarpingAmplitude * float2(sin(cascade0Phase.y), sin(cascade1Phase.y));
        float2 cascade0Uv = cascade0.yx + float2(warpCos01.x, warpSin01.x);
        float2 cascade1Uv = cascade1.yx + float2(warpCos01.y, warpSin01.y);
        float3 displacement =
            g_displacementTextureCascade0.SampleLevel(g_bilinearSampler, cascade0Uv, 0.0f).xyz;
        displacement +=
            g_displacementTextureCascade1.SampleLevel(g_bilinearSampler, cascade1Uv, 0.0f).xyz;

        float2 cascade2 = worldPosition.yx * g_UVScaleCascade2 + g_UVShiftCascade2;
        float2 cascade3 = worldPosition.yx * g_UVScaleCascade3 + g_UVShiftCascade3;
        float2 cascade2Phase = cascade2 * g_UVWarpingFrequency;
        float2 cascade3Phase = cascade3 * g_UVWarpingFrequency;
        float2 warpCos23 = g_UVWarpingAmplitude * float2(cos(cascade2Phase.x), cos(cascade3Phase.x));
        float2 warpSin23 = g_UVWarpingAmplitude * float2(sin(cascade2Phase.y), sin(cascade3Phase.y));
        float2 cascade2Uv = cascade2.yx + float2(warpCos23.x, warpSin23.x);
        float2 cascade3Uv = cascade3.yx + float2(warpCos23.y, warpSin23.y);
        displacement +=
            g_displacementTextureCascade2.SampleLevel(g_bilinearSampler, cascade2Uv, 0.0f).xyz;
        displacement +=
            g_displacementTextureCascade3.SampleLevel(g_bilinearSampler, cascade3Uv, 0.0f).xyz;
        g_readbackResults[sampleIndex] = float4(displacement, 0.0f);
    }
}
