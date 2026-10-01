// Skybox vertex shader: body of the reference sample's assets/shaders/skybox_vs.vsh with explicit registers.
#include <donut/shaders/binding_helpers.hlsli>

struct SkyboxConstants
{
    float4x4 mViewProjection;
};
DECLARE_CBUFFER(SkyboxConstants, g_Skybox, 0, 0);

struct Skybox_VSOut
{
    float3 coords : UVFACE;
};

void main(in float3 in_position : POSITION,
          out float4 position : SV_Position,
          out Skybox_VSOut vsoutput)
{
    // NOTE: Don't translate skybox and make sure depth == 1 (no clipping)
    position = mul(g_Skybox.mViewProjection, float4(in_position, 0.0f)).xyww;
    position.z = 0.0f; // 1-z
    vsoutput.coords = in_position.xyz;
}
