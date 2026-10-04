/*
* Asteroids (2018 donut framework) - vertex attributes shared by the framework geometry passes
* (passes/gbuffer_vs, forward_vs, forward_gs, cubemap_gs).
*
* This is the same SceneVertex as in demo/include/scene_material_2018.hlsli (the demo pixel
* shaders consume it). Do not include both headers in one shader; the pixel shaders that need
* material evaluation include scene_material_2018.hlsli instead.
*/

#ifndef FORWARD_VERTEX_2018_HLSLI
#define FORWARD_VERTEX_2018_HLSLI

struct SceneVertex
{
    float3 m_pos : POS;
    float2 m_uv : UV;
    centroid float3 m_normal : NORMAL;
    centroid float3 m_tangent : TANGENT;
    centroid float3 m_bitangent : BITANGENT;
};

#endif // FORWARD_VERTEX_2018_HLSLI
