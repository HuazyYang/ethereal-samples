/*
 * Textured star field / nebula background (full-screen pass).
 * - Nebula: the environment cube map, boosted where it is bright (saturate(1/(1-luma) - 1)).
 * - Stars: a tiling star texture projected per cube face and sampled at three frequencies,
 *   faded out where the nebula is bright.
 *
 * Entry point: main (ps_6_0).   Reconstructed from DXIL (texturedstarfield_ps).
 */

#include "include/space_cb.h"

cbuffer c_Sky : register(b0)
{
    StarFieldConstants g_Sky;
};

TextureCube t_EnvironmentMap : register(t0);
Texture2D t_StarMap : register(t1);
SamplerState s_Sampler : register(s0);

float4 main(float4 i_position : SV_Position, float2 i_uv : UV) : SV_Target
{
    float2 clipPos = float2(i_uv.x * 2.0 - 1.0, 1.0 - i_uv.y * 2.0);
    float4 worldPos = mul(float4(clipPos, 0.5, 1.0), g_Sky.matClipToTranslatedWorld);
    float3 direction = normalize(worldPos.xyz / worldPos.w);

    float3 nebula = t_EnvironmentMap.Sample(s_Sampler, float3(-direction.x, direction.y, direction.z)).rgb;

    float3 starColor = 0;

    if (g_Sky.starLinearBrightness > 0 || g_Sky.starSquareBrightness > 0)
    {
        // Project onto the dominant cube face: faceUV in [-1,1]^2 for that face.
        float3 absDir = abs(direction);
        float maxComponent = max(absDir.x, max(absDir.y, absDir.z));
        float3 faceMask = step(maxComponent, absDir);

        float2 faceUV = float2(
            dot(faceMask, float3(direction.z, -direction.y, direction.x)),
            dot(faceMask, float3(direction.y, direction.x, -direction.y))) / maxComponent;

        faceUV += 1.0;

        float3 stars = t_StarMap.Sample(s_Sampler, faceUV * 2.0).rgb
                     + t_StarMap.Sample(s_Sampler, faceUV * 4.0).rgb
                     + t_StarMap.Sample(s_Sampler, faceUV * 7.0).rgb;

        starColor = (stars * g_Sky.starSquareBrightness + g_Sky.starLinearBrightness) * stars;

        // Hide the stars behind bright nebula regions.
        float nebulaLuma = dot(nebula, float3(0.299, 0.587, 0.114));
        starColor *= 1.0 - saturate(pow(nebulaLuma, 2.0) * 8.0);
    }

    float luminance = dot(nebula, float3(0.2126, 0.7152, 0.0722));
    float3 nebulaColor = g_Sky.nebulaLinearBrightness * nebula * saturate(1.0 / (1.0 - luminance) - 1.0);

    return float4(nebulaColor + starColor, 0);
}
