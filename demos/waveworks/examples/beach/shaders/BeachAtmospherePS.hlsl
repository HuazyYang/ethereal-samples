#include "BeachAtmosphereIntegration.hlsli"

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float3 direction = BeachEnvironmentDirection(uv);
    float3 origin = float3(0, 0, PlanetRadius + g_viewHeightKm);
    float distanceToTop = SphereInterval(origin, direction, AtmosphereRadius).y;
    float groundDistance = SphereInterval(origin, direction, PlanetRadius).x;
    float rayLength = groundDistance > 0 ? groundDistance : distanceToTop;
    float3 throughput, radiance;
    BeachIntegrateAtmosphere(origin, direction, rayLength, 48, radiance, throughput);
    if (groundDistance > 0)
    {
        float3 groundPosition = origin + direction * groundDistance;
        float NoL = saturate(dot(normalize(groundPosition), g_atmosphereSun));
        // Lambertian ground boundary. The solar disk is excluded from IBL:
        // it is integrated separately by each material's direct-light BRDF.
        radiance += throughput * 0.06 / BEACH_PI * NoL *
            SolarTransmittance(groundPosition + normalize(groundPosition) * 0.01,
                g_atmosphereSun, g_aerosolExtinction) *
            g_solarIrradiance;
    }
    return float4(max(radiance, 0.0), 1);
}
