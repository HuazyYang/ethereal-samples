/*
 * Sun disk: drawn on a RectPass quad around the sun direction. ST is the position inside the quad
 * ([-1,1]^2); the disk is a soft (1 - 2 r^2)^4 cap times a gaussian glow, premultiplied by its own
 * alpha.
 *
 * Entry point: main (ps_6_0).   Reconstructed from DXIL (sun_disk_ps).
 */

#include "include/space_cb.h"

cbuffer cbSunDisk : register(b0)
{
    SunConstants g_Sun;
};

float4 main(
    float4 i_position : SV_Position,
    float2 i_st : ST,
    float3 i_direction : DIRECTION) : SV_Target
{
    float r2 = dot(i_st, i_st);
    float intensity = pow(saturate((1.0 - r2) * 2.0), 4.0) * exp(-r2 * 16.0);

    if (intensity == 0)
        discard;

    return float4(g_Sun.color * intensity, intensity);
}
