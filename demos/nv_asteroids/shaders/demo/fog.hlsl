/*
 * Volumetric height fog with sun shadows (single scattering ray march) at quarter resolution,
 * followed by a depth-aware upsampling filter.
 *
 * The fog layer is a gaussian in world height: density(y) = densityScale * exp(-(y - meanHeight)^2 / (2 thickness^2)).
 * ps_trace marches from the camera to the scene depth inside the slab meanHeight +- 10 thickness with
 * a step size that grows with distance and shrinks with density, accumulates in-scattered sun light
 * (Henyey-Greenstein phase, cascaded shadow maps, analytic extinction towards the sun through the
 * gaussian layer) and writes (inscatter, 1 - transparency). ps_filter upsamples the half-res result
 * to full resolution, weighting the 4x4 neighbourhood by linear-depth similarity.
 *
 * Both passes address the full-resolution depth buffer through the same pixel pattern: low-res pixel
 * p reads hi-res pixel 2p + (p.y & 1, ~p.x & 1).
 *
 * Entry points: ps_trace (ps_6_0), ps_filter (ps_6_0).   Reconstructed from DXIL (fog_ps_trace, fog_ps_filter).
 */

#include "include/space_cb.h"
#include "include/space_shadows.hlsli"

ConstantBuffer<FogConstants> g_Fog : register(b0);

Texture2D<float> t_DepthBuffer : register(t0);
Texture2DArray t_ShadowMapArray : register(t1);     // ps_trace
Texture2D t_UnfilteredFog : register(t1);           // ps_filter
SamplerComparisonState s_ShadowSampler : register(s0);

static const float PI = 3.14159265;

int2 GetHiResPixel(int2 lowResPixel)
{
    return lowResPixel * 2 + (int2(lowResPixel.y, lowResPixel.x + 1) & 1);
}

float GetLinearDepth(float depth)
{
    return g_Fog.projectionB / (depth - g_Fog.projectionA);
}

// ---------------------------------------------------------------------------------------------
// Trace
// ---------------------------------------------------------------------------------------------

float GetDensity(float height)
{
    float d = height - g_Fog.meanHeight;
    return g_Fog.densityScale * exp(-(d * d) / (g_Fog.thickness * g_Fog.thickness * 2.0));
}

// March step: 15% of the distance travelled in clear air, 2% in fully dense fog, at least 1 unit.
float GetStepSize(float density, float offset)
{
    return max(1.0, lerp(0.15, 0.02, density) * offset);
}

// Mangled name: ?IntersectRayWithXZPlanes@@YA_NV?$vector@M$02@@0MMAIAM1@Z
bool IntersectRayWithXZPlanes(float3 origin, float3 direction, float y0, float y1, inout float tNear, inout float tFar)
{
    if (abs(direction.y) < 1e-8)
        return false;

    float t0 = (y0 - origin.y) / direction.y;
    float t1 = (y1 - origin.y) / direction.y;
    tNear = min(t0, t1);
    tFar = max(t0, t1);
    return true;
}

// Abramowitz & Stegun 7.1.27
float erf(float x)
{
    float a = abs(x);
    float a2 = a * a;
    float s = 1.0 + dot(float4(0.278393, 0.230389, 0.000972, 0.078108), float4(a, a2, a2 * a, a2 * a2));
    float s2 = s * s;
    float v = 1.0 - 1.0 / (s2 * s2);
    if (x < 0)
        v = -v;
    return v;
}

// Integral over t in [0, inf) of exp(-(y + t * dirY - mean)^2 / (2 sigma^2)), i.e. the optical
// depth (per unit density) of the gaussian fog layer along a ray leaving height y with vertical
// direction component dirY. Mangled name: ?extinctionIntegralInf@@YAMMMMMM@Z
float extinctionIntegralInf(float y, float mean, float sigma, float dirY, float scale)
{
    float t0 = (mean - y) / dirY;       // ray parameter of the layer center
    float sigmaT = sigma / dirY;        // layer sigma measured along the ray
    float v = erf(-t0 * (1.0 / sqrt(2.0)) / sigmaT);
    return scale * sigmaT * sqrt(PI / 2.0) * (1.0 - v);
}

// Mangled name: ?GetShadow@@YAMV?$vector@M$02@@MAIA_N@Z
float GetShadow(float3 worldPos, float distance, inout bool useShadowMaps)
{
    if (!useShadowMaps)
        return 1;

    LightConstants light = g_Fog.light;

    // Copy the cascade indices once: indexing the constant-buffer int4 with the loop counter made DXC 1.8 re-copy it into
    // a local array on every iteration (the 2018 compiler hoisted that copy), costing ~0.4 ms in the fog pass.
    int4 cascadeIndices = light.shadowCascades;
    float2 shadow = 0;
    for (int cascade = 0; cascade < FOG_MAX_SHADOWS; cascade++)
    {
        int shadowIndex = cascadeIndices[cascade];
        if (shadowIndex >= 0)
        {
            float2 cascadeShadow = EvaluateShadowPCF(t_ShadowMapArray, s_ShadowSampler, g_Fog.shadows[shadowIndex], worldPos);

            shadow = saturate(shadow + cascadeShadow * (1.0001 - shadow.y));

            if (shadow.y == 1)
                break;
        }
        else
            break;
    }

    shadow.x += (1 - shadow.y) * light.outOfBoundsShadow;

    // Beyond the shadow maps: stop sampling them for the rest of the ray.
    if (shadow.y == 0 && distance > g_Fog.maxShadowDistance)
        useShadowMaps = false;

    return shadow.x;
}

float4 ps_trace(float4 i_position : SV_Position, float2 i_uv : UV) : SV_Target
{
    LightConstants light = g_Fog.light;

    int2 pixel = int2(i_position.xy);
    float depth = t_DepthBuffer[GetHiResPixel(pixel)];

    float4 clipPos = float4(i_uv.x * 2.0 - 1.0, 1.0 - i_uv.y * 2.0, depth, 1.0);
    float4 viewPos = mul(clipPos, g_Fog.matClipToView);
    viewPos.xyz /= viewPos.w;
    float3 worldPos = mul(float4(viewPos.xyz, 1.0), g_Fog.matViewToWorld).xyz;

    float3 ray = worldPos - g_Fog.cameraPos.xyz;
    float rayLength = length(ray);
    float3 rayDir = ray / rayLength;
    float distance = rayLength * 0.95;

    // Henyey-Greenstein (g = 0.75), normalized to 1 in the forward direction.
    // unresolved: the scale constant is the compiled value; its symbolic origin is unknown.
    const float g = 0.75;
    float cosTheta = dot(rayDir, -light.direction);
    float hg = (1 - g) / sqrt(max(1 + g * g - 2 * g * cosTheta, 0));
    float phase = hg * hg * 0.5973151326 * hg;

    // Clip the ray to the slab that contains the fog.
    float offset = 0.5;
    float tNear, tFar;
    float slabHalfHeight = g_Fog.thickness * 10.0;
    if (IntersectRayWithXZPlanes(g_Fog.cameraPos.xyz, rayDir, g_Fog.meanHeight - slabHalfHeight, g_Fog.meanHeight + slabHalfHeight, tNear, tFar))
    {
        if (tNear > distance)
            return 0;

        offset = max(tNear, offset);
        distance = min(tFar, distance);
    }

    float3 currentPos = g_Fog.cameraPos.xyz + rayDir * offset;

    // Dither the start of the march with the 4x4 pattern and a temporal offset.
    float stepSize = GetStepSize(GetDensity(currentPos.y), offset);
    int2 noiseCoord = int2(i_position.xy + g_Fog.randomOffset.xy) & 3;
    offset += g_Fog.noisePattern[noiseCoord.y][noiseCoord.x] * stepSize + frac(g_Fog.randomOffset.z) * 3.0;

    float transparency = 1.0;
    float inscatter = 0;
    bool useShadowMaps = true;

    [loop]
    while (true)
    {
        float density = GetDensity(currentPos.y);
        stepSize = GetStepSize(density, offset);
        offset += stepSize;

        if (offset >= distance)
            break;

        currentPos = g_Fog.cameraPos.xyz + rayDir * offset;

        float shadow = GetShadow(currentPos, offset, useShadowMaps);

        // Faint ambient term, fading in towards the top of the layer.
        float ambient = smoothstep(g_Fog.meanHeight - g_Fog.thickness, g_Fog.meanHeight + g_Fog.thickness, currentPos.y) * 0.001;

        if (shadow > 0)
        {
            // Extinction of the sun light on its way through the fog layer (towards -light.direction).
            // unresolved: the 0.75 factor (extinction coefficient?) is the compiled value.
            shadow *= exp(-g_Fog.densityScale * extinctionIntegralInf(currentPos.y, g_Fog.meanHeight, g_Fog.thickness, -light.direction.y, 0.75));
        }

        // note: weighted by (1 - transparency), not by transparency; kept as compiled.
        inscatter += stepSize * (1.0 - transparency) * density * (phase * shadow + ambient);
        transparency *= exp(-stepSize * density);

        if (transparency < g_Fog.visibilityThreshold)
            break;
    }

    // Illuminance of the sun disk: radiance * angularSize^2 / 8
    float irradiance = light.angularSizeOrInvRange * light.angularSizeOrInvRange * (light.radiance * 0.125);

    return float4(light.color * inscatter * g_Fog.color * irradiance * g_Fog.intensity, 1.0 - transparency);
}

// ---------------------------------------------------------------------------------------------
// Filter (depth-aware upsampling)
// ---------------------------------------------------------------------------------------------

float4 ps_filter(float4 i_position : SV_Position, float2 i_uv : UV) : SV_Target
{
    float depth = t_DepthBuffer[uint2(i_position.xy)];
    float linearDepth = GetLinearDepth(depth);

    int2 lowResCenter = int2(i_position.xy) >> 1;

    float4 result = 0;
    float4 fallbackResult = 0;
    float weightSum = 0;

    for (int dy = -2; dy < 2; dy++)
    {
        for (int dx = -2; dx < 2; dx++)
        {
            int2 lowResPixel = lowResCenter + int2(dx, dy);
            float sampleDepth = t_DepthBuffer[GetHiResPixel(lowResPixel)];
            float4 fog = t_UnfilteredFog[lowResPixel];

            float weight;
            if (depth == 1)
                weight = (sampleDepth == 1) ? 1 : 0;    // sky pixel: only use sky samples
            else
                weight = saturate(1.0 - abs(GetLinearDepth(sampleDepth) - linearDepth) * 10.0 / linearDepth);

            result += weight * fog;
            fallbackResult += fog;
            weightSum += weight;
        }
    }

    if (weightSum > 0)
        return result / weightSum;

    return fallbackResult / 16.0;
}
