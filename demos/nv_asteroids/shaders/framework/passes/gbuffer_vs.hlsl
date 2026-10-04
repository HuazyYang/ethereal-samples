/*
* Asteroids (2018 donut framework) - G-buffer fill vertex shader (passes/gbuffer_vs.hlsl).
*
* Instanced scene geometry: the per-instance object-to-world transform comes from a second
* vertex stream (TRANSFORM, a row-major float3x4 in three float4 attributes).
*
* Permutations: SINGLE_PASS_STEREO={0,1} MOTION_VECTORS={0,1}
*   MOTION_VECTORS=1    : adds the PREV_TRANSFORM instance attribute and outputs the previous
*                         world position (PREV_WORLD_POS) for the motion vectors of gbuffer_ps.
*   SINGLE_PASS_STEREO=1: also outputs the right-eye clip position (NV_X_RIGHT) and the NVIDIA
*                         single-pass-stereo viewport mask (NV_VIEWPORT_MASK); consumed by
*                         passes/forward_gs.hlsl running as an NVAPI fast geometry shader.
*/

#pragma pack_matrix(row_major)

#include "surface_cb.h"
#include "../framework_cb.h"
#include "../forward_vertex_2018.hlsli"

cbuffer c_GBuffer : register(b0)
{
    GBufferFillConstants c_GBuffer;
};

void main(
    in SceneVertex i_vtx,
    in float3x4 i_instanceMatrix : TRANSFORM,
#if MOTION_VECTORS
    in float3x4 i_prevInstanceMatrix : PREV_TRANSFORM,
#endif
    in uint i_instance : SV_InstanceID,
    out float4 o_position : SV_Position,
    out SceneVertex o_vtx
#if MOTION_VECTORS
    , out float3 o_prevWorldPos : PREV_WORLD_POS
#endif
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

#if MOTION_VECTORS
    o_prevWorldPos = mul(i_prevInstanceMatrix, float4(i_vtx.m_pos, 1.0));
#endif

    float4 worldPos = float4(o_vtx.m_pos, 1.0);
    float4 viewPos = mul(worldPos, c_GBuffer.matWorldToView);
    o_position = mul(viewPos, c_GBuffer.matViewToClip);

#if SINGLE_PASS_STEREO
    float4 viewPosRight = mul(worldPos, c_GBuffer.matWorldToViewRight);
    o_positionRight = mul(viewPosRight, c_GBuffer.matViewToClipRight);
    o_viewportMask = FRAMEWORK_SPS_VIEWPORT_MASK;
#endif
}
