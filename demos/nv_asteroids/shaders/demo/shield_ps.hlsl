/*
* Asteroids (2018) - ship shield pixel shader (full-screen pass).
*
* Reconstructs the translated-world position of the depth buffer sample, intersects the view
* ray with the shield sphere around the ship and shades the front and back faces of the bubble
* with an animated noise pattern (shadeShield, fresnel weighted). A hexagonal grid band sweeps
* over the sphere with the shield intensity. Output is premultiplied alpha.
*
* No permutations.
*/

#pragma pack_matrix(row_major)

#include "surface_cb.h"

cbuffer g_Shield : register(b0)
{
    ShieldConstants g_Shield;
};

Texture2D<float> t_Depth : register(t0);

static const float PI = 3.14159;

float square(float x)
{
    return x * x;
}

// Ray / sphere intersection with the ray origin given relative to the sphere center and its
// distance to the center precomputed. Returns the two ray parameters (entry, exit).
bool raySphereIntersections(float3 rayOrigin, float3 rayDir, float rayOriginDistance, float sphereRadius, out float2 intersections)
{
    float b = 2.0 * dot(rayOrigin, rayDir);
    float c = square(rayOriginDistance) - square(sphereRadius);
    float discriminant = b * b - 4.0 * c;

    if (discriminant < 0)
    {
        intersections = 0;
        return false;
    }

    float s = sqrt(discriminant);
    intersections = float2(-b - s, -b + s) * 0.5;
    return true;
}

// Value noise, after Inigo Quilez.
float hash(float n)
{
    return frac(sin(n) * 753.5453123);
}

float valueNoise(float3 x)
{
    float3 p = floor(x);
    float3 f = frac(x);
    f = f * f * (3.0 - 2.0 * f);

    float n = p.x + p.y * 157.0 + 113.0 * p.z;
    return lerp(lerp(lerp(hash(n + 0.0), hash(n + 1.0), f.x),
                     lerp(hash(n + 157.0), hash(n + 158.0), f.x), f.y),
                lerp(lerp(hash(n + 113.0), hash(n + 114.0), f.x),
                     lerp(hash(n + 270.0), hash(n + 271.0), f.x), f.y), f.z);
}

float4 shadeShield(float3 normal, float3 rayDir, bool frontFace)
{
    float fresnel = pow(saturate(1.0 - abs(dot(normal, rayDir))), 5.0);
    fresnel = fresnel + (1.0 - fresnel) * 0.01;

    float frequency = 65.0 + 15.0 * sin(g_Shield.time * 2.0);
    float noise = valueNoise(normal * 5.0 + g_Shield.time);
    float pattern = smoothstep(0.1, 0.4, noise) * smoothstep(0.8, 0.65, noise) * (sin(noise * frequency) * 0.5 + 0.5);

    float intensity = square(saturate(dot(normal, g_Shield.shieldDirection) + 0.8)) * g_Shield.shieldIntensity;

    float3 color = frontFace ? float3(0.0, 0.878, 0.69) : float3(0.003, 0.188, 0.352);
    float alpha;

    if (intensity < 1.0)
    {
        alpha = saturate(pattern - 1.0 + sqrt(intensity));
    }
    else
    {
        color *= intensity;
        alpha = pattern;
    }

    alpha *= fresnel;
    return float4(color * alpha, alpha);
}

// Hexagon cell edge distance, after Inigo Quilez ("hexagons - distance").
float hexagonEdgeDistance(float2 p)
{
    float2 q = float2(p.x * 2.0 * 0.5773503, p.y + p.x * 0.5773503);

    float2 pi = floor(q);
    float2 pf = frac(q);

    float v = fmod(pi.x + pi.y, 3.0);

    float ca = step(1.0, v);
    float cb = step(2.0, v);
    float2 ma = step(pf.xy, pf.yx);

    // distance to borders
    return dot(ma, 1.0 - pf.yx + ca * (pf.x + pf.y - 1.0) + cb * (pf.yx - 2.0 * pf.xy));
}

// Hexagonal grid band that moves across the sphere with the shield intensity.
// unresolved: the exact spherical parameterization feeding the hexagon grid is only known up
// to constant folding; this form reproduces the compiled constants.
float4 shadeHexGrid(float3 normal)
{
    float facing = (dot(normal, g_Shield.shieldDirection) + 1.0) * 0.5;
    float intensity = saturate(g_Shield.shieldIntensity);
    float band = smoothstep(intensity - 0.6, intensity - 0.3, facing) - smoothstep(intensity - 0.3, intensity, facing);

    float2 sphereUV = float2(atan(normal.z / normal.x), asin(normal.y)) / PI;
    float2 gridPos = (float2(sphereUV.x * 0.8660254, -sphereUV.y * 2.0) + 1.0) * 75.0;

    float edgeDistance = hexagonEdgeDistance(gridPos);
    float grid = 1.0 - smoothstep(0.0, 0.075, edgeDistance);

    float3 color = lerp(float3(0.003, 0.188, 0.352), float3(0.0, 0.439, 0.345), frac(g_Shield.shieldIntensity * 5.0 + 0.5 - sphereUV.y));

    return float4(color, 1.0) * (band * g_Shield.shieldIntensity) * grid;
}

void main(
    in float4 i_position : SV_Position,
    in float2 i_uv : UV,
    out float4 o_color : SV_Target0)
{
    o_color = 0;

    float depth = t_Depth[uint2(i_position.xy)];

    float4 clipPos = float4(i_uv.x * 2.0 - 1.0, 1.0 - i_uv.y * 2.0, depth, 1.0);
    float4 worldPos = mul(clipPos, g_Shield.matClipToTranslatedWorld);
    worldPos /= worldPos.w;

    // Note: length and direction are taken of the homogeneous float4 (w = 1), as in the original.
    float sceneDistance = length(worldPos);
    float3 rayDir = normalize(worldPos).xyz;

    float2 intersections;
    if (!raySphereIntersections(-g_Shield.shipPosition, rayDir, g_Shield.distanceToShip, g_Shield.shieldRadius, intersections))
        discard;

    // Shield hidden by the scene, or entirely behind the camera.
    bool insideShield = intersections.x <= 0;
    if (sceneDistance < intersections.x || (insideShield && sceneDistance < intersections.y) || intersections.y <= 0)
        discard;

    float3 frontNormal = normalize(rayDir * intersections.x - g_Shield.shipPosition);
    float3 backNormal = normalize(rayDir * intersections.y - g_Shield.shipPosition);

    float4 frontColor = shadeShield(frontNormal, rayDir, true);
    float4 backColor = shadeShield(backNormal, rayDir, false);

    float4 bubbleColor;
    if (intersections.x <= 0)
        bubbleColor = backColor;
    else if (sceneDistance < intersections.y)
        bubbleColor = frontColor;
    else
    {
        bubbleColor.rgb = frontColor.rgb + backColor.rgb * (1.0 - frontColor.a);
        bubbleColor.a = 1.0 - (1.0 - frontColor.a) * (1.0 - backColor.a);
    }

    float4 hexColor = insideShield ? shadeHexGrid(backNormal) : shadeHexGrid(frontNormal);

    o_color = bubbleColor + hexColor;
}
