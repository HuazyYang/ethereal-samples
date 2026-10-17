#ifndef WW_REGISTER
#ifdef WAVEWORKS_SPACE
#define WW_REGISTER(reg) register(reg, WAVEWORKS_SPACE)
#else
#define WW_REGISTER(reg) register(reg)
#endif
#endif
#ifndef BEACH_ATMOSPHERE_INTEGRATION_HLSLI
#define BEACH_ATMOSPHERE_INTEGRATION_HLSLI
#include "BeachAtmosphere.hlsli"
#include "BeachAtmosphereConstants.hlsli"
Texture2D g_multipleScattering : WW_REGISTER(t1);
SamplerState g_atmosphereSampler : WW_REGISTER(s0);

// Finite radiative transfer shared by the sky view and aerial volume.
// Hillaire (EGSR 2020): single scattering plus isotropic multiple-scattering LUT,
// exact homogeneous segment integration and RGB Beer-Lambert throughput.
void BeachIntegrateAtmosphere(float3 origin, float3 direction, float rayLength,
    uint sampleCount, out float3 radiance, out float3 throughput)
{
    float mu = dot(direction, g_atmosphereSun);
    float rayleighPhase = RayleighPhase(mu), miePhase = MiePhase(mu);
    throughput = 1; radiance = 0;
    [loop] for (uint i = 0; i < sampleCount; ++i)
    {
        float a = float(i) / sampleCount, b = float(i + 1) / sampleCount;
        float start = a * a * rayLength, end = b * b * rayLength;
        float stepLength = end - start;
        float3 p = origin + direction * ((start + end) * 0.5);
        float3 extinction, rayleigh;
        float mie;
        AtmosphereMedium(p, g_aerosolExtinction, extinction, rayleigh, mie);
        float3 opticalDepth = extinction * stepLength;
        float3 transmittance = exp(-opticalDepth);
        float3 source = SolarTransmittance(p, g_atmosphereSun, g_aerosolExtinction) *
            (rayleigh * rayleighPhase + mie * miePhase) * g_solarIrradiance;
        float altitude = max(length(p) - PlanetRadius, 0.0);
        float sunCos = dot(normalize(p), g_atmosphereSun);
        float2 multipleUv = clamp(float2(sunCos * 0.5 + 0.5,
            sqrt(altitude / 100.0)), 0.5 / 32.0, 1.0 - 0.5 / 32.0);
        source += (rayleigh + mie) * g_multipleScattering.SampleLevel(
            g_atmosphereSampler, multipleUv, 0).rgb;
        // Small-optical-depth series preserves the first sub-metre slices.
        float3 integral = float3(
            opticalDepth.x < .001 ? stepLength * (1 - .5 * opticalDepth.x) : (1 - transmittance.x) / extinction.x,
            opticalDepth.y < .001 ? stepLength * (1 - .5 * opticalDepth.y) : (1 - transmittance.y) / extinction.y,
            opticalDepth.z < .001 ? stepLength * (1 - .5 * opticalDepth.z) : (1 - transmittance.z) / extinction.z);
        radiance += throughput * source * integral;
        throughput *= transmittance;
    }
}
#endif
