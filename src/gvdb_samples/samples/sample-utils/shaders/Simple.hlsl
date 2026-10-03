#pragma pack_matrix(row_major)
#include <donut/shaders/binding_helpers.hlsli>

struct Common {
    float4x4 modelMatrix;
    float4x4 viewProjMatrix;
    float4 eyePosW;
    float4 lightPosW;
    float4 ambientColor;
    float4 diffuseFactor;
    float4 specularFactor;
};

DECLARE_CBUFFER(Common, g_Const, 0, 0);

struct VSOutput {
    float3 posW : POSITION;
    float3 normalW : NORMAL;
    float4 posH: SV_Position;
    float4 posH2: POSITION2;
};

VSOutput VSMain(float3 posL: POSITION, float3 normalL: NORMAL) {
    VSOutput vout = (VSOutput)0;
    float4 posW = mul(float4(posL, 1.0), g_Const.modelMatrix);
    float4 normalW = mul(float4(normalL, 0.0), g_Const.modelMatrix);
    vout.posW = posW.xyz;
    vout.normalW = normalW.xyz;
    vout.posH = mul(posW, g_Const.viewProjMatrix);
    vout.posH2 = vout.posH;

    return vout;
}

struct PixelOutput {
    float4 color: SV_Target0;
    float depth: SV_Target1;
};

PixelOutput PSMain(VSOutput pin) {
    PixelOutput pout = (PixelOutput)0;

    // Vertex normal, flipped towards the eye (OBJ meshes may be inconsistently wound).
    float3 n = normalize(pin.normalW);
    if (dot(n, g_Const.eyePosW.xyz - pin.posW) < 0.0) n = -n;

    float3 lightDir = normalize(g_Const.lightPosW.xyz - pin.posW);
    float4 diffuse = g_Const.diffuseFactor * max(dot(lightDir, n), 0.0);

    pout.color = g_Const.ambientColor + diffuse;
    pout.depth = pin.posH2.z / pin.posH2.w;

    return pout;
}