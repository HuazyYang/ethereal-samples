// 2018 framework sharpen_ps (shaders/framework/sharpen_ps.bin in media.db), reconstructed from the DXIL.
// Final-blit sharpening used by CommonRenderPasses::BlitWithSharpening (Asteroids.exe 0x14007AAB0) with
// UIData::sharpness (default 0.125). Same kernel as donut's sharpen_ps; the cbuffer is the 2018 layout.

#include "blit_cb_2018.h"

cbuffer c_Blit : register(b0)
{
    BlitConstants g_Blit;
};

Texture2D tex : register(t0);
SamplerState samp : register(s0);

void main(
    in float4 i_pos : SV_Position,
    in float2 i_uv : UV,
    out float4 o_rgba : SV_Target)
{
    float4 x = tex.SampleLevel(samp, i_uv, 0);

    float4 a = tex.SampleLevel(samp, i_uv, 0, int2(-1,  0));
    float4 b = tex.SampleLevel(samp, i_uv, 0, int2( 1,  0));
    float4 c = tex.SampleLevel(samp, i_uv, 0, int2( 0,  1));
    float4 d = tex.SampleLevel(samp, i_uv, 0, int2( 0, -1));

    float4 e = tex.SampleLevel(samp, i_uv, 0, int2(-1, -1));
    float4 f = tex.SampleLevel(samp, i_uv, 0, int2( 1,  1));
    float4 g = tex.SampleLevel(samp, i_uv, 0, int2(-1,  1));
    float4 h = tex.SampleLevel(samp, i_uv, 0, int2( 1, -1));

    o_rgba = x * (6.828427 * g_Blit.sharpenFactor + 1)
        - (a + b + c + d) * g_Blit.sharpenFactor
        - (e + g + f + h) * g_Blit.sharpenFactor * 0.7071;
}
