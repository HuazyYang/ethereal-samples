/*
* Asteroids (2018 donut framework) - material ID pixel shader (passes/material_id_ps.hlsl).
*
* Writes g_Material.materialID to an R32_UINT target, for picking.
* Entry points:
*   main               - opaque geometry
*   main_alpha_tested  - evaluates the material opacity first and clips below 0.5
* No permutations. The demo's own demo/material_id_ps.hlsl is the meshlet/ship variant.
*/

#pragma pack_matrix(row_major)

#include "scene_material_2018.hlsli"

void main(
    in float4 i_position : SV_Position,
    in SceneVertex i_vtx,
    out uint o_materialID : SV_Target0)
{
    o_materialID = g_Material.materialID;
}

void main_alpha_tested(
    in float4 i_position : SV_Position,
    in SceneVertex i_vtx,
    out uint o_materialID : SV_Target0)
{
    SurfaceParams surface = EvaluateSceneMaterial(i_vtx);

    clip(surface.opacity - 0.5);

    o_materialID = g_Material.materialID;
}
