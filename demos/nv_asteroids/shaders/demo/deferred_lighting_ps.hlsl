/*
* Asteroids (2018) - deferred lighting pixel shader (full-screen pass over the G-buffer).
*
* Same lighting model as forward_ps: analytic lights with cascaded shadow maps (16-tap rotated
* Poisson PCF, rotation from a 4x4 noise pattern), per-object gather-filtered shadows, light
* probes, hemispherical ambient and an optional indirect diffuse buffer. Alpha is written as 0.
*
* G-buffer layout (see gbuffer_ps):
*   t_GBuffer0 = (diffuse albedo, opacity)
*   t_GBuffer1 = (specular color, 0) or (emissive color, 1)
*   t_GBuffer2 = (normal, roughness)
*
* No permutations.
*/

#pragma pack_matrix(row_major)

#include "surface_cb.h"
#include "surface_2018.hlsli"

cbuffer c_Deferred : register(b0)
{
    DeferredLightingConstants g_Deferred;
};

Texture2D t_GBufferDepth : register(t8);
Texture2D t_GBuffer0 : register(t9);
Texture2D t_GBuffer1 : register(t10);
Texture2D t_GBuffer2 : register(t11);

Texture2DArray t_ShadowMapArray : register(t0);
TextureCubeArray t_DiffuseLightProbe : register(t1);
TextureCubeArray t_SpecularLightProbe : register(t2);
Texture2D t_EnvironmentBrdf : register(t3);
Texture2D t_IndirectDiffuse : register(t4);

SamplerState s_ShadowSampler : register(s0);
SamplerComparisonState s_ShadowSamplerComparison : register(s1);
SamplerState s_LightProbeSampler : register(s2);
SamplerState s_BrdfSampler : register(s3);

#include "lighting_2018.hlsli"

SurfaceParams GetSurfaceParams(int2 pixelPos, float2 uv)
{
    SurfaceParams surface = (SurfaceParams)0;

    float depth = t_GBufferDepth.Load(int3(pixelPos, 0)).x;
    float4 clipPos = float4(uv.x * 2 - 1, 1 - uv.y * 2, depth, 1);

    float4 gbuffer0 = t_GBuffer0.Load(int3(pixelPos, 0));
    float4 gbuffer1 = t_GBuffer1.Load(int3(pixelPos, 0));
    float4 gbuffer2 = t_GBuffer2.Load(int3(pixelPos, 0));

    surface.diffuseColor = gbuffer0.rgb;
    surface.opacity = gbuffer0.a;

    if (gbuffer1.w == 0)
        surface.specularColor = gbuffer1.rgb;
    else
        surface.emissiveColor = gbuffer1.rgb;

    surface.normal = gbuffer2.xyz;
    surface.roughness = gbuffer2.w;

    float4 viewPos = mul(clipPos, g_Deferred.matClipToView);
    viewPos.xyz /= viewPos.w;
    surface.worldPos = mul(float4(viewPos.xyz, 1), g_Deferred.matViewToWorld).xyz;

    return surface;
}

void main(
    in float4 i_position : SV_Position,
    in float2 i_uv : UV,
    out float4 o_color : SV_Target0)
{
    int2 pixelPos = int2(i_position.xy);
    SurfaceParams surface = GetSurfaceParams(pixelPos, i_uv);

    float3 viewIncident = GetIncidentVector(g_Deferred.cameraDirectionOrPosition, surface.worldPos);

    // Per-pixel rotation of the Poisson shadow kernel.
    int2 noisePos = int2(i_position.xy + g_Deferred.randomOffset);
    float angle = g_Deferred.noisePattern[noisePos.y & 3][noisePos.x & 3];
    float2 sincosAngle;
    sincos(angle, sincosAngle.x, sincosAngle.y);

    float3 diffuseTerm = 0;
    float3 specularTerm = 0;

    [loop]
    for (uint nLight = 0; nLight < g_Deferred.numLights; nLight++)
    {
        LightConstants light = g_Deferred.lights[nLight];

        int4 cascadeIndices = light.shadowCascades;
        float2 shadow = 0;
        for (int cascade = 0; cascade < 4; cascade++)
        {
            if (cascadeIndices[cascade] >= 0)
            {
                float2 cascadeShadow = EvaluateShadowPoisson(t_ShadowMapArray, s_ShadowSamplerComparison, g_Deferred.shadows[cascadeIndices[cascade]], surface.worldPos, sincosAngle, 3.0);

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
            if (light.perObjectShadows[object] >= 0)
            {
                float2 thisObjectShadow = EvaluateShadowGather16(t_ShadowMapArray, s_ShadowSampler, g_Deferred.shadows[light.perObjectShadows[object]], surface.worldPos, g_Deferred.shadowMapTextureSize);

                objectShadow *= saturate(thisObjectShadow.x + (1 - thisObjectShadow.y));
            }
        }

        shadow.x *= objectShadow;

        float diffuseRadiance, specularRadiance;
        ShadeSurface(light, surface, viewIncident, diffuseRadiance, specularRadiance);

        diffuseTerm += (shadow.x * diffuseRadiance) * light.color;
        specularTerm += (shadow.x * specularRadiance) * light.color * surface.specularColor;
    }

    if (g_Deferred.numLightProbes > 0)
    {
        float3 N = surface.normal;
        float3 R = reflect(viewIncident, N);
        float NdotV = saturate(-dot(N, viewIncident));

        float2 environmentBrdf = t_EnvironmentBrdf.SampleLevel(s_BrdfSampler, float2(NdotV, surface.roughness), 0).xy;

        float lightProbeWeight = 0;
        float3 lightProbeDiffuse = 0;
        float3 lightProbeSpecular = 0;

        [loop]
        for (uint nProbe = 0; nProbe < g_Deferred.numLightProbes; nProbe++)
        {
            LightProbeConstants lightProbe = g_Deferred.lightProbes[nProbe];

            float weight = GetLightProbeWeight(lightProbe, surface.worldPos);

            if (weight == 0)
                continue;

            // The probe cube maps are stored with a flipped Z axis.
            float specularMipLevel = sqrt(saturate(surface.roughness)) * (lightProbe.mipLevels - 1);
            float3 diffuseProbe = t_DiffuseLightProbe.SampleLevel(s_LightProbeSampler, float4(N.xy, -N.z, lightProbe.diffuseArrayIndex), 0).rgb;
            float3 specularProbe = t_SpecularLightProbe.SampleLevel(s_LightProbeSampler, float4(R.xy, -R.z, lightProbe.specularArrayIndex), specularMipLevel).rgb;

            lightProbeDiffuse += (weight * lightProbe.diffuseScale) * diffuseProbe;
            lightProbeSpecular += (weight * lightProbe.specularScale) * specularProbe;
            lightProbeWeight += weight;
        }

        if (lightProbeWeight > 1)
        {
            float invWeight = rcp(lightProbeWeight);
            lightProbeDiffuse *= invWeight;
            lightProbeSpecular *= invWeight;
        }

        diffuseTerm += lightProbeDiffuse;
        specularTerm += lightProbeSpecular * (surface.specularColor * environmentBrdf.x + environmentBrdf.y);
    }

    {
        float3 ambientColor = lerp(g_Deferred.ambientColorBottom.rgb, g_Deferred.ambientColorTop.rgb, surface.normal.y * 0.5 + 0.5);

        diffuseTerm += ambientColor;
        specularTerm += ambientColor * surface.specularColor;
    }

    if (g_Deferred.indirectDiffuseScale > 0)
    {
        diffuseTerm += g_Deferred.indirectDiffuseScale * t_IndirectDiffuse.Load(int3(pixelPos, 0)).rgb;
    }

    o_color.rgb = diffuseTerm * surface.diffuseColor + specularTerm + surface.emissiveColor;
    o_color.a = 0;
}
