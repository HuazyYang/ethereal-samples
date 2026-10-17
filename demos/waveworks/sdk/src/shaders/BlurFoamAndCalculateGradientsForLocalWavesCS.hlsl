// Recovered from the matching DXBC assembly.

cbuffer CS_CBUFFER : register(b0)
{
    uint g_simulationGridSize;
    float g_simulationWorldspaceSize;
    float g_dT;
    float g_foamTemporalDissipation;
    float g_foamSpatialDissipation;
    float g_foamGenerationThreshold;
    float g_foamGenerationAmount;
    float pad0;
};

SamplerState g_bilinearSampler : register(s0);
Texture2D<float> g_foamEnergyInput : register(t0);
RWTexture2D<float4> g_displacementsInOut : register(u0);
RWTexture2D<float4> g_gradientsOutput : register(u1);

[numthreads(16, 16, 1)]
void BlurFoamAndCalculateGradientsForLocalWavesCS(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 thread = dispatchThreadID.xy;
    uint last = g_simulationGridSize;
    last = last - 1;
    if (all(thread < last) && thread.x > 0 && thread.y > 0)
    {
        float scale = float(g_simulationGridSize) / g_simulationWorldspaceSize;
        float3 left = g_displacementsInOut[uint2(thread.x - 1, thread.y)].xyz;
        float3 leftScaled = scale * left;
        float3 right = g_displacementsInOut[uint2(thread.x + 1, thread.y)].xyz;
        float3 rightScaled = scale * right;
        float3 lower = g_displacementsInOut[uint2(thread.x, thread.y - 1)].xyz;
        float3 lowerScaled = scale * lower;
        float3 upper = g_displacementsInOut[uint2(thread.x, thread.y + 1)].xyz;
        float3 upperScaled = scale * upper;

        // The shipped blob scales each neighbour separately and subtracts the
        // scaled values (mad r5.x, r0.z, r2.z, -r0.w), not scale * (a - b).
        float2 gradient = 0.5f * float2(leftScaled.z - rightScaled.z,
            lowerScaled.z - upperScaled.z);
        float2 lateralX = rightScaled.xy - leftScaled.xy;
        float2 lateralY = upperScaled.xy - lowerScaled.xy;
        float jacobian = (1.0f + lateralX.x) * (1.0f + lateralY.y) - lateralX.y * lateralY.x;

        float2 uv = (float2(thread) + 0.5f) / float(g_simulationGridSize);
        float inverseWorldspaceSize = 1.0f / g_simulationWorldspaceSize;
        float spatialDissipation = inverseWorldspaceSize * g_foamSpatialDissipation;
        float2 offset;
        offset.x = 0.0f;
        offset.y = spatialDissipation * g_dT;
        float tapA = g_foamEnergyInput.SampleLevel(g_bilinearSampler,
            uv - offset * float2(2.0f, 2.0f), 0.0f);
        float tapB = g_foamEnergyInput.SampleLevel(g_bilinearSampler, uv - offset, 0.0f);
        float tapC = g_foamEnergyInput.SampleLevel(g_bilinearSampler, uv + offset, 0.0f);
        float tapD = g_foamEnergyInput.SampleLevel(g_bilinearSampler,
            mad(offset, float2(2.0f, 2.0f), uv), 0.0f);
        float foam = tapA + tapB + tapC + tapD;
        // mul r0.y, r0.y, l(0.250000) precedes mul r5.w, r0.z, r0.y: the shipped
        // blob quarters the sum first and applies the dissipation factor after.
        foam *= 0.25f;
        foam *= pow(abs(g_foamTemporalDissipation), g_dT);
        g_gradientsOutput[thread] = float4(gradient, jacobian, foam);
    }
    if (any(thread == 0) || thread.x == last || thread.y == last)
    {
        g_displacementsInOut[thread] = 0.0f.xxxx;
        g_gradientsOutput[thread] = float4(0.0f, 0.0f, 1.0f, 0.0f);
    }
}
