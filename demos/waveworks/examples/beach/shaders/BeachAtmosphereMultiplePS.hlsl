#include "BeachAtmosphere.hlsli"
#include "BeachAtmosphereConstants.hlsli"

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float altitude = max(uv.y * uv.y * 100.0, 0.01);
    float sunCos = uv.x * 2.0 - 1.0;
    float3 sun = float3(sqrt(saturate(1.0 - sunCos * sunCos)), 0, sunCos);
    float3 origin = float3(0, 0, PlanetRadius + altitude);
    float3 meanRadiance = 0, meanFeedback = 0;
    // Uniform-sphere angular quadrature of single scattering and unit
    // isotropic incident radiance. Higher orders use Hillaire's isotropic
    // geometric-series closure Lms = <Lsingle> / (1 - <feedback>).
    [loop] for (uint directionIndex = 0; directionIndex < 64; ++directionIndex)
    {
        float2 xi = BeachHammersley(directionIndex, 64);
        float z = 1.0 - 2.0 * xi.x, phi = 2.0 * BEACH_PI * xi.y;
        float r = sqrt(saturate(1.0 - z * z));
        float3 direction = float3(r * cos(phi), r * sin(phi), z);
        float groundDistance = SphereInterval(origin, direction, PlanetRadius).x;
        float rayLength = groundDistance > 0 ? groundDistance :
            SphereInterval(origin, direction, AtmosphereRadius).y;
        float3 throughput = 1, radiance = 0, feedback = 0;
        float mu = dot(direction, sun);
        [loop] for (uint i = 0; i < 32; ++i)
        {
            float a = float(i) / 32.0, b = float(i + 1) / 32.0;
            float start = a * a * rayLength, end = b * b * rayLength;
            float3 p = origin + direction * ((start + end) * 0.5);
            float3 extinction, rayleigh;
            float mie;
            AtmosphereMedium(p, g_aerosolExtinction, extinction, rayleigh, mie);
            float3 transmittance = exp(-extinction * (end - start));
            float3 integral = throughput * (1.0 - transmittance) / max(extinction, 1e-7);
            radiance += integral * SolarTransmittance(p, sun, g_aerosolExtinction) * g_solarIrradiance *
                (rayleigh * RayleighPhase(mu) + mie * MiePhase(mu));
            feedback += integral * (rayleigh + mie);
            throughput *= transmittance;
        }
        if (groundDistance > 0)
        {
            float3 p = origin + direction * groundDistance;
            float3 up = normalize(p);
            radiance += throughput * 0.06 / BEACH_PI * saturate(dot(up, sun)) *
                SolarTransmittance(p + up * 0.01, sun, g_aerosolExtinction) * g_solarIrradiance;
            feedback += throughput * 0.06;
        }
        meanRadiance += radiance / 64.0;
        meanFeedback += feedback / 64.0;
    }
    return float4(meanRadiance / max(1.0 - meanFeedback, 0.01), 1);
}
