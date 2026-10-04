/*
 * Dummy vertex-shader outputs of the 2018 NVAPI task / mesh shaders.
 *
 * The NVAPI "mesh shaders" are vertex shaders: their real outputs are written with the extension ops, but the
 * VS output signature still defines the attribute registers the pixel shader links to (generic attribute slot N
 * of op 38 / 39 = output register N). Every entry point returns this struct filled with 12345.0.
 *
 *   NV_OUTPUT_ASTEROIDS=1 : asteroidTS / asteroidMS   (SV_POSITION, ATTR1, ATTR2, ALPHA_LOD)
 *   NV_OUTPUT_ASTEROIDS=0 : basicTS / basicMS / debugTS / debugMS (SV_POSITION, POS, UV, NORMAL, TANGENT,
 *                           BITANGENT, PREV_WORLD_POS)
 *   DEPTH_PRE_PASS=1      : DepthOutput { SV_POSITION }
 */

#ifndef NV_OUTPUTS_HLSLI
#define NV_OUTPUTS_HLSLI

#ifndef DEPTH_PRE_PASS
#define DEPTH_PRE_PASS 0
#endif

#if DEPTH_PRE_PASS

struct DepthOutput
{
    float4 i_position : SV_POSITION;
};
#define VertexOutput DepthOutput

VertexOutput DummyOutput()
{
    VertexOutput output;
    output.i_position = 12345.0;
    return output;
}

#elif NV_OUTPUT_ASTEROIDS

struct Output
{
    float4 i_position : SV_POSITION;
    float4 attr1 : ATTR1;
    float4 attr2 : ATTR2;
    nointerpolation float4 alphalod : ALPHA_LOD;
};
#define VertexOutput Output

VertexOutput DummyOutput()
{
    VertexOutput output;
    output.i_position = 12345.0;
    output.attr1 = 12345.0;
    output.attr2 = 12345.0;
    output.alphalod = 12345.0;
    return output;
}

#else

struct Output
{
    float4 i_position : SV_POSITION;
    float3 m_pos : POS;
    float2 m_uv : UV;
    centroid float3 m_normal : NORMAL;
    centroid float3 m_tangent : TANGENT;
    centroid float3 m_bitangent : BITANGENT;
    float3 m_prevPos : PREV_WORLD_POS;
};
#define VertexOutput Output

VertexOutput DummyOutput()
{
    VertexOutput output;
    output.i_position = 12345.0;
    output.m_pos = 12345.0;
    output.m_uv = 12345.0;
    output.m_normal = 12345.0;
    output.m_tangent = 12345.0;
    output.m_bitangent = 12345.0;
    output.m_prevPos = 12345.0;
    return output;
}

#endif

// Generic attribute slots (output registers of Output above).
#define NV_SLOT_ATTR1       1
#define NV_SLOT_ATTR2       2
#define NV_SLOT_ALPHA_LOD   3
#define NV_SLOT_POS         1
#define NV_SLOT_UV          2
#define NV_SLOT_NORMAL      3
#define NV_SLOT_TANGENT     4
#define NV_SLOT_BITANGENT   5
#define NV_SLOT_PREV_POS    6

#endif // NV_OUTPUTS_HLSLI
