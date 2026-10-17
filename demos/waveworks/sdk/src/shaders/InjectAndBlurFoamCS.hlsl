// Recovered from the matching DXBC assembly.
//
// Horizontal half of the separable foam blur, plus foam injection.
// BlurFoamAndCalculateGradientsAndMomentsCS runs the vertical half and packs
// the result into .w of the gradients texture; .z of that texture holds the
// displacement Jacobian, which is what drives foam generation here.

cbuffer CS_CBUFFER : register(b0)
{
    uint g_simulationGridSize : packoffset(c0.x);
    float g_texelsPerMeter : packoffset(c0.y);
    float g_foamTemporalDissipation : packoffset(c0.z);
    float g_foamSpatialDissipation : packoffset(c0.w);
    float g_foamGenerationThreshold : packoffset(c1.x);
    float g_foamGenerationAmount : packoffset(c1.y);
    float2 pad0 : packoffset(c1.z);
};

SamplerState g_bilinearSampler : register(s0);
Texture2D<float4> g_gradientsInput : register(t0);
RWTexture2D<float> g_foamEnergyOutput : register(u0);

[numthreads(16, 16, 1)]
void InjectAndBlurFoamCS(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 thread = dispatchThreadID.xy;
    float2 uv = (float2(thread) + 0.5f) / float(g_simulationGridSize);

    // g_foamSpatialDissipation already arrives divided by the grid size, so it
    // is a UV-space step. The blur runs along X only.
    float2 blurOffset = float2(g_foamSpatialDissipation, 0.0f);

    // Keep the negate on blurOffset. `uv + blurOffset * -2.0f` folds it into
    // the literal and emits `mad r0.zw, r1.xxxy, l(0,0,-2,-2), r0.xxxy` where
    // the blob has `mad r0.zw, -r1.xxxy, l(0,0,2,2), r0.xxxy`.
    float tapA = g_gradientsInput.SampleLevel(g_bilinearSampler,
        uv - blurOffset * 2.0f, 0.0f).w;
    float tapB = g_gradientsInput.SampleLevel(g_bilinearSampler,
        uv - blurOffset, 0.0f).w;
    float tapC = g_gradientsInput.SampleLevel(g_bilinearSampler,
        uv + blurOffset, 0.0f).w;
    float tapD = g_gradientsInput.SampleLevel(g_bilinearSampler,
        uv + blurOffset * 2.0f, 0.0f).w;
    float blurredFoam = tapA + tapB + tapC + tapD;

    // Surface folding (Jacobian below the threshold) injects fresh foam.
    // Plain max()/min(): DXBC already has the NaN behaviour these need, so the
    // decompiler's isnan() emulation would only add ne+movc the blob lacks.
    float jacobian = g_gradientsInput.Load(int3(thread, 0)).z;
    float fold = 1.0f - max(jacobian, 0.0f) - g_foamGenerationThreshold;
    float generatedFoam = max(fold, 0.0f) * g_foamGenerationAmount;

    // Explicit `a * b + c` with the fused product first. mad(blurredFoam,
    // 0.25f, generatedFoam) lets FXC fuse the g_foamGenerationAmount product
    // instead, emitting a separate `mul r0.x, r0.x, l(0.25)` the blob lacks.
    float foam = blurredFoam * 0.25f + generatedFoam;
    g_foamEnergyOutput[thread] = min(foam, 1.0f);
}
