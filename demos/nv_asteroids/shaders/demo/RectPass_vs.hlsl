/*
 * Full-screen (or arbitrary) rect pass vertex shader used by the sky / planet / sun passes.
 * Draws a 4-vertex triangle strip; each corner takes its clip position and its world-space view
 * direction from the constant buffer, ST is the corner in [-1,1]^2 (y up).
 *
 * Entry point: main (vs_6_0).   Reconstructed from DXIL (RectPass_vs).
 */

#include "include/space_cb.h"

cbuffer cbRectConstants : register(b0)
{
    RectConstants g_param;
};

void main(
    uint vertexID : SV_VertexID,
    out float4 o_position : SV_Position,
    out float2 o_st : ST,
    out float3 o_direction : DIRECTION)
{
    // 0: (-1, 1)  1: (1, 1)  2: (-1, -1)  3: (1, -1)
    o_st = float2(int((vertexID << 1) & 2) - 1, 1 - int(vertexID & 2));

    uint corner = vertexID & 3;
    o_position = g_param.vertices[corner];
    o_direction = g_param.directions[corner].xyz;
}
