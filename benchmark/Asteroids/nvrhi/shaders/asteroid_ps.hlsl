// Asteroid pixel shader of asteroids_nvrhi. The shader body is the one of the reference sample
// (assets/shaders/asteroid_ps_diligent.psh); only the resource declarations differ.
//   BINDLESS 0 : one texture per binding set (t0, space 1)
//   BINDLESS 1 : unbounded texture array in a descriptor table (t0.., space 1), indexed per instance
#include "common_defines.h"
#include "shader_common.h"
#include <donut/shaders/binding_helpers.hlsli>

// Shader model 5.0 (D3D11) has no unbounded arrays; the bindless permutation is never loaded there.
#if BINDLESS && !defined(TARGET_D3D11)
#    define USE_BINDLESS 1
#else
#    define USE_BINDLESS 0
#endif

#if USE_BINDLESS
Texture2DArray<float4> Tex[] : REGISTER_SRV(0, 1);
#else
Texture2DArray<float4> Tex : REGISTER_SRV(0, 1);
#endif
SamplerState Tex_sampler : REGISTER_SAMPLER(0, 0);

void main(in float4 position : SV_Position,
          in VSOut vs_output,
          out float4 color : SV_Target)
{
    // Tweaking
    float3 lightPos    = float3(0.5, -0.25, -1);
    bool applyNoise    = true;
    bool applyLight    = true;
    bool applyCoverage = true;

    float3 normal = normalize(vs_output.normalWorld);

    // Triplanar projection
    float3 blendWeights = abs(normalize(vs_output.positionModel));
    float3 uvw = vs_output.positionModel * 0.5f + 0.5f;
    // Tighten up the blending zone
    blendWeights = saturate((blendWeights - 0.2f) * 7.0f);
    blendWeights /= (blendWeights.x + blendWeights.y + blendWeights.z).xxx;

    float3 coords1 = float3(uvw.yz, 0);
    float3 coords2 = float3(uvw.zx, 1);
    float3 coords3 = float3(uvw.xy, 2);

    float3 detailTex = float3(0.0, 0.0, 0.0);
#if USE_BINDLESS
    detailTex += blendWeights.x * Tex[NonUniformResourceIndex(vs_output.textureId)].Sample(Tex_sampler, coords1).xyz;
    detailTex += blendWeights.y * Tex[NonUniformResourceIndex(vs_output.textureId)].Sample(Tex_sampler, coords2).xyz;
    detailTex += blendWeights.z * Tex[NonUniformResourceIndex(vs_output.textureId)].Sample(Tex_sampler, coords3).xyz;
#else
    detailTex += blendWeights.x * Tex.Sample(Tex_sampler, coords1).xyz;
    detailTex += blendWeights.y * Tex.Sample(Tex_sampler, coords2).xyz;
    detailTex += blendWeights.z * Tex.Sample(Tex_sampler, coords3).xyz;
#endif

    float wrap = 0.0f;
    float wrap_diffuse = saturate((dot(normal, normalize(lightPos)) + wrap) / (1.0f + wrap));
    float light = 3.0f * wrap_diffuse + 0.06f;

    // Approximate partial coverage on distant asteroids (by fading them out)
    float coverage = saturate(position.z * 4000.0f);

    color.rgb = vs_output.albedo.rgb;
    [flatten] if (applyNoise)    color.rgb = color.rgb * (2.0f * detailTex);
    [flatten] if (applyLight)    color.rgb = color.rgb * light;
    [flatten] if (applyCoverage) color.rgb = color.rgb * coverage;
    color.a = 1.0;
}
