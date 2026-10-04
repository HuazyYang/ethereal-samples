/*
* Asteroids (2018 donut framework) - alpha-tested depth-only pixel shader (passes/depth_ps.hlsl).
* Clips where the albedo texture alpha is below 0.5. No permutations, no constant buffer.
*/

Texture2D t_Albedo : register(t0);
SamplerState s_MaterialSampler : register(s0);

void main(
    in float4 i_position : SV_Position,
    in float2 i_uv : UV)
{
    float4 albedo = t_Albedo.Sample(s_MaterialSampler, i_uv);
    clip(albedo.a - 0.5);
}
