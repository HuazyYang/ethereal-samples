// Recovered from the matching DXBC assembly; low-level register flow is retained.

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
Texture2D<float4> g_gradientsInput : register(t0);
RWTexture2D<float> g_foamEnergyOutput : register(u0);

[numthreads(16, 16, 1)]
void InjectAndBlurFoamForLocalWavesCS(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    if (all(dispatchThreadID.xy < g_simulationGridSize.xx))
    {
        float2 uv = (float2(dispatchThreadID.xy) + 0.5f) / float(g_simulationGridSize);
        float inverseWorldspaceSize = 1.0f / g_simulationWorldspaceSize;
        float spatialDissipation = inverseWorldspaceSize * g_foamSpatialDissipation;
        float offset = spatialDissipation * g_dT;
        float2 blurOffset = float2(offset, 0.0f);
        float2 blurOffset2 = blurOffset + blurOffset;
        float tapA = g_gradientsInput.SampleLevel(g_bilinearSampler, uv - blurOffset2, 0.0f).w;
        float tapB = g_gradientsInput.SampleLevel(g_bilinearSampler, uv - blurOffset, 0.0f).w;
        float tapC = g_gradientsInput.SampleLevel(g_bilinearSampler, uv + blurOffset, 0.0f).w;
        float tapD = g_gradientsInput.SampleLevel(g_bilinearSampler, uv + blurOffset2, 0.0f).w;
        // .z of the gradients texture is the displacement Jacobian, not a
        // gradient: BlurFoamAndCalculateGradientsForLocalWavesCS writes
        // float4(gradient.xy, jacobian, foam).
        float jacobian = g_gradientsInput.Load(int3(dispatchThreadID.xy, 0)).z;
        float blurredFoam = tapA + tapB + tapC + tapD;
        float jacobianWithThreshold = jacobian + g_foamGenerationThreshold;
        float generatedFoam = min(max(1.0f - jacobianWithThreshold, 0.0f) *
                                  g_foamGenerationAmount * g_dT,
            1.0f);
        g_foamEnergyOutput[dispatchThreadID.xy] = mad(blurredFoam, 0.25f, generatedFoam);
    }
}
