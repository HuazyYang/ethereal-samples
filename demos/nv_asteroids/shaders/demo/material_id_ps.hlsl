/*
* Asteroids (2018) - material ID pass pixel shader.
*
* Writes g_Material.materialID into an R32_UINT target. Asteroids (_ASTEROIDS=1, fed by
* asteroidMS) apply the dithered LOD cross-fade first; ship / scene geometry is fed by the
* scene vertex shader. IS_SHIP does not change the code of this shader.
*
* Permutations: IS_SHIP={0,1} _ASTEROIDS={0,1}
*/

#pragma pack_matrix(row_major)

#include "scene_material_2018.hlsli"

#if _ASTEROIDS
#define ASTEROID_MATERIAL_USE_DITHER 1
#define ASTEROID_MATERIAL_DITHER_ONLY 1
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
    out uint o_materialID : SV_Target0)
{
#if _ASTEROIDS
    LodTransitionDither(i_position, i_alphaLod);
#endif

    o_materialID = g_Material.materialID;
}
