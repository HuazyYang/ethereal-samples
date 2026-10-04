/*
* Asteroids (2018 donut framework) - depth-only vertex shader (passes/depth_vs.hlsl), used for
* shadow map rendering of instanced scene geometry. No permutations.
*/

#pragma pack_matrix(row_major)

#include "../framework_cb.h"

cbuffer c_Depth : register(b0)
{
    DepthPassConstants g_Depth;
};

void main(
    in float3 i_pos : POSITION,
    in float2 i_uv : UV,
    in float3x4 i_instanceMatrix : TRANSFORM,
    in uint i_instance : SV_InstanceID,
    out float4 o_position : SV_Position,
    out float2 o_uv : UV)
{
    float4 worldPos = float4(mul(i_instanceMatrix, float4(i_pos, 1.0)), 1.0);
    o_position = mul(worldPos, g_Depth.matWorldToClip);

    o_uv = i_uv;
}
