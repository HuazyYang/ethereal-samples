#ifndef BEACH_ATMOSPHERE_HLSLI
#define BEACH_ATMOSPHERE_HLSLI
#include "BeachEnvironment.hlsli"

// Kilometres, with RGB extinction/scattering in inverse kilometres.
static const float PlanetRadius = 6360.0;
static const float AtmosphereRadius = 6460.0;
// Scene medium parameter: shallow marine aerosol layer, shared by sky and
// finite aerial paths. This changes density, not the output color.
static const float AerosolScaleHeight = 0.30;
static const float3 Rayleigh = float3(0.005802, 0.013558, 0.033100);
static const float3 Ozone = float3(0.000650, 0.001881, 0.000085);

float2 SphereInterval(float3 origin, float3 direction, float radius)
{
    float b = dot(origin, direction);
    float originRadius = length(origin);
    float c = (originRadius - radius) * (originRadius + radius);
    float discriminant = b * b - c;
    if (discriminant < 0) return -1;
    float root = sqrt(discriminant);
    // Stable roots retain the near intersection at grazing angles.
    float q = -b - (b >= 0 ? root : -root);
    if (abs(q) < 1e-8) return 0;
    return float2(min(q, c / q), max(q, c / q));
}

void AtmosphereMedium(float3 position, float aerosolExtinction, out float3 extinction,
    out float3 rayleighScattering, out float mieScattering)
{
    float altitude = max(length(position) - PlanetRadius, 0.0);
    rayleighScattering = Rayleigh * exp(-altitude / 8.0);
    float mie = aerosolExtinction * exp(-altitude / AerosolScaleHeight);
    mieScattering = mie * 0.9;
    float ozoneDensity = max(0.0, 1.0 - abs(altitude - 25.0) / 15.0);
    extinction = rayleighScattering + mie + Ozone * ozoneDensity;
}

float3 SolarTransmittance(float3 position, float3 sun, float aerosolExtinction)
{
    if (SphereInterval(position, sun, PlanetRadius).x > 0) return 0;
    float distanceToTop = max(SphereInterval(position, sun, AtmosphereRadius).y, 0.0);
    float3 opticalDepth = 0;
    [loop] for (uint i = 0; i < 16; ++i)
    {
        float a = float(i) / 16.0, b = float(i + 1) / 16.0;
        float start = a * a * distanceToTop, end = b * b * distanceToTop;
        float3 extinction, rayleigh;
        float mie;
        AtmosphereMedium(position + sun * ((start + end) * 0.5), aerosolExtinction,
            extinction, rayleigh, mie);
        opticalDepth += extinction * (end - start);
    }
    return exp(-opticalDepth);
}

float RayleighPhase(float mu)
{
    return 3.0 * (1.0 + mu * mu) / (16.0 * BEACH_PI);
}

float MiePhase(float mu)
{
    float g = 0.8;
    return (1.0 - g * g) /
        (4.0 * BEACH_PI * pow(max(1.0 + g * g - 2.0 * g * mu, 1e-4), 1.5));
}
#endif
