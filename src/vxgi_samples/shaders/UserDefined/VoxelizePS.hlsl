#pragma pack_matrix(row_major)

#include <donut/shaders/forward_cb.h>

// Declare the constants that drive material bindings in 'material_bindings.hlsli'
// to match the bindings explicitly declared in 'forward_cb.h'.

#define MATERIAL_REGISTER_SPACE FORWARD_SPACE_MATERIAL
#define MATERIAL_CB_SLOT FORWARD_BINDING_MATERIAL_CONSTANTS
#define MATERIAL_DIFFUSE_SLOT FORWARD_BINDING_MATERIAL_DIFFUSE_TEXTURE
#define MATERIAL_SPECULAR_SLOT FORWARD_BINDING_MATERIAL_SPECULAR_TEXTURE
#define MATERIAL_NORMALS_SLOT FORWARD_BINDING_MATERIAL_NORMAL_TEXTURE
#define MATERIAL_EMISSIVE_SLOT FORWARD_BINDING_MATERIAL_EMISSIVE_TEXTURE
#define MATERIAL_OCCLUSION_SLOT FORWARD_BINDING_MATERIAL_OCCLUSION_TEXTURE
#define MATERIAL_TRANSMISSION_SLOT FORWARD_BINDING_MATERIAL_TRANSMISSION_TEXTURE
#define MATERIAL_OPACITY_SLOT FORWARD_BINDING_MATERIAL_OPACITY_TEXTURE

#define MATERIAL_SAMPLER_REGISTER_SPACE FORWARD_SPACE_SHADING
#define MATERIAL_SAMPLER_SLOT FORWARD_BINDING_MATERIAL_SAMPLER

#include <donut/shaders/scene_material.hlsli>
#include <donut/shaders/material_bindings.hlsli>
#include <donut/shaders/forward_vertex.hlsli>
#include <donut/shaders/lighting.hlsli>
#include <donut/shaders/shadows.hlsli>
#include <donut/shaders/binding_helpers.hlsli>

DECLARE_CBUFFER(ForwardShadingViewConstants, g_ForwardView, FORWARD_BINDING_VIEW_CONSTANTS, FORWARD_SPACE_VIEW);
DECLARE_CBUFFER(ForwardShadingLightConstants, g_ForwardLight, FORWARD_BINDING_LIGHT_CONSTANTS, FORWARD_SPACE_SHADING);

Texture2DArray t_ShadowMapArray : REGISTER_SRV(FORWARD_BINDING_SHADOW_MAP_TEXTURE, FORWARD_SPACE_SHADING);

SamplerComparisonState s_ShadowSampler : REGISTER_SAMPLER(FORWARD_BINDING_SHADOW_MAP_SAMPLER, FORWARD_SPACE_SHADING);

#if VXGI_PS_TYPE == 0
#define COVERAGE_WITH_EDGE_EQUATIONS 0
#include "VoxelizeOpacityPS.hlsli"
#else
#if VXGI_PS_TYPE == 1
#define COVERAGE_WITH_EDGE_EQUATIONS 0
#elif VXGI_PS_TYPE == 2
#define COVERAGE_WITH_EDGE_EQUATIONS 1
#endif
#include "VoxelizeEmittancePS.hlsli"
#endif

struct PSInput {
    float3 positionWS : POS;
    float2 texCoord : TEXCOORD;
    float3 normal : NORMAL;
    float4 tangent : TANGENT;
    VxgiVoxelizationPSInputData vxgiData;
};

static const float PI = 3.14159265;

float GetShadowFast(float3 fragmentPos) {
    float4 clipPos = mul(float4(fragmentPos, 1.0), g_ForwardLight.shadows[0].matWorldToUvzwShadow);
    clipPos.xyz /= clipPos.w;

    if (any(clipPos.xyz < 0.0.xxx) || any(clipPos.xyz > 1.0.xxx))
        return 0.0;

    return t_ShadowMapArray.SampleCmpLevelZero(s_ShadowSampler, float3(clipPos.xy, 0.0), clipPos.z);
}

void main(PSInput IN)
{
    if (VxgiIsEmissiveVoxelizationPass) {
        float3 worldPos = IN.positionWS;
        float3 normal = normalize(IN.normal);
        float3 albedo = g_Material.baseOrDiffuseColor.rgb;
        if (g_Material.opacity > 0)
            albedo = t_BaseOrDiffuse.Sample(s_MaterialSampler, IN.texCoord).rgb;

        LightConstants light = g_ForwardLight.lights[0];

        float NdotL = saturate(-dot(normal, light.direction));
        float3 radiosity = 0.f;
        if (NdotL > 0.f) {
            // float2 shadow = 0;
            // for (int cascade = 0; cascade < 4; ++cascade) {
            //     if (light.shadowCascades[cascade] >= 0) {
            //         float2 cascadeShadow = EvaluateShadowGather16(t_ShadowMapArray, s_ShadowSampler, g_ForwardLight.shadows[light.shadowCascades[cascade]], worldPos, g_ForwardLight.shadowMapTextureSize);

            //         shadow = saturate(shadow + cascadeShadow * (1.0001 - shadow.y));

            //         if (shadow.y == 1)
            //             break;
            //     }
            // }

            // shadow.x += (1 - shadow.y) * light.outOfBoundsShadow;

            // radiosity += albedo.rgb * light.color.rgb * (NdotL * shadow.x);
            float shadow = GetShadowFast(worldPos);
            radiosity += albedo.rgb * light.color.rgb * (NdotL * shadow);
        }

        radiosity += albedo.rgb * VxgiGetIndirectIrradiance(worldPos, normal) / PI;

        VxgiStoreVoxelizationData(IN.vxgiData, radiosity);
    } else {
        VxgiStoreVoxelizationData(IN.vxgiData, 0);
    }
};