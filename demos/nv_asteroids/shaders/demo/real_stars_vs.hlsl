/*
 * Real star field, vertex shader: every star of the catalogue (media/SkyAndStars/Stars.buf, see
 * StarInstance in space_cb.h) is drawn as one small screen-aligned triangle. Non-indexed draw,
 * 3 vertices per star, REAL_STARS_PER_INSTANCE (64) stars per instance.
 *
 * Entry point: VS (vs_6_0).   Reconstructed from DXIL (real_stars_vs_VS).
 */

#include "include/space_cb.h"
#include "include/real_stars.hlsli"

cbuffer CelestialConstants : register(b0)
{
    SPACE_ROW_MAJOR float4x4 ViewProjectionToClip;  // equatorial unit direction -> clip
    float ST;           // sidereal time            (unused)
    float SinLAT;       // sin(observer latitude)   (unused)
    float CosLAT;       // cos(observer latitude)   (unused)
    float Aspect;
    float Brightness;
};

StructuredBuffer<StarInstance> t_StarInstances : register(t0);

// Equilateral triangle circumscribing the unit circle.
static const float2 TrianglePoints[3] = {
    float2(0, 2),
    float2(1.7320508, -1),
    float2(-1.7320508, -1)
};

static const float PI = 3.14159265;

VSOut VS(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
    float vertexPos = (float(vertexID) + 0.5) / 3.0;
    uint corner = uint(floor(frac(vertexPos) * 3.0));
    uint starIndex = uint(floor(vertexPos)) + (instanceID << 6);  // REAL_STARS_PER_INSTANCE == 64

    StarInstance star = t_StarInstances[starIndex];

    float ra = star.Ra * (PI / 180.0);
    float dec = star.Dec * (PI / 180.0);

    VSOut o;
    o.dir = float3(cos(ra) * cos(dec), sin(dec), sin(ra) * cos(dec));

    float4 clipPos = mul(float4(o.dir, 1), ViewProjectionToClip);
    float2 screenPos = clipPos.xy / clipPos.w;

    // The triangle grows with brightness; once it is larger than the minimum size the extra
    // brightness goes into the size instead of the intensity.
    float size = 2.0 * exp(-0.3 * star.Mag);
    float clampedSize = max(1.0, size);
    float intensity = size / clampedSize;

    float2 corner2 = TrianglePoints[corner];
    o.pos = float4(screenPos + corner2 * 0.0033 * float2(1, Aspect) * clampedSize, 1, 1);
    o.uv = float3(corner2, intensity * intensity * 1000.0 * intensity * exp(-0.2 * star.Mag) * Brightness);

    return o;
}
