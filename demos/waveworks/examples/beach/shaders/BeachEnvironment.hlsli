#ifndef BEACH_ENVIRONMENT_HLSLI
#define BEACH_ENVIRONMENT_HLSLI
#include "BeachPBR.hlsli"

// Full-sphere, Z-up sky-view parameterization. Quadratic latitude allocates
// resolution to the horizon's steep optical-depth gradient (not a color ramp).
// U wraps; V clamps at the poles. Integration PDFs remain in solid angle.
float2 BeachEnvironmentUv(float3 direction)
{
    direction = normalize(direction);
    float longitudeX = dot(direction.xy, direction.xy) > 1e-12 ? direction.x : 1.0;
    float longitude = atan2(direction.y, longitudeX);
    float elevation = asin(clamp(direction.z, -1.0, 1.0));
    return float2(longitude / (2.0 * BEACH_PI) + 0.5,
        0.5 - 0.5 * sign(elevation) * sqrt(abs(elevation) / (0.5 * BEACH_PI)));
}

float3 BeachEnvironmentDirection(float2 uv)
{
    float phi = (uv.x - 0.5) * (2.0 * BEACH_PI);
    float latitude = 1.0 - 2.0 * uv.y;
    float elevation = sign(latitude) * latitude * latitude * (0.5 * BEACH_PI);
    return float3(cos(phi) * cos(elevation), sin(phi) * cos(elevation), sin(elevation));
}

float2 BeachHammersley(uint index, uint count)
{
    return float2((float(index) + 0.5) / float(count),
        float(reversebits(index)) * 2.3283064365386963e-10);
}

float3 BeachLocalDirection(float3 local, float3 N)
{
    float3 axis = abs(N.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 tangent = normalize(cross(axis, N));
    return tangent * local.x + cross(N, tangent) * local.y + N * local.z;
}

float3 BeachSampleGGX(float2 xi, float roughness, float3 N)
{
    float alpha = max(roughness * roughness, 1e-4);
    float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (alpha * alpha - 1.0) * xi.y));
    float sinTheta = sqrt(saturate(1.0 - cosTheta * cosTheta));
    float phi = 2.0 * BEACH_PI * xi.x;
    return BeachLocalDirection(float3(cos(phi) * sinTheta,
        sin(phi) * sinTheta, cosTheta), N);
}
#endif
