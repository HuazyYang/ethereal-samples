/*
 * Procedural sun lens flare (full-screen additive pass).
 *
 * vs_main evaluates the sun visibility once per vertex (shadow maps sampled at the camera position,
 * i.e. the origin of the camera-relative world) and collapses the quad when the sun is fully
 * occluded; ps_main evaluates the flare: chromatic ghosts along the sun axis (with a fish-eye style
 * distortion of the sun position), a ghost series and a halo, all with 3 channel-shifted copies.
 *
 * Entry points: vs_main (vs_6_0), ps_main (ps_6_0).   Reconstructed from DXIL (lensflare_vs_main, lensflare_ps_main).
 */

#include "include/space_cb.h"
#include "include/space_shadows.hlsli"

ConstantBuffer<LensFlareConstants> g_LensFlare : register(b0);

Texture2DArray t_ShadowMapArray : register(t0);
SamplerComparisonState s_ShadowSampler : register(s0);

// ---------------------------------------------------------------------------------------------
// Vertex shader
// ---------------------------------------------------------------------------------------------

// Sun visibility at a world position: cascaded shadows plus per-object shadows (donut 2018 deferred
// lighting). Mangled name: ?GetShadow@@YAMV?$vector@M$02@@@Z
float GetShadow(float3 worldPos)
{
    LightConstants light = g_LensFlare.light;

    int4 cascadeIndices = light.shadowCascades;
    float2 shadow = 0;
    // note: the loop runs over all LENSFLARE_MAX_SHADOWS (5) slots although shadowCascades only has
    // 4 components; the 5th iteration is only reached if all four cascades are valid and none of them
    // fully covers the point. Kept as compiled.
    for (int cascade = 0; cascade < LENSFLARE_MAX_SHADOWS; cascade++)
    {
        int shadowIndex = cascadeIndices[cascade];
        if (shadowIndex >= 0)
        {
            float2 cascadeShadow = EvaluateShadowPCF(t_ShadowMapArray, s_ShadowSampler, g_LensFlare.shadows[shadowIndex], worldPos);

            shadow = saturate(shadow + cascadeShadow * (1.0001 - shadow.y));

            if (shadow.y == 1)
                break;
        }
        else
            break;
    }

    shadow.x += (1 - shadow.y) * light.outOfBoundsShadow;

    float objectShadow = 1;
    for (int object = 0; object < 4; object++)
    {
        int shadowIndex = light.perObjectShadows[object];
        if (shadowIndex >= 0)
        {
            float2 thisObjectShadow = EvaluateShadowPCF(t_ShadowMapArray, s_ShadowSampler, g_LensFlare.shadows[shadowIndex], worldPos);
            objectShadow *= saturate(thisObjectShadow.x + 1 - thisObjectShadow.y);
        }
    }

    shadow.x *= objectShadow;

    return shadow.x;
}

void vs_main(
    uint vertexID : SV_VertexID,
    out float4 o_position : SV_Position,
    out float2 o_uv : UV,
    out float o_occlusion : OCCLUSION)
{
    o_occlusion = GetShadow(float3(0, 0, 0));

    if (o_occlusion <= 0)
    {
        // Sun fully occluded: degenerate quad.
        o_position = float4(0, 0, -1, 1);
        o_uv = 0;
        return;
    }

    // Full-screen quad as a 4-vertex triangle strip.
    float2 uv = float2(vertexID & 1, (vertexID >> 1) & 1);
    o_position = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 1, 1);
    o_uv = uv;
}

// ---------------------------------------------------------------------------------------------
// Pixel shader
// ---------------------------------------------------------------------------------------------

// Per-channel offset applied to the position along the flare axis (R, G, B).
static const float3 c_ChromaticShift = float3(0.05, 0.0, -0.05);

// Soft disc ghost: positive inside radius pow(0.01 / scale, 1 / 2.4).
float Ghost(float2 p, float scale)
{
    return max(0.01 - pow(length(p), 2.4) * scale, 0);
}

// Ghost centered at -t * pos (in the 'uv + t * pos' frame), with chromatic separation.
float3 Ghost3(float2 uv, float2 pos, float t, float scale)
{
    return float3(
        Ghost(uv + (t - c_ChromaticShift.x) * pos, scale),
        Ghost(uv + (t - c_ChromaticShift.y) * pos, scale),
        Ghost(uv + (t - c_ChromaticShift.z) * pos, scale));
}

float Halo(float2 p)
{
    return max(1.0 / (1.0 + 32.0 * pow(length(p), 2.0)), 0);
}

float3 Halo3(float2 uv, float2 pos, float t)
{
    return float3(
        Halo(uv + (t - c_ChromaticShift.x) * pos),
        Halo(uv + (t - c_ChromaticShift.y) * pos),
        Halo(uv + (t - c_ChromaticShift.z) * pos));
}

// Rotation of v around the unit axis by 'angle' (Rodrigues).
float3 RotateAroundAxis(float3 v, float3 axis, float angle)
{
    float c = cos(angle);
    float s = sin(angle);
    return v * c + cross(axis, v) * s + axis * dot(axis, v) * (1.0 - c);
}

// Series of 5 ghosts between the sun and the screen center, growing with the index.
float3 GhostSeries(float2 uv, float2 pos)
{
    float3 c = 0;
    for (int i = 1; i < 6; i++)
    {
        float t = i / (i + 1.0);
        float scale = i + 0.1;
        c += float3(6.0 - i * 0.5, 6.0, i * 0.5 + 3.0) * Ghost3(uv, pos, t, scale);
    }
    return c;
}

// Mangled name: ?lensflare@@YA?AV?$vector@M$02@@V?$vector@M$01@@0MM@Z
// uv  : pixel position relative to the screen center, x scaled by the aspect ratio
// pos : sun position in the same frame
float3 lensflare(float2 uv, float2 pos, float intensity, float occlusion)
{
    float uvLength = length(uv);

    // The sun position seen through a fish-eye: rotate it around the axis perpendicular to the
    // pixel direction by an angle proportional to the pixel's distance from the center.
    float3 axis = cross(float3(uv, 0), float3(0, 0, 1)) / uvLength;
    float2 distortedPos = RotateAroundAxis(float3(pos, 0), axis, uvLength * 2.221).xy;

    float3 c = GhostSeries(uv, distortedPos);
    c += 24.0 * Ghost3(uv, distortedPos, 0.5, 0.25);
    c *= 0.25;

    c += 18.0 * Ghost3(uv, distortedPos, -1.0, 0.5);
    c += 6.0 * (Ghost3(uv, distortedPos, -3.0, 1.0 / 6.0)
              + Ghost3(uv, distortedPos, 0.5, 0.625)
              + Ghost3(uv, distortedPos, -0.4, 0.625));

    float2 uvd = uv * uvLength;
    c += float3(0.125, 0.115, 0.105) * (Halo3(uvd, pos, 1.0) + Halo3(uvd, pos, -1.0));

    c *= intensity * 0.5;

    return pow(c, 1.5) * occlusion;
}

float4 ps_main(
    float4 i_position : SV_Position,
    float2 i_uv : UV,
    float i_occlusion : OCCLUSION) : SV_Target
{
    float aspect = g_LensFlare.screenScale.y / g_LensFlare.screenScale.x;

    float2 uv = i_uv - 0.5;
    uv.x *= aspect;

    float2 pos = g_LensFlare.pos;
    pos.x *= aspect;

    float3 color = lensflare(uv, pos, g_LensFlare.irradiance, i_occlusion);

    return float4(color, i_occlusion);
}
