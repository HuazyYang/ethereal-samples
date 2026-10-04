/*
* Asteroids (2018 donut framework) - forward shading vertex shader (passes/forward_vs.hlsl).
*
* Uses the monolithic 2018 ForwardShadingConstants (c_Forward, here at b0; the forward pixel
* shaders see the same constants at b1).
*
* Permutations: SINGLE_PASS_STEREO={0,1}
*   SINGLE_PASS_STEREO=1: NV_X_RIGHT = left clip position with x replaced by
*                         dot(worldPos, worldToClipXRight); NV_VIEWPORT_MASK for the NVAPI
*                         single-pass-stereo fast GS (passes/forward_gs.hlsl, MOTION_VECTORS=0).
*/

#pragma pack_matrix(row_major)

#include "surface_cb.h"
#include "../framework_cb.h"
#include "../forward_vertex_2018.hlsli"

cbuffer c_Forward : register(b0)
{
    ForwardShadingConstants g_Forward;
};

void main(
    in SceneVertex i_vtx,
    in float3x4 i_instanceMatrix : TRANSFORM,
    in uint i_instance : SV_InstanceID,
    out float4 o_position : SV_Position,
    out SceneVertex o_vtx
#if SINGLE_PASS_STEREO
    , out float4 o_positionRight : NV_X_RIGHT
    , out uint4 o_viewportMask : NV_VIEWPORT_MASK
#endif
)
{
    o_vtx = i_vtx;
    o_vtx.m_pos = mul(i_instanceMatrix, float4(i_vtx.m_pos, 1.0));
    o_vtx.m_normal = mul(i_instanceMatrix, float4(i_vtx.m_normal, 0));
    o_vtx.m_tangent = mul(i_instanceMatrix, float4(i_vtx.m_tangent, 0));
    o_vtx.m_bitangent = mul(i_instanceMatrix, float4(i_vtx.m_bitangent, 0));

    float4 worldPos = float4(o_vtx.m_pos, 1.0);
    o_position = mul(worldPos, g_Forward.matWorldToClip);

#if SINGLE_PASS_STEREO
    o_positionRight = float4(dot(worldPos, g_Forward.worldToClipXRight), o_position.yzw);
    o_viewportMask = FRAMEWORK_SPS_VIEWPORT_MASK;
#endif
}
