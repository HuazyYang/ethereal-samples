/*
 * Real star field, pixel shader: soft round star inside the triangle emitted by real_stars_vs.
 *
 * Entry point: PS (ps_6_0).   Reconstructed from DXIL (real_stars_ps_PS).
 */

#include "include/real_stars.hlsli"

float4 PS(VSOut i) : SV_Target
{
    float r = length(i.uv.xy);

    if (r >= 1.0)
        return 0;

    return float4((i.uv.z * 0.001 * exp(-10.0 * r)).xxx, 1);
}
