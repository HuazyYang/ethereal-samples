/*
* Asteroids (2018) - scene material evaluation (early donut "scene_material.hlsli").
*
* Reconstructed from the inlined EvaluateSceneMaterial / RgbToNormal code in forward_ps,
* gbuffer_ps and material_id_ps. Declares the material bindings shared by all surface
* shaders (c_Material b0, t_Diffuse..t_Emissive t0..t3, s_MaterialSampler s0).
*/

#ifndef SCENE_MATERIAL_2018_HLSLI
#define SCENE_MATERIAL_2018_HLSLI

#include "surface_cb.h"
#include "surface_2018.hlsli"

cbuffer c_Material : register(b0)
{
    MaterialConstants g_Material;
};

Texture2D t_Diffuse : register(t0);
Texture2D t_Specular : register(t1);
Texture2D t_Normals : register(t2);
Texture2D t_Emissive : register(t3);
SamplerState s_MaterialSampler : register(s0);

// Vertex attributes written by the scene vertex shader (ship / non-meshlet geometry).
struct SceneVertex
{
    float3 m_pos : POS;
    float2 m_uv : UV;
    centroid float3 m_normal : NORMAL;
    centroid float3 m_tangent : TANGENT;
    centroid float3 m_bitangent : BITANGENT;
};

// Unpacks a 3-channel normal map texel; 'len' receives the length of the unpacked vector,
// which is < 1 where the mip chain averaged diverging normals.
float3 RgbToNormal(float3 rgb, out float len)
{
    float3 n = rgb * 2.0 - 1.0;
    len = length(n);
    return (len > 0) ? n / len : 0;
}

// Toksvig-style specular anti-aliasing: widen the lobe where the averaged normal is short.
// unresolved: the original variable names are unknown, the arithmetic is exact.
float AdjustRoughnessToksvig(float roughness, float normalLength)
{
    float specularPower = 4.0 / square(roughness) - 4.0;
    float toksvigFactor = max(normalLength / lerp(specularPower, 1.0, normalLength), 0.01);
    return sqrt(2.0 / (specularPower * 0.5 * toksvigFactor + 2.0));
}

SurfaceParams EvaluateSceneMaterial(SceneVertex vertex)
{
    SurfaceParams surface = (SurfaceParams)0;

    surface.worldPos = vertex.m_pos;

    float normalLength = length(vertex.m_normal);
    if (normalLength == 0)
        surface.geometryNormal = normalize(cross(ddy(vertex.m_pos), ddx(vertex.m_pos)));
    else
        surface.geometryNormal = vertex.m_normal / normalLength;

    surface.normal = surface.geometryNormal;
    surface.diffuseColor = g_Material.diffuseColor;
    surface.specularColor = g_Material.specularColor;
    surface.emissiveColor = g_Material.emissiveColor;
    surface.opacity = g_Material.opacity;
    surface.roughness = g_Material.roughness;

    if (g_Material.useDiffuseTexture)
    {
        float4 diffuseTextureValue = t_Diffuse.Sample(s_MaterialSampler, vertex.m_uv);
        surface.diffuseColor *= diffuseTextureValue.rgb;
        surface.opacity *= diffuseTextureValue.a;
    }

    if (g_Material.specularTextureType != SPECULAR_TEXTURE_NONE)
    {
        float4 specularTextureValue = t_Specular.Sample(s_MaterialSampler, vertex.m_uv);

        if (g_Material.specularTextureType == SPECULAR_TEXTURE_INTENSITY)
        {
            surface.specularColor *= specularTextureValue.r;
            surface.diffuseColor *= 1.0 - surface.specularColor;
        }
        else if (g_Material.specularTextureType == SPECULAR_TEXTURE_SPECULAR_GLOSS)
        {
            surface.specularColor *= specularTextureValue.rgb;

            if (specularTextureValue.a > 0)
                surface.roughness = 1.0 - specularTextureValue.a * (1.0 - surface.roughness);

            surface.diffuseColor *= 1.0 - surface.specularColor;
        }
        else if (g_Material.specularTextureType == SPECULAR_TEXTURE_ORM)
        {
            float metalness = specularTextureValue.b;
            surface.specularColor = lerp(0.04, surface.diffuseColor, metalness);
            surface.diffuseColor *= 1.0 - metalness;
            surface.roughness = 1.0 - (1.0 - specularTextureValue.g) * (1.0 - surface.roughness);
        }
    }
    else
    {
        surface.diffuseColor *= 1.0 - surface.specularColor;
    }

    if (g_Material.useEmissiveTexture)
    {
        surface.emissiveColor *= t_Emissive.Sample(s_MaterialSampler, vertex.m_uv).rgb;
    }

    if (g_Material.useNormalsTexture)
    {
        float4 normalsTextureValue = t_Normals.Sample(s_MaterialSampler, vertex.m_uv);

        float3 localNormal;
        if (normalsTextureValue.z > 0)
        {
            // 3-channel normal map
            float normalMapLength;
            localNormal = RgbToNormal(normalsTextureValue.rgb, normalMapLength);

            if (normalMapLength > 0)
                surface.roughness = AdjustRoughnessToksvig(surface.roughness, normalMapLength);
        }
        else
        {
            // 2-channel (BC5) normal map, Z reconstructed.
            // Note: the original reconstructs Z from the *unscaled* texel (0..1 range), kept as is.
            localNormal.xy = normalsTextureValue.xy * 2.0 - 1.0;
            localNormal.z = sqrt(1.0 - saturate(dot(normalsTextureValue.xy, normalsTextureValue.xy)));
            localNormal = normalize(localNormal);
        }

        surface.normal = normalize(localNormal.x * vertex.m_tangent + localNormal.y * vertex.m_bitangent + localNormal.z * vertex.m_normal);
    }

    return surface;
}

#endif // SCENE_MATERIAL_2018_HLSLI
