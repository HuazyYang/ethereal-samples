/*
* Asteroids (2018) - SurfaceParams, the shading input shared by the material evaluation
* (scene_material_2018.hlsli, asteroid_material.hlsli), the G-buffer decoding in
* deferred_lighting_ps and the lighting code (lighting_2018.hlsli).
* Layout taken from the type annotation in the reflection (struct size 116).
*/

#ifndef SURFACE_2018_HLSLI
#define SURFACE_2018_HLSLI

struct SurfaceParams
{
    float4 clipPos;         // not used by the shipped shaders (eliminated as dead code)
    float3 worldPos;        // camera-relative (translated) world position for asteroids
    float3 geometryNormal;
    float3 normal;          // shading normal
    float3 diffuseColor;    // diffuse albedo (already scaled by 1 - specular / metalness)
    float3 specularColor;   // specular F0
    float3 emissiveColor;
    float  opacity;
    float  roughness;
};

float square(float x)
{
    return x * x;
}

#endif // SURFACE_2018_HLSLI
