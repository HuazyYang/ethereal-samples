/*
* Asteroids (2018) - forward shading pixel shader.
*
* Analytic lights with cascaded + per-object gather-filtered shadow maps, light probes and a
* hemispherical ambient term. Materials with a non-zero emissive constant (the ship's engine
* exhaust) are rendered as animated additive glow instead.
*
* Permutations: IS_SHIP={0,1} _ASTEROIDS={0,1}
*   _ASTEROIDS=1: input from asteroidMS (ATTR1/ATTR2/ALPHA_LOD), procedural asteroid material.
*   _ASTEROIDS=0: input from the scene vertex shader (SceneVertex + PREV_WORLD_POS).
*   IS_SHIP does not change the code of this shader.
*/

#pragma pack_matrix(row_major)

#include "scene_material_2018.hlsli"
#include "meshlet_cb.h"

cbuffer cbFrame : register(b0, space1)
{
    FrameCB cbFrame;
};

cbuffer c_Forward : register(b1)
{
    ForwardShadingConstants g_Forward;
};

Texture2DArray t_ShadowMapArray : register(t4);
TextureCubeArray t_DiffuseLightProbe : register(t5);
TextureCubeArray t_SpecularLightProbe : register(t6);
Texture2D t_EnvironmentBrdf : register(t7);

SamplerState s_ShadowSampler : register(s1);
SamplerState s_LightProbeSampler : register(s2);
SamplerState s_BrdfSampler : register(s3);

#include "lighting_2018.hlsli"

#if _ASTEROIDS
#include "asteroid_material.hlsli"
#endif

void main(
    in float4 i_position : SV_Position,
#if _ASTEROIDS
    in float4 i_attr1 : ATTR1,
    in float4 i_attr2 : ATTR2,
    nointerpolation in float4 i_alphaLod : ALPHA_LOD,
#else
    in SceneVertex i_vtx,
    in float3 i_prevWorldPos : PREV_WORLD_POS,
#endif
    out float4 o_color : SV_Target0)
{
#if _ASTEROIDS
    float2 uv = float2(i_attr1.w, i_attr2.w);
    SurfaceParams surface = EvaluateAsteroidsMaterial(i_attr1.xyz, uv, i_attr2.xyz);
#else
    float2 uv = i_vtx.m_uv;
    SurfaceParams surface = EvaluateSceneMaterial(i_vtx);
#endif

    float3 viewIncident = GetIncidentVector(g_Forward.cameraDirectionOrPosition, surface.worldPos);

    float3 diffuseTerm = 0;
    float3 specularTerm = 0;

    [loop]
    for (uint nLight = 0; nLight < g_Forward.numLights; nLight++)
    {
        LightConstants light = g_Forward.lights[nLight];

        int4 cascadeIndices = light.shadowCascades;
        float2 shadow = 0;
        for (int cascade = 0; cascade < 4; cascade++)
        {
            if (cascadeIndices[cascade] >= 0)
            {
                float2 cascadeShadow = EvaluateShadowGather16(t_ShadowMapArray, s_ShadowSampler, g_Forward.shadows[cascadeIndices[cascade]], surface.worldPos, g_Forward.shadowMapTextureSize);

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
                float2 thisObjectShadow = EvaluateShadowGather16(t_ShadowMapArray, s_ShadowSampler, g_Forward.shadows[light.perObjectShadows[object]], surface.worldPos, g_Forward.shadowMapTextureSize);

                objectShadow *= saturate(thisObjectShadow.x + (1 - thisObjectShadow.y));
            }
        }

        shadow.x *= objectShadow;

        float diffuseRadiance, specularRadiance;
        ShadeSurface(light, surface, viewIncident, diffuseRadiance, specularRadiance);

        diffuseTerm += (shadow.x * diffuseRadiance) * light.color;
        specularTerm += (shadow.x * specularRadiance) * light.color * surface.specularColor;
    }

    if (g_Forward.numLightProbes > 0)
    {
        float3 N = surface.normal;
        float3 R = reflect(viewIncident, N);
        float NdotV = saturate(-dot(N, viewIncident));

        float2 environmentBrdf = t_EnvironmentBrdf.SampleLevel(s_BrdfSampler, float2(NdotV, surface.roughness), 0).xy;

        float lightProbeWeight = 0;
        float3 lightProbeDiffuse = 0;
        float3 lightProbeSpecular = 0;

        [loop]
        for (uint nProbe = 0; nProbe < g_Forward.numLightProbes; nProbe++)
        {
            LightProbeConstants lightProbe = g_Forward.lightProbes[nProbe];

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
        float3 ambientColor = lerp(g_Forward.ambientColorBottom.rgb, g_Forward.ambientColorTop.rgb, surface.normal.y * 0.5 + 0.5);

        diffuseTerm += ambientColor;
        specularTerm += ambientColor * surface.specularColor;
    }

    o_color.rgb = diffuseTerm * surface.diffuseColor + specularTerm + surface.emissiveColor;
    o_color.a = surface.opacity;

    // Emissive materials (engine exhaust): scrolling two-layer noise from the diffuse texture,
    // faded towards grazing angles, written for additive blending.
    if (any(g_Material.emissiveColor != 0))
    {
        float noise1 = t_Diffuse.Sample(s_MaterialSampler, float2(uv.x, uv.y * 0.5 - cbFrame.time * 1.1)).r;
        float noise2 = t_Diffuse.Sample(s_MaterialSampler, float2(uv.x - 0.25, uv.y * 0.7 - cbFrame.time * 0.5)).r;

        float NdotV = saturate(dot(surface.normal, -viewIncident));

        o_color.rgb = (o_color.rgb + surface.emissiveColor * 10) * (noise1 * noise2) * (NdotV * NdotV * surface.opacity * NdotV);
        o_color.a = 0;
    }
}
