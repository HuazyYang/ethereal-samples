// Recovered from the matching DXBC assembly; low-level register flow is retained.

cbuffer CDCB : register(b0)
{
    float2 g_SimulationDomainCenter : packoffset(c0.x);
    float g_SimulationDomainSize : packoffset(c0.z);
    uint g_numberOfReadbackSamples : packoffset(c0.w);
};

SamplerState g_bilinearSampler : register(s0);
StructuredBuffer<float2> g_readbackPositions : register(t0);
Texture2D<float4> g_displacementTexture : register(t1);
RWStructuredBuffer<float4> g_readbackResults : register(u0);

[numthreads(128, 1, 1)]
void CalculateDisplacementReadbacksForLocalWavesCS(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint sampleIndex = dispatchThreadID.x;
    if (sampleIndex < g_numberOfReadbackSamples)
    {
        float2 worldPosition = g_readbackPositions[sampleIndex];
        float2 uv = (worldPosition - g_SimulationDomainCenter) / g_SimulationDomainSize + 0.5f;
        float3 displacement = g_displacementTexture.SampleLevel(g_bilinearSampler, uv, 0.0f).xyz;
        g_readbackResults[sampleIndex] = float4(displacement, 0.0f);
    }
}
