/*
 * Debug view of one face set / mip of a cube map array as a lat-long (equirectangular) image.
 * UV.x = longitude [0,1) -> [0, 2pi), UV.y = latitude [0,1] -> [+pi/2, -pi/2].
 *
 * Entry point: main (ps_6_0).   Reconstructed from DXIL (show_cubemap_ps).
 */

cbuffer CB : register(b0)
{
    uint g_ArrayIndex;
    uint g_MipLevel;
};

TextureCubeArray t_SourceTexture : register(t0);
SamplerState s_Sampler : register(s0);

static const float PI = 3.14159265;

float4 main(float4 i_position : SV_Position, float2 i_uv : UV) : SV_Target
{
    float azimuth = i_uv.x * 2.0 * PI;
    float elevation = (0.5 - i_uv.y) * PI;

    float3 direction = float3(
        cos(azimuth) * cos(elevation),
        sin(elevation),
        -sin(azimuth) * cos(elevation));

    return t_SourceTexture.SampleLevel(s_Sampler, float4(direction, g_ArrayIndex), g_MipLevel);
}
