#include <donut/shaders/binding_helpers.hlsli>

// HAS_TEXTURE_2 = {0, 1}

struct Common {
    float4 screenST;
};

DECLARE_PUSH_CONSTANTS(Common, g_Const, 0, 0);

SamplerState g_SamplerState: register(s0);
Texture2D g_Texture1: register(t0);
Texture2D g_Texture2: register(t1);

struct VSOutput {
    float2 uv: TEXCOORD0;
    float4 posH: SV_Position;
};

VSOutput VSMain(uint id: SV_VertexID) {
    VSOutput vout;
    float2 uv = float2( float(id & 1), float(id >> 1) );
    vout.posH = float4(2.f * uv.x - 1.f, 1.f - 2.f * uv.y, 1.f, 1.f);
    vout.uv = uv * g_Const.screenST.xy + g_Const.screenST.zw;;
    return vout;
}

float4 PSMain(float2 uv: TEXCOORD0): SV_Target {
    float4 color = g_Texture1.Sample(g_SamplerState, uv);
#if HAS_TEXTURE_2 == 1
    float4 color2 = g_Texture2.Sample(g_SamplerState, uv);
    color.xyz = lerp(color.xyz, color2.xyz, color2.w);
    // color = color2;
#endif
    color.w = 1.0;
    return color;
}