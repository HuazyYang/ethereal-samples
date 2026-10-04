#pragma pack_matrix(row_major)
#include <donut/shaders/binding_helpers.hlsli>

// SPHPointRenderer: one point per particle from three vertex streams (the SPNT
// shader of the fluids5.0 reference, nv_gui.cpp CreateSPnt).
//   colour = unpacked RGBA8 (R in the low byte) + |velocity|^2 * velocityTint on rgb, alpha + 1

struct PointsConstants {
    float4x4 matWorldViewProj;   // instance transform * view * projection (row vectors)
    float velocityTint;          // the reference uses 1 / 20
    float3 padding;
};
DECLARE_PUSH_CONSTANTS(PointsConstants, g_Const, 0, 0);

struct VSInput {
    float3 pos : POSITION;
    uint clr : COLOR;
    float3 vel : VELOCITY;
};

struct VSOutput {
    float4 posH : SV_Position;
    float4 color : COLOR;
#if defined(SPIRV) || defined(TARGET_VULKAN)
    // Vulkan leaves the size of a point undefined unless the shader writes it.
    [[vk::builtin("PointSize")]] float pointSize : PSIZE;
#endif
};

float4 unpackColor(uint c) {
    return float4(float(c & 255u), float((c >> 8u) & 255u), float((c >> 16u) & 255u), float((c >> 24u) & 255u)) / 255.0;
}

VSOutput main_vs(VSInput vin) {
    VSOutput vout;
    vout.posH = mul(float4(vin.pos, 1.0), g_Const.matWorldViewProj);
    float v = dot(vin.vel, vin.vel) * g_Const.velocityTint;
    vout.color = unpackColor(vin.clr) + float4(v, v, v, 1.0);
#if defined(SPIRV) || defined(TARGET_VULKAN)
    vout.pointSize = 1.0;
#endif
    return vout;
}

float4 main_ps(VSOutput pin) : SV_Target {
    // The reference writes the unclamped colour to a fixed-point target, which clamps it.
    return saturate(pin.color);
}
