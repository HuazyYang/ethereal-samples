// PostProcess.hlsli
//
// Pixel shader input of the fullscreen post-process passes. Same register layout as
// VS_QUAD_OUTPUT (Quad.hlsli) but with per-sample interpolation of the texture coordinate:
// every post-process blob declares "dcl_input_ps linear sample v2" and runs at sample frequency.

#ifndef NVVL_POSTPROCESS_HLSLI
#define NVVL_POSTPROCESS_HLSLI

struct PS_QUAD_INPUT
{
    float4 vPos : SV_POSITION;
    float4 vWorldPos : TEXCOORD0;
    sample float2 vTex : TEXCOORD1;
};

#endif // NVVL_POSTPROCESS_HLSLI
