/*
* Asteroids (2018) - G-buffer fill pixel shader.
*
* Outputs:
*   SV_Target0 = (diffuse albedo, opacity)
*   SV_Target1 = (emissive color, 1) if the surface is emissive, else (specular color, 0)
*   SV_Target2 = (shading normal, roughness)
*   SV_Target3 = screen-space motion vector (pixels)
*
* Permutations: IS_SHIP={0,1} _ASTEROIDS={0,1}
*   _ASTEROIDS=1: input from asteroidMS (ATTR1/ATTR2/ALPHA_LOD), procedural asteroid material,
*                 dithered LOD transition, distance fade and LOD visualization.
*   _ASTEROIDS=0: input from the scene vertex shader (SceneVertex + PREV_WORLD_POS).
*   IS_SHIP=1   : scales the emissive output of three texture regions of the ship (engines and
*                 lights) by per-object intensities passed through cbObjectInfo.
*/

#pragma pack_matrix(row_major)

#include "scene_material_2018.hlsli"
#include "meshlet_cb.h"

cbuffer c_GBuffer : register(b1)
{
    GBufferFillConstants c_GBuffer;
};

#if _ASTEROIDS
cbuffer cbFrame : register(b0, space1)
{
    FrameCB cbFrame;
};
#endif

#if IS_SHIP
cbuffer cbObjectInfo : register(b4, space1)
{
    ObjectConstants cbObjectInfo;
};
#endif

#if _ASTEROIDS
#define ASTEROID_MATERIAL_USE_DITHER 1
#include "asteroid_material.hlsli"
#endif

float2 GetMotionVector(float2 svPosition, float3 prevWorldPos)
{
    float4 clipPos = mul(float4(prevWorldPos, 1), c_GBuffer.matWorldToClipPrev);
    if (clipPos.w <= 0)
        return 0;

    clipPos.xyz /= clipPos.w;
    float2 prevWindowPos = clipPos.xy * c_GBuffer.viewportScalePrev + c_GBuffer.viewportBiasPrev;

    return prevWindowPos - svPosition + c_GBuffer.pixelOffset;
}

#if IS_SHIP
bool IsInTextureRect(float2 uv, float2 rectMin, float2 rectMax)
{
    return all(uv >= rectMin) && all(uv < rectMax);
}

// The ship's emissive texture has three areas with separately controlled brightness
// (coordinates are texels of the 4096x4096 ship texture).
// The C++ side (MeshletDrawStrategy, 2018 0x140042400) repurposes ObjectConstants for the ship:
// lodBias = engine glow, center.x / center.y = nav-light blink states (SpaceObject::shipGlowParams.xyz).
float3 GetShipEmissive(float3 emissiveColor, float2 uv)
{
    if (IsInTextureRect(uv, float2(0, 0), float2(1008, 1008) / 4096.0))
        emissiveColor *= cbObjectInfo.lodBias;
    else if (IsInTextureRect(uv, float2(211, 2447) / 4096.0, float2(245, 2457) / 4096.0))
        emissiveColor *= cbObjectInfo.center.x;
    else if (IsInTextureRect(uv, float2(3898, 1207) / 4096.0, float2(3915, 1237) / 4096.0))
        emissiveColor *= cbObjectInfo.center.y;

    return emissiveColor;
}
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
    out float4 o_channel0 : SV_Target0,
    out float4 o_channel1 : SV_Target1,
    out float4 o_channel2 : SV_Target2,
    out float2 o_motion : SV_Target3)
{
#if _ASTEROIDS
    float random = LodTransitionDither(i_position, i_alphaLod);

    if (cbFrame.enableDistanceLod != 0 && random > i_alphaLod.z)
        discard;

    float2 uv = float2(i_attr1.w, i_attr2.w);
    SurfaceParams surface = EvaluateAsteroidsMaterial(i_attr1.xyz, uv, i_attr2.xyz);

    // Asteroids are static: the previous position only differs by the camera translation.
    float3 prevWorldPos = i_attr1.xyz + (cbFrame.preViewTranslationPrevious.xyz - cbFrame.preViewTranslation.xyz);
#else
    float2 uv = i_vtx.m_uv;
    SurfaceParams surface = EvaluateSceneMaterial(i_vtx);
    float3 prevWorldPos = i_prevWorldPos;
#endif

    clip(surface.opacity - 0.5);

    o_channel0 = float4(surface.diffuseColor, surface.opacity);

#if _ASTEROIDS
    if (cbFrame.visualizeLods)
    {
        static const float3 palette[10] = {
            float3(0.0, 0.0, 0.0),
            float3(0.8, 0.0, 0.0),
            float3(1.0, 0.5, 0.0),
            float3(1.0, 1.0, 0.0),
            float3(0.0, 1.0, 0.0),
            float3(0.0, 0.75, 0.25),
            float3(0.0, 0.0, 1.0),
            float3(0.5, 0.0, 1.0),
            float3(0.5, 0.5, 1.0),
            float3(1.0, 1.0, 1.0)
        };

        float lod;
        if (i_alphaLod.y != 0)
            lod = ceil(i_alphaLod.w);
        else
            lod = floor(i_alphaLod.w);

        o_channel0.rgb = palette[clamp(uint(lod), 0, 9)];
    }
#endif

    // Emissive surfaces store their emissive color instead of the specular color.
    float4 specularOrEmissive;
    if (any(surface.emissiveColor > 0))
    {
        float3 emissiveColor = surface.emissiveColor;
#if IS_SHIP
        emissiveColor = GetShipEmissive(emissiveColor, uv);
#endif
        specularOrEmissive = float4(emissiveColor, 1);
    }
    else
        specularOrEmissive = float4(surface.specularColor, 0);

    o_channel1 = specularOrEmissive;

    o_channel2 = float4(surface.normal, surface.roughness);
    o_motion = GetMotionVector(i_position.xy, prevWorldPos);
}
