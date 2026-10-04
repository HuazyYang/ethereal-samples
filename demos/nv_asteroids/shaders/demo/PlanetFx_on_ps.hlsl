/*
 * Planet seen from space, drawn on a RectPass quad around the planet (DIRECTION = view ray).
 *
 * - Planet surface: lat-long albedo + tangent-space normal map, wrapped sun lighting.
 * - Atmosphere: Sean O'Neil's "Accurate Atmospheric Scattering" (GPU Gems 2, ch. 16) GroundFromSpace
 *   and SkyFromSpace, evaluated per pixel with 2 samples (scale depth 0.25, Kr 0.0025, Km 0.001,
 *   ESun 20, Mie g -0.99).
 * - PlanetWithRingsFx_on_ps.hlsl compiles this file with PLANET_WITH_RINGS=1: a textured ring plane,
 *   shadowed by the planet, composited over the planet / atmosphere / space.
 *
 * All positions are camera relative (camera at the origin).
 *
 * Entry point: main (ps_6_0).   Reconstructed from DXIL (PlanetFx_on_ps, PlanetWithRingsFx_on_ps).
 */

#include "include/space_cb.h"

#ifndef PLANET_WITH_RINGS
#define PLANET_WITH_RINGS 0
#endif

cbuffer cbPlanetFx : register(b0)
{
    PlanetConstants g_param;
};

Texture2D Surface : register(t0);
Texture2D Normals : register(t1);
#if PLANET_WITH_RINGS
Texture2D Rings : register(t2);
#endif
SamplerState SurfaceSampler : register(s0);

static const float PI = 3.14159265;

// O'Neil's scattering constants
static const int nSamples = 2;
static const float3 v3InvWavelength = float3(5.602, 9.4733, 19.6438);    // 1 / pow(float3(0.650, 0.570, 0.475), 4)
static const float fKr = 0.0025;
static const float fKm = 0.0010;
static const float fESun = 20.0;
static const float fKrESun = fKr * fESun;
static const float fKmESun = fKm * fESun;
static const float fKr4PI = fKr * 4.0 * PI;
static const float fKm4PI = fKm * 4.0 * PI;
static const float fScaleDepth = 0.25;
static const float g = -0.99;

float scale(float fCos)
{
    float x = 1.0 - fCos;
    return fScaleDepth * exp(-0.00287 + x * (0.459 + x * (3.83 + x * (-6.80 + x * 5.25))));
}

// Intersections of a ray (unit direction) with a sphere centered at the origin.
// Mangled name: ?raySphereIntersections@@YA_NV?$vector@M$02@@0MMAIAV?$vector@M$01@@@Z
bool raySphereIntersections(float3 rayStart, float3 rayDir, float rayStartDistance2, float sphereRadius, out float2 intersections)
{
    intersections = 0;

    float B = 2.0 * dot(rayStart, rayDir);
    float C = rayStartDistance2 - sphereRadius * sphereRadius;
    float det = B * B - 4.0 * C;

    if (det < 0)
        return false;

    float sqrtDet = sqrt(det);
    intersections = float2(0.5 * (-B - sqrtDet), 0.5 * (-B + sqrtDet));
    return true;
}

#if PLANET_WITH_RINGS
float rayPlaneIntersection(float3 rayStart, float3 rayDir, float4 plane)
{
    return (-dot(plane.xyz, rayStart) - plane.w) / dot(plane.xyz, rayDir);
}

// Lit ring color at a point of the ring plane (relative to the planet center).
// Mangled name: ?ringColor@@YA?AV?$vector@M$03@@V?$vector@M$02@@MM@Z
float4 ringColor(float3 ringPos, float ringPosDistance2, float planetRadius)
{
    // Planet shadow on the rings
    float2 shadowHit;
    bool inShadow = raySphereIntersections(ringPos, g_param.directionToSun, ringPosDistance2, planetRadius, shadowHit)
                    && all(shadowHit > 0);

    float u = saturate((ringPosDistance2 - g_param.innerRingRadiusSquared) / (g_param.outerRingRadiusSquared - g_param.innerRingRadiusSquared));
    float4 ring = Rings.Sample(SurfaceSampler, float2(u, u));

    float lighting = abs(dot(g_param.ringsPlane.xyz, g_param.directionToSun)) * (inShadow ? 0 : 1);

    return float4(g_param.litBrightness * ring.rgb * (g_param.sunColor * lighting * g_param.litBrightness + g_param.ambientBrightness), ring.a);
}
#endif

// Mangled name: ?integratePlanetFromSpace@@YA?AV?$vector@M$03@@V?$vector@M$02@@@Z
float4 integratePlanetFromSpace(float3 direction)
{
    float3 rayDir = normalize(direction);
    float3 rayStart = -g_param.planetPosition;      // camera position relative to the planet center
    float rayStartDistance2 = dot(rayStart, rayStart);

    float fOuterRadius = g_param.atmosphericRadius;
    float fInnerRadius = g_param.radiusRatio * fOuterRadius;
    float fScale = 1.0 / (fOuterRadius - fInnerRadius);
    float fScaleOverScaleDepth = fScale / fScaleDepth;

    float2 planetHit, atmosphereHit;
    bool hitPlanet = raySphereIntersections(rayStart, rayDir, rayStartDistance2, fInnerRadius, planetHit);
    bool hitAtmosphere = raySphereIntersections(rayStart, rayDir, rayStartDistance2, fOuterRadius, atmosphereHit);

    float4 result = 0;

    if (hitPlanet)
    {
        // Surface
        float3 surfacePos = rayStart + rayDir * planetHit.x;
        float3 normal = normalize(surfacePos);

        float2 texcoord = float2(
            (atan(-normal.z / normal.x) / PI + (normal.x < 0 ? 1.0 : 0.0)) * 0.5 + g_param.rotation,
            acos(normal.y) / PI);

        float3 albedo = Surface.SampleLevel(SurfaceSampler, texcoord, g_param.textureLOD).rgb;

        float wrapLighting = saturate(dot(normal, g_param.directionToSun) + g_param.stretchAmount);

        float3 tangent = normalize(float3(normal.z, 0, -normal.x));
        float3 bitangent = normalize(cross(normal, tangent));
        float3 normalSample = Normals.SampleLevel(SurfaceSampler, texcoord, g_param.textureLOD).xyz;
        float3 bumpedNormal = tangent * normalSample.x + bitangent * normalSample.y + normal * normalSample.z;

        float NdotL = saturate(wrapLighting * wrapLighting * dot(bumpedNormal, g_param.directionToSun));
        float3 color = (g_param.sunColor * NdotL * g_param.litBrightness + g_param.ambientBrightness) * albedo;

        if (g_param.radiusRatio < 1)
        {
            // GroundFromSpace
            float3 v3Start = rayStart + rayDir * atmosphereHit.x;
            float fDepth = exp((fInnerRadius - fOuterRadius) / fScaleDepth);
            float fCameraAngle = dot(-rayDir, surfacePos) / length(surfacePos);
            float fLightAngle = dot(g_param.directionToSun, surfacePos) / length(surfacePos);
            float fCameraScale = scale(fCameraAngle);
            float fLightScale = scale(fLightAngle);
            float fCameraOffset = fDepth * fCameraScale;
            float fTemp = fLightScale + fCameraScale;

            float fSampleLength = (planetHit.x - atmosphereHit.x) / nSamples;
            float fScaledLength = fSampleLength * fScale;
            float3 v3SampleRay = rayDir * fSampleLength;
            float3 v3SamplePoint = v3Start + v3SampleRay * 0.5;

            float3 v3FrontColor = 0;
            float3 v3Attenuate = 0;
            [unroll]
            for (int i = 0; i < nSamples; i++)
            {
                float fHeight = length(v3SamplePoint);
                float fSampleDepth = exp(fScaleOverScaleDepth * (fInnerRadius - fHeight));
                float fScatter = fSampleDepth * fTemp - fCameraOffset;
                v3Attenuate = exp(-fScatter * (v3InvWavelength * fKr4PI + fKm4PI));
                v3FrontColor += v3Attenuate * (fSampleDepth * fScaledLength);
                v3SamplePoint += v3SampleRay;
            }

            float3 c0 = v3FrontColor * (v3InvWavelength * fKrESun + fKmESun);
            float3 c1 = v3Attenuate;
            float3 atmosphere = (c0 + 0.25 * c1) * g_param.atmosphereBrightness * g_param.sunColor;

            float atmosphereAlpha = 1.0 - exp(-(planetHit.x - atmosphereHit.x) * g_param.atmosphericAlpha);
            color = lerp(color, atmosphere, atmosphereAlpha);
        }

        result = float4(color, 1);
    }
    else if (hitAtmosphere)
    {
        // SkyFromSpace
        float3 v3Start = rayStart + rayDir * atmosphereHit.x;
        float fStartAngle = dot(rayDir, v3Start) / fOuterRadius;
        float fStartDepth = exp(-1.0 / fScaleDepth);
        float fStartOffset = fStartDepth * scale(fStartAngle);

        float fSampleLength = (atmosphereHit.y - atmosphereHit.x) / nSamples;
        float fScaledLength = fSampleLength * fScale;
        float3 v3SampleRay = rayDir * fSampleLength;
        float3 v3SamplePoint = v3Start + v3SampleRay * 0.5;

        float3 v3FrontColor = 0;
        [unroll]
        for (int i = 0; i < nSamples; i++)
        {
            float fHeight = length(v3SamplePoint);
            float fDepth = exp(fScaleOverScaleDepth * (fInnerRadius - fHeight));
            float fLightAngle = dot(g_param.directionToSun, v3SamplePoint) / fHeight;
            float fCameraAngle = dot(rayDir, v3SamplePoint) / fHeight;
            float fScatter = fStartOffset + fDepth * (scale(fLightAngle) - scale(fCameraAngle));
            float3 v3Attenuate = exp(-fScatter * (v3InvWavelength * fKr4PI + fKm4PI));
            v3FrontColor += v3Attenuate * (fDepth * fScaledLength);
            v3SamplePoint += v3SampleRay;
        }

        float fCos = dot(-rayDir, g_param.directionToSun);
        float fMiePhase = 1.5 * ((1.0 - g * g) / (2.0 + g * g)) * (1.0 + fCos * fCos) / pow(1.0 + g * g - 2.0 * g * fCos, 1.5);

        float3 color = v3FrontColor * (v3InvWavelength * fKrESun + fKmESun * fMiePhase) * g_param.atmosphereBrightness * g_param.sunColor;
        float alpha = 1.0 - exp(-(atmosphereHit.y - atmosphereHit.x) * g_param.atmosphericAlpha);

        result = float4(color, alpha);
    }

#if PLANET_WITH_RINGS
    float ringT = rayPlaneIntersection(float3(0, 0, 0), rayDir, g_param.ringsPlane);
    float3 ringPos = rayDir * ringT - g_param.planetPosition;
    float ringPosDistance2 = dot(ringPos, ringPos);

    if (ringT > 0 && g_param.innerRingRadiusSquared <= ringPosDistance2 && ringPosDistance2 <= g_param.outerRingRadiusSquared)
    {
        // Over the planet disk, only the part of the rings in front of the planet center is visible.
        if (!hitPlanet || dot(ringPos, rayDir) < 0)
        {
            float4 ring = ringColor(ringPos, ringPosDistance2, fInnerRadius);
            result = float4(lerp(result.rgb, ring.rgb, ring.a), hitPlanet ? 1.0 : ring.a);
        }
    }
#endif

    return result;
}

float4 main(
    float4 i_position : SV_Position,
    float2 i_st : ST,
    float3 i_direction : DIRECTION) : SV_Target
{
    // Outside of the circle inscribed in the quad: nothing to draw.
    if (dot(i_st, i_st) > 1)
        return 0;

    return integratePlanetFromSpace(i_direction);
}
