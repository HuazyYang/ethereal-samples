/*
 * Interface shared by real_stars_vs.hlsl (VS) and real_stars_ps.hlsl (PS).
 * The VSOut struct name and member names come from the DXIL type annotations of both shaders.
 */

#ifndef REAL_STARS_HLSLI
#define REAL_STARS_HLSLI

struct VSOut
{
    float4 pos : SV_Position;
    float3 uv  : TEXCOORD1;     // xy: position inside the star triangle (unit circle = star disk), z: intensity * 1000
    float3 dir : TEXCOORD2;     // unit direction to the star (equatorial frame); not used by PS
};

#endif // REAL_STARS_HLSLI
