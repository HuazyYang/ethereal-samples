// DownsampleDepth_PS.hlsl
//
// Copies (and downsamples) the application's scene depth into the internal depth buffer
// (ContextImp_D3D11::BeginAccumulation_CopyDepth, fullscreen pass with depth func ALWAYS).
// Blobs: table ps_DownsampleDepth @ 0x1801FF370 (2 entries)
//   SAMPLEMODE: scene depth is single-sampled (SAMPLEMODE_SINGLE) or MSAA (SAMPLEMODE_MSAA).

#include "ShaderCommon.hlsli"
#include "PostProcess.hlsli"

#if (SAMPLEMODE == SAMPLEMODE_MSAA)
Texture2DMS<float> tDepthMap : register(t0);
#else
Texture2D<float> tDepthMap : register(t0);
#endif

float main(PS_QUAD_INPUT input, uint sampleID : SV_SAMPLEINDEX) : SV_DEPTH
{
    // Checkerboard alternation of the jitter offset between neighbouring pixels
    uint2 pixel = uint2(input.vPos.xy);
    float2 jitter = ((pixel.x + pixel.y) & 1) ? g_vJitterOffset.xy : g_vJitterOffset.yx;
    float2 tex = EvaluateAttributeAtSample(input.vTex, sampleID);
    float2 uv = (tex * g_vOutputViewportSize + jitter) * g_vOutputSize_Inv;
#if (SAMPLEMODE == SAMPLEMODE_MSAA)
    return tDepthMap.Load(int2(uv * g_vOutputSize), 0);
#else
    return tDepthMap.SampleLevel(sPoint, uv, 0);
#endif
}
