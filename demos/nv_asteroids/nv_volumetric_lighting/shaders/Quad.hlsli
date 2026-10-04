// Quad.hlsli
//
// Interface of the fullscreen triangle (Quad_VS.hlsl) consumed by the post-process pixel shaders
// (DownsampleDepth, ComputePhaseLookup, Resolve, TemporalFilter, Apply, RenderVolume sky/final passes).

#ifndef NVVL_QUAD_HLSLI
#define NVVL_QUAD_HLSLI

struct VS_QUAD_OUTPUT
{
    float4 vPos : SV_POSITION;
    float4 vWorldPos : TEXCOORD0;
    float2 vTex : TEXCOORD1;
};

#endif // NVVL_QUAD_HLSLI
