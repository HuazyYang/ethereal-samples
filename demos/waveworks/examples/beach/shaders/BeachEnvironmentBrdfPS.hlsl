#include "BeachEnvironment.hlsli"

float2 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float NoV = max(uv.x, 1e-4), roughness = uv.y;
    float3 N = float3(0, 0, 1);
    float3 V = float3(sqrt(saturate(1.0 - NoV * NoV)), 0, NoV);
    float2 integral = 0;
    [loop] for (uint i = 0; i < 512; ++i)
    {
        float3 H = BeachSampleGGX(BeachHammersley(i, 512), roughness, N);
        float VoH = saturate(dot(V, H));
        float3 L = 2.0 * VoH * H - V;
        float NoL = saturate(L.z);
        if (NoL > 0)
        {
            // The same correlated Smith function as the direct-light BRDF.
            // D cancels the GGX sampling PDF; integrate the two Schlick terms.
            float visibility = BeachVisibilitySmithJointGGX(NoV, NoL, roughness) *
                NoL * 4.0 * VoH / max(H.z, 1e-5);
            float Fc = pow(1.0 - VoH, 5.0);
            integral += float2(1.0 - Fc, Fc) * visibility;
        }
    }
    return integral / 512.0;
}
