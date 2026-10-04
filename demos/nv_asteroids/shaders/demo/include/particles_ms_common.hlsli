/*
 * Shared part of the particle mesh shaders: ../particles_ms.hlsl (SM 6.5 port) and ../nvapi/particles.hlsl
 * (the 2018 NVAPI form, particles.hlsl:ms_main). Resources, PS_Input and the particle lighting / fading.
 */

#ifndef PARTICLES_MS_COMMON_HLSLI
#define PARTICLES_MS_COMMON_HLSLI

// ParticleConstants / ParticleInfo (and the 2018 LightConstants / ShadowConstants via light_cb.h)
// are shared with particles.hlsl (vs_main / ps_main / update_cs_main) and the C++ particle system.
#include "particles_cb.h"

#define PARTICLES_GROUP_SIZE 32     // particles per mesh group, one triangle each

cbuffer g_Particles : register(b0) { ParticleConstants g_Particles; };

Texture2DArray                  t_ShadowMapArray : register(t0);
StructuredBuffer<ParticleInfo>  t_Particles      : register(t1);
SamplerComparisonState          s_ShadowSampler  : register(s0);

// The 2018 NVAPI shader also declared g_NvidiaExt : register(u0) (see ../nvapi/particles.hlsl).

struct PS_Input
{
    float4 position : SV_Position;
    nointerpolation float4 color : COLOR;
    float2 uv : UV;
};

// Equilateral triangle circumscribing the unit circle (inradius 1, centroid at the origin).
static const float2 TrianglePoints[3] =
{
    float2( 0.0,        2.0),
    float2( 1.7320508, -1.0),
    float2(-1.7320508, -1.0)
};

float2 EvaluateShadowPCF(Texture2DArray shadowMapArray, SamplerComparisonState shadowSampler,
                         ShadowConstants shadow, float3 worldPos)
{
    float4 uvzw = mul(float4(worldPos, 1.0), shadow.matWorldToUvzwShadow);
    if (!(uvzw.w > 0))
        return 0;

    float3 uvz = uvzw.xyz / uvzw.w;

    float2 fadeXY = saturate(abs(uvz.xy - shadow.shadowMapCenterUV) * shadow.shadowFadeScale + shadow.shadowFadeBias);
    float fade = fadeXY.x * fadeXY.y;
    if (fade == 0)
        return 0;

    float visibility = shadowMapArray.SampleCmpLevelZero(shadowSampler,
        float3(uvz.xy, float(shadow.shadowMapArrayIndex)), min(uvz.z, 0.999999));

    return float2(visibility * fade, fade);
}

// x = accumulated shadow visibility, y = accumulated coverage of the cascades.
float EvaluateDirectionalLightShadow(float3 worldPos)
{
    int4 cascadeIndices = g_Particles.light.shadowCascades;
    float2 shadow = 0;

    for (int cascade = 0; cascade < 4; cascade++)
    {
        int shadowIndex = cascadeIndices[cascade];
        if (shadowIndex < 0)
            break;

        float2 cascadeShadow = EvaluateShadowPCF(t_ShadowMapArray, s_ShadowSampler, g_Particles.shadows[shadowIndex], worldPos);
        shadow = saturate(cascadeShadow * (1.0001 - shadow.y) + shadow);

        if (shadow.y == 1)
            break;
    }

    return shadow.x + (1.0 - shadow.y) * g_Particles.light.outOfBoundsShadow;
}

void SetupParticle(float3 position, float4 clipPos, float brightness, float distance,
                   out float2 size, out float4 color)
{
    // Clamp the clip-space size to a minimum of 0.3 and fade out what becomes sub-pixel.
    float clampedW = max(0.3, clipPos.w * 0.003);
    float sizeFade = pow(0.3 / clampedW, 1.5);
    float farFade = saturate((1.0 - distance / g_Particles.maxDistance) * 10.0);
    float nearFade = saturate(distance * 0.02 - 1.0);
    float alpha = farFade * sizeFade * nearFade;

    // Forward-scattering phase function (g = 0.9) plus an isotropic term.
    float cosTheta = dot(position / distance, -g_Particles.light.direction);
    float t = 0.100000024 / sqrt(max(1.81 - 1.8 * cosTheta, 0.0));   // (1 - 0.9f) / sqrt(1 + g^2 - 2 g cos)
    float phase = t * t * 82.5119705 * t + 0.795774698;

    float irradiance = g_Particles.light.radiance * 0.125 * g_Particles.light.angularSizeOrInvRange * g_Particles.light.angularSizeOrInvRange;
    float shadow = EvaluateDirectionalLightShadow(position);

    size = g_Particles.screenScale * clampedW;
    color.rgb = irradiance * brightness * g_Particles.light.color * (shadow * phase + 0.2);
    color.a = alpha;
}

#endif // PARTICLES_MS_COMMON_HLSLI
