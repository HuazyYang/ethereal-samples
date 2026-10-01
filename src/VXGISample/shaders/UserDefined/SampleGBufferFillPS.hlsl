#pragma pack_matrix(row_major)

#include <donut/shaders/gbuffer_cb.h>

// Declare the constants that drive material bindings in 'material_bindings.hlsli'
// to match the bindings explicitly declared in 'gbuffer_cb.h'.

#define MATERIAL_REGISTER_SPACE         GBUFFER_SPACE_MATERIAL
#define MATERIAL_CB_SLOT                GBUFFER_BINDING_MATERIAL_CONSTANTS
#define MATERIAL_DIFFUSE_SLOT           GBUFFER_BINDING_MATERIAL_DIFFUSE_TEXTURE
#define MATERIAL_SPECULAR_SLOT          GBUFFER_BINDING_MATERIAL_SPECULAR_TEXTURE
#define MATERIAL_NORMALS_SLOT           GBUFFER_BINDING_MATERIAL_NORMAL_TEXTURE
#define MATERIAL_EMISSIVE_SLOT          GBUFFER_BINDING_MATERIAL_EMISSIVE_TEXTURE
#define MATERIAL_OCCLUSION_SLOT         GBUFFER_BINDING_MATERIAL_OCCLUSION_TEXTURE
#define MATERIAL_TRANSMISSION_SLOT      GBUFFER_BINDING_MATERIAL_TRANSMISSION_TEXTURE
#define MATERIAL_OPACITY_SLOT           GBUFFER_BINDING_MATERIAL_OPACITY_TEXTURE

#define MATERIAL_SAMPLER_REGISTER_SPACE GBUFFER_SPACE_VIEW
#define MATERIAL_SAMPLER_SLOT           GBUFFER_BINDING_MATERIAL_SAMPLER

#include <donut/shaders/scene_material.hlsli>
#include <donut/shaders/material_bindings.hlsli>
#include <donut/shaders/motion_vectors.hlsli>
#include <donut/shaders/forward_vertex.hlsli>
#include <donut/shaders/binding_helpers.hlsli>

DECLARE_CBUFFER(GBufferFillConstants, c_GBuffer, GBUFFER_BINDING_VIEW_CONSTANTS, GBUFFER_SPACE_VIEW);

float2 GetFlipUV(float2 uv) {
    return float2(uv.x, 1.0 - uv.y);
}

float3 GetNormal(SceneVertex vtx) {

    float3 pixelNormal = t_Normal.Sample(s_MaterialSampler, GetFlipUV(vtx.texCoord)).xyz;
    float3 normal = normalize(vtx.normal);

    // A tangent of (0,0,0) with w == 0 is how the importers signal "no usable tangent
    // frame" for this vertex -- GltfImporter emits it for every vertex whose incident
    // triangles all have degenerate UVs. Normalizing that is 0/0, and the resulting NaN
    // propagates into the shading normal, which drives both NdotL and the VXGI cone
    // directions to garbage. Skip normal mapping instead, exactly as donut's own
    // ApplyNormalMap() in scene_material.hlsli does.
    float squareTangentLength = dot(vtx.tangent.xyz, vtx.tangent.xyz);

    if (pixelNormal.z && squareTangentLength > 0 && vtx.tangent.w != 0) {
        float3 tangent = vtx.tangent.xyz * rsqrt(squareTangentLength);
        // Honour the stored handedness rather than assuming a fixed winding.
        float3 binormal = cross(normal, tangent) * vtx.tangent.w;

        float3x3 tangentMatrix = float3x3(tangent, binormal, normal);
        normal = normalize(mul(pixelNormal * 2 - 1, tangentMatrix));
    }

    return normal;
}

void main(
    in float4 i_position: SV_Position,
    in SceneVertex i_vtx,
    out float4 o_albedo: SV_Target0,
    out float4 o_normal: SV_Target1
)
{
    float4 diffuseColor;
    diffuseColor.rgb = t_BaseOrDiffuse.Sample(s_MaterialSampler, GetFlipUV(i_vtx.texCoord)).rgb;
    diffuseColor.a = t_MetalRoughOrSpecular.Sample(s_MaterialSampler, GetFlipUV(i_vtx.texCoord)).r;
    float3 normal = GetNormal(i_vtx);
    float roughness = (diffuseColor.a > 0) ? 0.5 : 0;

    o_albedo = diffuseColor;
    o_normal = float4(normal.xyz, roughness);
}