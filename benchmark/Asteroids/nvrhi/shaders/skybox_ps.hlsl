// Skybox pixel shader: body of the reference sample's assets/shaders/skybox_ps.psh with explicit registers.
#include <donut/shaders/binding_helpers.hlsli>

struct Skybox_VSOut
{
    float3 coords : UVFACE;
};

TextureCube  Skybox         : REGISTER_SRV(0, 0);
SamplerState Skybox_sampler : REGISTER_SAMPLER(0, 0);

void main(in float4 position : SV_Position,
          in Skybox_VSOut vsoutput,
          out float4 color : SV_Target)
{
    float4 tex = Skybox.Sample(Skybox_sampler, vsoutput.coords);
    color = float4(tex.xyz, 1.0f);
}
