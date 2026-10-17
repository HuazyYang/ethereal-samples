// Recovered from the matching DXBC assembly; low-level register flow is retained.

cbuffer CS_CBUFFER : register(b0)
{
    uint g_simulationGridSize;
    float g_texelsPerMeter;
    float g_foamTemporalDissipation;
    float g_foamSpatialDissipation;
    float g_foamGenerationThreshold;
    float g_foamGenerationAmount;
    float2 pad0;
};

SamplerState g_bilinearSampler : register(s0);
Texture2D<float> g_foamEnergyInput : register(t0);
RWTexture2D<float4> g_displacementsInOut : register(u0);
RWTexture2D<float4> g_gradientsOutput : register(u1);
RWTexture2D<float4> g_momentsOutput : register(u2);

[numthreads(16, 16, 1)]
void BlurFoamAndCalculateGradientsAndMomentsCS(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 thread = dispatchThreadID.xy;
    float2 uv = (float2(thread) + 0.5f) / float(g_simulationGridSize);
    float4 blurOffset;
    blurOffset.x = 0.0f;
    blurOffset.y = g_foamSpatialDissipation;
    float foam = g_foamEnergyInput.SampleLevel(g_bilinearSampler,
        uv - blurOffset.xy * float2(2.0f, 2.0f), 0.0f);
    foam += g_foamEnergyInput.SampleLevel(g_bilinearSampler, uv - blurOffset.xy, 0.0f);
    foam += g_foamEnergyInput.SampleLevel(g_bilinearSampler, uv + blurOffset.xy, 0.0f);
    foam += g_foamEnergyInput.SampleLevel(g_bilinearSampler,
        mad(blurOffset.xy, float2(2.0f, 2.0f), uv), 0.0f);
    foam *= 0.25f * g_foamTemporalDissipation;

    uint gridMinusOne = g_simulationGridSize - 1;
    uint2 negative = gridMinusOne + thread;
    negative.x = negative.x >= g_simulationGridSize ? negative.x - g_simulationGridSize : negative.x;
    negative.y = negative.y >= g_simulationGridSize ? negative.y - g_simulationGridSize : negative.y;
    uint2 positive = thread + 1;
    positive.x = positive.x >= g_simulationGridSize ? positive.x - g_simulationGridSize : positive.x;
    positive.y = positive.y >= g_simulationGridSize ? positive.y - g_simulationGridSize : positive.y;

    float3 negativeX = g_displacementsInOut[uint2(negative.x, thread.y)].xyz;
    float3 negativeY = g_displacementsInOut[uint2(thread.x, negative.y)].xyz;
    float4 negativeYScaled = negativeY.zyxy * g_texelsPerMeter;
    float4 negativeXScaled = negativeX.zxxy * g_texelsPerMeter;
    float3 positiveX = g_displacementsInOut[uint2(positive.x, thread.y)].xyz;
    float3 positiveY = g_displacementsInOut[uint2(thread.x, positive.y)].xyz;
    float3 center = g_displacementsInOut[thread].xyz;

    float2 negativeXSlope = mad(g_texelsPerMeter, center.zx, -negativeXScaled.xy);
    float2 negativeYSlope = mad(g_texelsPerMeter, center.zy, -negativeYScaled.xy);
    float2 lateralX = mad(g_texelsPerMeter, positiveX.xy, -negativeXScaled.zw);
    float2 lateralY = mad(g_texelsPerMeter, positiveY.xy, -negativeYScaled.zw);
    float jacobian = (1.0f + lateralX.x) * (1.0f + lateralY.y) - lateralX.y * lateralY.x;

    float3 centerScaled = center.zxy * g_texelsPerMeter;
    float2 positiveYSlope;
    float2 positiveXSlope;
    positiveXSlope.x = mad(g_texelsPerMeter, positiveX.z, -centerScaled.x);
    positiveXSlope.y = mad(g_texelsPerMeter, positiveX.x, -centerScaled.y);
    positiveYSlope.x = mad(g_texelsPerMeter, positiveY.z, -centerScaled.x);
    positiveYSlope.y = mad(g_texelsPerMeter, positiveY.y, -centerScaled.z);
    float gradientX = 0.5f *
        ((-negativeXSlope.x) / max(1.0f + negativeXSlope.y, 0.1f) +
         (-positiveXSlope.x) / max(1.0f + positiveXSlope.y, 0.1f));
    float gradientY = 0.5f *
        ((-positiveYSlope.x) / max(1.0f + positiveYSlope.y, 0.1f) +
         (-negativeYSlope.x) / max(1.0f + negativeYSlope.y, 0.1f));

    float4 gradients = float4(gradientX, gradientY, jacobian, foam);
    g_gradientsOutput[thread] = gradients;
    g_momentsOutput[thread] = float4(gradientX * gradientX,
        gradientY * gradientY, gradientX * gradientY, 1.0f);
}
