/*
* Asteroids (2018) - analytic lights, shadow map filtering and light probe helpers of the early
* donut framework (lighting.hlsli + shadows.hlsli + forward_ps helpers of that era).
*
* Reconstructed from the inlined ShadeSurface, EvaluateShadowGather16, EvaluateShadowPoisson,
* ShadowWithMaxDistance, sharpen and GetIncidentVector code in forward_ps and
* deferred_lighting_ps. The shadow functions are identical to the 2021 public donut
* shadows.hlsli; the GGX function is the one that is commented out (as _GGX) in the 2021
* lighting.hlsli; ShadeSurface is the 2018 version that returns scalar radiances.
*
* The calling shader declares the textures and samplers and passes them in.
*/

#ifndef LIGHTING_2018_HLSLI
#define LIGHTING_2018_HLSLI

#include "surface_cb.h"
#include "surface_2018.hlsli"

static const float M_PI = 3.14159265f;

float3 GetIncidentVector(float4 directionOrPosition, float3 surfacePos)
{
    if (directionOrPosition.w > 0)
        return normalize(surfacePos.xyz - directionOrPosition.xyz);
    else
        return directionOrPosition.xyz;
}

float Lambert(float3 normal, float3 lightIncident)
{
    return max(0, -dot(normal, lightIncident));
}

float3 slerp(float3 a, float3 b, float angle, float t)
{
    t = saturate(t);
    float sin1 = sin(angle * t);
    float sin2 = sin(angle * (1 - t));
    float ta = sin1 / (sin1 + sin2);
    float3 result = lerp(a, b, ta);
    return normalize(result);
}

float GGX(SurfaceParams surface, float3 lightIncident, float3 viewIncident, float halfAngularSize)
{
    float3 N = surface.normal;
    float3 V = -viewIncident;
    float3 L = -lightIncident;
    float3 R = reflect(viewIncident, N);

    // Correction of light vector L for spherical / directional area lights.
    // Inspired by "Real Shading in Unreal Engine 4" by B. Karis,
    // re-formulated to work in spherical coordinates instead of world space.
    float AngleLR = acos(clamp(dot(R, L), -1, 1));

    float3 CorrectedL = (AngleLR > 0) ? slerp(L, R, AngleLR, saturate(halfAngularSize / AngleLR)) : L;
    float3 H = normalize(CorrectedL + V);

    float NdotH = max(0, dot(N, H));
    float NdotL = max(0, dot(N, CorrectedL));
    float NdotV = max(0, dot(N, V));

    float Alpha = max(0.01, square(surface.roughness));

    // Normalization for the widening of D, see the paper referenced above.
    float CorrectedAlpha = saturate(Alpha + 0.5 * tan(halfAngularSize));
    float SphereNormalization = square(Alpha / CorrectedAlpha);

    // GGX / Trowbridge-Reitz NDF with normalization for sphere lights
    float D = square(Alpha) / (M_PI * square(square(NdotH) * (square(Alpha) - 1) + 1)) * SphereNormalization;

    // Schlick model for geometric attenuation
    // The (NdotL * NdotV) term in the numerator is cancelled out by the same term in the denominator of the final result.
    float k = square(surface.roughness + 1) / 8.0;
    float G = 1 / ((NdotL * (1 - k) + k) * (NdotV * (1 - k) + k));

    return D * G / 4;
}

// Returns the diffuse and specular radiance of one light as scalars; the caller multiplies them
// by the light color (and the specular one by the surface specular color).
void ShadeSurface(LightConstants light, SurfaceParams surface, float3 viewIncident, out float o_diffuseRadiance, out float o_specularRadiance)
{
    o_diffuseRadiance = 0;
    o_specularRadiance = 0;

    float3 incidentVector = 0;
    float halfAngularSize = 0;
    float attenuation = 1;
    float spotlight = 1;

    if (light.lightType == LightType_Directional)
    {
        incidentVector = light.direction;
        halfAngularSize = light.angularSizeOrInvRange * 0.5;
    }
    else if (light.lightType == LightType_Spot || light.lightType == LightType_Point)
    {
        float3 lightToSurface = surface.worldPos - light.position;
        float distance = sqrt(dot(lightToSurface, lightToSurface));
        float rDistance = 1.0 / distance;
        incidentVector = lightToSurface * rDistance;

        if (light.angularSizeOrInvRange > 0)
        {
            attenuation = square(saturate(1.0 - square(square(distance * light.angularSizeOrInvRange))));

            if (attenuation == 0)
                return;
        }

        halfAngularSize = atan(min(light.radius * rDistance, 1));

        if (light.lightType == LightType_Spot)
        {
            float LdotD = dot(incidentVector, light.direction);
            float directionAngle = acos(LdotD);
            spotlight = 1 - smoothstep(light.innerAngle, light.outerAngle, directionAngle);

            if (spotlight == 0)
                return;
        }
    }
    else
    {
        return;
    }

    // Irradiance of a disk light over 2*pi: radiance * (1 - cos(halfAngularSize)),
    // with 1 - cos(x) approximated by x^2 / 2.
    float irradiance = light.radiance * 0.5 * square(halfAngularSize) * attenuation * spotlight;

    o_diffuseRadiance = irradiance * Lambert(surface.normal, incidentVector);
    o_specularRadiance = o_diffuseRadiance * GGX(surface, incidentVector, viewIncident, halfAngularSize) * (2 * M_PI);
}

float GetLightProbeWeight(LightProbeConstants lightProbe, float3 position)
{
    float weight = 1;

    [unroll]
    for (uint nPlane = 0; nPlane < 6; nPlane++)
    {
        float4 plane = lightProbe.frustumPlanes[nPlane];

        float planeResult = plane.w - dot(plane.xyz, position.xyz);

        weight *= saturate(planeResult);
    }

    return weight;
}

float sharpen(float x, float sharpening)
{
    if (x < 0.5)
        return 0.5 * pow(2.0 * x, sharpening);
    else
        return -0.5 * pow(-2.0 * x + 2.0, sharpening) + 1.0;
}

float4 ShadowWithMaxDistance(float4 shadowMapValues, float reference, float maxDistance)
{
    // deviation: the donut source reads
    //   1.0 - (reference > shadowMapValues) * (maxDistance == 0 ? 1 : saturate((shadowMapValues + maxDistance - reference) * 10))
    // but the 2018 compiler (dxc 1.2) typed that conditional as a scalar and only kept the .x
    // component of the falloff, which is what the shipped shaders do. Reproduced explicitly.
    float falloff = (maxDistance == 0) ? 1 : saturate((shadowMapValues.x + maxDistance - reference) * 10);
    return 1.0 - (reference > shadowMapValues) * falloff;
}

float2 EvaluateShadowGather16(Texture2DArray ShadowMapArray, SamplerState ShadowSampler, ShadowConstants shadowParams, float3 worldPos, float2 shadowMapTextureSize)
{
    float4 uvzwShadow = mul(float4(worldPos, 1), shadowParams.matWorldToUvzwShadow);

    if (uvzwShadow.w <= 0)
        return 0;

    float3 uvzShadow = uvzwShadow.xyz / uvzwShadow.w;

    if (shadowParams.shadowFalloffDistance == 0)
        uvzShadow.z = min(uvzShadow.z, 0.999999);

    float2 fadeUV = saturate(abs(uvzShadow.xy - shadowParams.shadowMapCenterUV) * shadowParams.shadowFadeScale + shadowParams.shadowFadeBias);
    float fade = fadeUV.x * fadeUV.y;

    if (shadowParams.shadowFalloffDistance > 0)
        fade *= saturate((1.0 - uvzShadow.z) * 10);

    if (fade == 0)
        return 0;

    float3 sampleLocation = float3(uvzShadow.xy, shadowParams.shadowMapArrayIndex);

    // Do the samples - each one a 2x2 GatherCmp
    float4 samplesNW = ShadowMapArray.GatherRed(ShadowSampler, sampleLocation, int2(-1, -1));
    float4 samplesNE = ShadowMapArray.GatherRed(ShadowSampler, sampleLocation, int2(1, -1));
    float4 samplesSW = ShadowMapArray.GatherRed(ShadowSampler, sampleLocation, int2(-1, 1));
    float4 samplesSE = ShadowMapArray.GatherRed(ShadowSampler, sampleLocation, int2(1, 1));

    samplesNW = ShadowWithMaxDistance(samplesNW, uvzShadow.z, shadowParams.shadowFalloffDistance);
    samplesNE = ShadowWithMaxDistance(samplesNE, uvzShadow.z, shadowParams.shadowFalloffDistance);
    samplesSW = ShadowWithMaxDistance(samplesSW, uvzShadow.z, shadowParams.shadowFalloffDistance);
    samplesSE = ShadowWithMaxDistance(samplesSE, uvzShadow.z, shadowParams.shadowFalloffDistance);

    // Calculate fractional location relative to texel centers.  The 1/512 offset is needed to ensure
    // that frac()'s output steps from 1 to 0 at the exact same point that GatherCmp switches texels.
    float2 offset = frac(uvzShadow.xy * shadowMapTextureSize + (-0.5 + 1.0 / 512.0));

    // Calculate weights for the samples based on a 2px-radius biquadratic filter
    static const float radius = 2.0;
    float4 xOffsets = offset.x + float4(1, 0, -1, -2);
    float4 yOffsets = offset.y + float4(1, 0, -1, -2);

    // Readable version: xWeights = max(0, 1 - x^2/r^2) for x in xOffsets
    float4 xWeights = saturate(xOffsets * xOffsets * (-1.0 / (radius * radius)) + 1.0);
    float4 yWeights = saturate(yOffsets * yOffsets * (-1.0 / (radius * radius)) + 1.0);

    // Calculate weighted sum of samples
    float sampleSum = dot(xWeights.xyyx, yWeights.yyxx * samplesNW) +
        dot(xWeights.zwwz, yWeights.yyxx * samplesNE) +
        dot(xWeights.xyyx, yWeights.wwzz * samplesSW) +
        dot(xWeights.zwwz, yWeights.wwzz * samplesSE);

    float weightSum = dot(xWeights.xyyx, yWeights.yyxx) +
        dot(xWeights.zwwz, yWeights.yyxx) +
        dot(xWeights.xyyx, yWeights.wwzz) +
        dot(xWeights.zwwz, yWeights.wwzz);

    const float shadowSharpening = 1.0;
    float shadow = sharpen(saturate(sampleSum / weightSum), shadowSharpening);
    return float2(shadow * fade, fade);
}

static const float2 g_ShadowSamplePositions[] = {

    // Poisson disk with 16 points : 0 - 15
    float2(-0.3935238f, 0.7530643f),
    float2(-0.3022015f, 0.297664f),
    float2(0.09813362f, 0.192451f),
    float2(-0.7593753f, 0.518795f),
    float2(0.2293134f, 0.7607011f),
    float2(0.6505286f, 0.6297367f),
    float2(0.5322764f, 0.2350069f),
    float2(0.8581018f, -0.01624052f),
    float2(-0.6928226f, 0.07119545f),
    float2(-0.3114384f, -0.3017288f),
    float2(0.2837671f, -0.179743f),
    float2(-0.3093514f, -0.749256f),
    float2(-0.7386893f, -0.5215692f),
    float2(0.3988827f, -0.617012f),
    float2(0.8114883f, -0.458026f),
    float2(0.08265103f, -0.8939569f),

};

float2 EvaluateShadowPoisson(Texture2DArray ShadowMapArray, SamplerComparisonState ShadowSampler, ShadowConstants shadowParams, float3 worldPos, float2 sincosRotationAngle, float diskSizeTexels)
{
    float4 uvzwShadow = mul(float4(worldPos, 1), shadowParams.matWorldToUvzwShadow);

    if (uvzwShadow.w <= 0)
        return 0;

    float3 uvzShadow = uvzwShadow.xyz / uvzwShadow.w;

    if (shadowParams.shadowFalloffDistance == 0)
        uvzShadow.z = min(uvzShadow.z, 0.999999);

    float2 fadeUV = saturate(abs(uvzShadow.xy - shadowParams.shadowMapCenterUV) * shadowParams.shadowFadeScale + shadowParams.shadowFadeBias);
    float fade = fadeUV.x * fadeUV.y;

    if (shadowParams.shadowFalloffDistance > 0)
        fade *= saturate((1.0 - uvzShadow.z) * 10);

    if (fade == 0)
        return 0;

    float shadow = 0;

    [unroll]
    for (uint nSample = 0; nSample < 16; ++nSample)
    {
        float2 offset = g_ShadowSamplePositions[nSample];
        offset = float2(
            offset.x * sincosRotationAngle.x - offset.y * sincosRotationAngle.y,
            offset.x * sincosRotationAngle.y + offset.y * sincosRotationAngle.x
            );

        offset *= shadowParams.shadowMapSizeTexelsInv * diskSizeTexels;

        float shadowSample = ShadowMapArray.SampleCmpLevelZero(
            ShadowSampler,
            float3(uvzShadow.xy + offset.xy, shadowParams.shadowMapArrayIndex),
            uvzShadow.z);
        shadow += shadowSample;
    }

    shadow /= 16;
    return float2(shadow * fade, fade);
}

#endif // LIGHTING_2018_HLSLI
