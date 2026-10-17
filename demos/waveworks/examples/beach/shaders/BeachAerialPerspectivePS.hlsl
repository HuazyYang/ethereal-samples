#include "BeachAtmosphereIntegration.hlsli"
#include "BeachAerialPerspective.hlsli"
struct AerialOutput
{
    float4 radiance : SV_Target0;
    float4 transmittance : SV_Target1;
};
AerialOutput main(float4 position : SV_Position, float2 uv : TEXCOORD0)
{
    uint slice = uint(position.x) / BeachAerialWidth;
    float2 screen = float2(fmod(position.x, float(BeachAerialWidth)) / BeachAerialWidth,
        position.y / BeachAerialHeight);
    float horizon = abs(g_viewUp.z) > 1e-5 ?
        .5 + .5 * g_viewForward.z / g_viewUp.z + (screen.x - .5) * g_viewRight.z / g_viewUp.z : -1;
    screen.y = BeachAerialScreenY(screen.y, horizon);
    float2 ndc = screen * float2(2, -2) + float2(-1, 1);
    float3 direction = normalize(g_viewForward + g_viewRight * ndc.x + g_viewUp * ndc.y);
    float3 origin = float3(0, 0, PlanetRadius + g_viewHeightKm);
    float rayLength = BeachAerialSliceDistance(float(slice), g_aerialFarDistance) * .001;
    float groundDistance = SphereInterval(origin, direction, PlanetRadius).x;
    float topDistance = SphereInterval(origin, direction, AtmosphereRadius).y;
    rayLength = min(rayLength, groundDistance > 0 ? groundDistance : max(topDistance, 0));
    float3 radiance, throughput;
    BeachIntegrateAtmosphere(origin, direction, rayLength, 32, radiance, throughput);
    // No ground boundary: the surface shader already supplies that radiance.
    AerialOutput output;
    output.radiance = float4(radiance, 1);
    output.transmittance = float4(throughput, 1);
    return output;
}
