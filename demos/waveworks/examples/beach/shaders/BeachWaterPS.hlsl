#include "BeachWaterCommon.hlsli"

bool BeachRefractScene(float3 origin, float3 ray, float waterDistance, float maximumDistance,
    out float2 hitUv, out float hitDistance)
{
    hitUv = 0; hitDistance = 0;
    float previous = 0;
    [loop] for (uint i = 1; i <= 16; ++i)
    {
        float t = float(i) / 16.0;
        t = t * t * maximumDistance;
        float3 samplePosition = origin + ray * t;
        float4 clip = mul(g_worldToClip, float4(samplePosition, 1));
        float2 uv = clip.xy / max(clip.w, 1e-5) * float2(0.5, -0.5) + 0.5;
        if (clip.w <= 0 || any(uv < 0) || any(uv > 1)) return false;
        // Nearest depth preserves silhouettes; bilinear radial depth invents
        // surfaces between a foreground cliff and a distant ocean/sky pixel.
        float sceneDepth = g_linearDepth.Load(int3(int2(uv / g_inverseViewport), 0)).r;
        float rayDepth = distance(samplePosition, g_eyePosition);
        if (sceneDepth > waterDistance + 0.02 && sceneDepth < g_farClip && rayDepth >= sceneDepth)
        {
            float low = previous, high = t;
            [unroll] for (uint j = 0; j < 7; ++j)
            {
                float mid = 0.5 * (low + high);
                float3 p = origin + ray * mid;
                float4 projected = mul(g_worldToClip, float4(p, 1));
                float2 candidate = projected.xy / max(projected.w, 1e-5) * float2(0.5, -0.5) + 0.5;
                float candidateDepth = g_linearDepth.Load(int3(int2(candidate / g_inverseViewport), 0)).r;
                if (distance(p, g_eyePosition) >= candidateDepth && candidateDepth > waterDistance)
                    high = mid;
                else low = mid;
            }
            hitDistance = high;
            float4 projected = mul(g_worldToClip, float4(origin + ray * high, 1));
            hitUv = projected.xy / projected.w * float2(0.5, -0.5) + 0.5;
            return true;
        }
        previous = t;
    }
    return false;
}

float4 main(DSOutput input) : SV_Target
{
    BeachWaveSurface surface = BeachReadWaveSurface(input);
    float3 N = surface.normal;
    float3 V = normalize(g_eyePosition - input.worldDisplaced);
    float3 L = normalize(g_sunDirection);
    float ior = max(g_waterIOR, 1.0001);
    float f0 = (ior - 1.0) / (ior + 1.0);
    f0 *= f0;
    float fresnel = g_useFresnel > 0.5
        ? BeachFresnelSchlick(saturate(dot(N, V)), f0.xxx).r : f0;
    float2 uv = input.clipPosition.xy * g_inverseViewport;
    float waterDistance = distance(input.worldDisplaced, g_eyePosition);
    float opaqueDistance = g_linearDepth.SampleLevel(g_linearClamp, uv, 0).r;
    // Radial depth difference already measures ray length; do not divide by N.V.
    float pathLength = clamp(opaqueDistance - waterDistance, 0.0, g_maxOpticalDepth);
    float3 transmittedRay = refract(-V, N, 1.0 / ior);
    // A finite screen-space ray is a visibility lookup, not an optical-depth
    // cutoff. Beyond it use the local bathymetric tangent plane. Switching to
    // camera-ray distance here produced a conspicuous 18 m turquoise shelf.
    float2 dxWorld = ddx(input.worldUndisplaced.xy);
    float2 dyWorld = ddy(input.worldUndisplaced.xy);
    float determinant = dxWorld.x * dyWorld.y - dxWorld.y * dyWorld.x;
    float2 depthGradient = 0;
    if (abs(determinant) > 1e-8)
        depthGradient = float2(
            ddx(input.shoreData.x) * dyWorld.y - ddy(input.shoreData.x) * dxWorld.y,
            ddy(input.shoreData.x) * dxWorld.x - ddx(input.shoreData.x) * dyWorld.x) / determinant;
    float planePath = input.shoreData.x / max(-transmittedRay.z -
        dot(depthGradient, transmittedRay.xy), 0.02);
    pathLength = min(planePath, g_maxOpticalDepth);
    float2 candidate;
    float refractedLength;
    bool validRefraction = BeachRefractScene(input.worldDisplaced, transmittedRay,
        waterDistance, min(g_maxOpticalDepth,
            max(g_refractionDistanceLimit, 1.5 * planePath + 2.0)), candidate, refractedLength);
    float2 refractedUv = validRefraction ? candidate : uv;
    float3 background = g_opaqueScene.SampleLevel(g_linearClamp, refractedUv, 0).rgb;
    // The opaque capture contains sky where there is no seabed. Sky is not
    // radiance arriving from below an optically deep ocean.
    float backgroundDepth = g_linearDepth.SampleLevel(g_linearClamp, refractedUv, 0).r;
    if (backgroundDepth >= g_farClip) background = 0;
    if (validRefraction) pathLength = refractedLength;
    // Exponential near-bed sediment concentration; coefficients are inverse metres.
    float concentration = g_shallowWater * g_sedimentConcentration *
        exp(-max(input.shoreData.x, 0.0) / max(g_sedimentDepth, 1e-3));
    float3 sigmaA = max(g_absorption + concentration * g_sedimentAbsorption, 0.0);
    float3 sigmaS = max(g_scattering + concentration * g_sedimentScattering, 0.0);
    float3 sigmaT = max(sigmaA + sigmaS, 1e-6);
    float3 transmittance = exp(-sigmaT * pathLength);
    // Analytic homogeneous single scattering: both the viewing path and
    // the solar path attenuate the source. Isotropic phase is 1/(4*pi).
    float sunPathRatio = max(-transmittedRay.z, 0.0) / max(L.z, 0.01);
    float3 sunExtinction = sigmaT * (1.0 + sunPathRatio);
    float sunInterface = 1.0 - BeachFresnelSchlick(saturate(L.z), f0.xxx).r;
    float sunVisibility = BeachSunVisibility(input.worldDisplaced.xzy, N.xzy);
    float3 solarScatter = sigmaS * g_sunRadiance * g_sunIntensity * sunInterface *
        (1.0 - exp(-sunExtinction * pathLength)) / sunExtinction /
        (4.0 * BEACH_PI) * sunVisibility;
    float3 ambientScatter = sigmaS / sigmaT * 2.0 * BeachSkyIrradiance() *
        (1.0 - transmittance) / (4.0 * BEACH_PI);
    float3 transmission = background * transmittance + solarScatter + ambientScatter;
    float roughness = BeachApplySpecularAA(N, sqrt(sqrt(saturate(surface.variance))));
    float3 reflection = BeachRoughSkyReflection(N, V, roughness, f0) * g_useReflection;
    float3 specular = BeachSpecularBRDF(N, V, L, f0.xxx, roughness) *
        g_sunRadiance * g_sunIntensity * saturate(dot(N, L)) * g_useSpecular * sunVisibility;
    float3 color = max((1.0 - fresnel) * transmission + reflection + specular, 0.0);
    // Explicit diagnostics only; the production material above is unchanged.
    float4 swe = g_sweSurface.SampleLevel(g_linearClamp,
        BeachShallowUv(input.worldUndisplaced.xy), 0);
    if (g_oceanPadding0 == 1) return float4((pathLength / 20.0).xxx, 1);
    if (g_oceanPadding0 == 2) return float4(transmission, 1);
    if (g_oceanPadding0 == 3) return float4(reflection + specular, 1);
    if (g_oceanPadding0 == 4) return float4(validRefraction.xxx, 1);
    if (g_oceanPadding0 == 5) return float4((swe.y / 5.0).xxx, 1);
    if (g_oceanPadding0 == 6) return float4(N * 0.5 + 0.5, 1);
    if (g_oceanPadding0 == 7) return float4(swe.zzz, 1);
    if (g_oceanPadding0 == 8) return float4(swe.www, 1);
    return float4(BeachAerialPerspective(color, input.worldDisplaced,
        g_eyePosition, g_sunIntensity), 1);
}
