#pragma pack_matrix(row_major)
#include <donut/shaders/binding_helpers.hlsli>

// DebugDraw: world-space line list with per-vertex colours.

struct LinesConstants {
    float4x4 matViewProj;
};
DECLARE_CBUFFER(LinesConstants, g_Const, 0, 0);

struct VSInput {
    float3 posW : POSITION;
    float4 color : COLOR;
};

struct VSOutput {
    float4 posH : SV_Position;
    float4 color : COLOR;
};

VSOutput VSMain(VSInput vin) {
    VSOutput vout;
    vout.posH = mul(float4(vin.posW, 1.0), g_Const.matViewProj);
    vout.color = vin.color;
    return vout;
}

float4 PSMain(VSOutput pin) : SV_Target {
    return pin.color;
}
