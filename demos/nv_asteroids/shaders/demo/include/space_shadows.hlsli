/*
 * Shadow-map helper of the 2018 (early donut) framework, used by the particle, lens-flare and fog
 * shaders. The function name and signature come from the mangled names in the DXIL:
 *   ?EvaluateShadowPCF@@YA?AV?$vector@M$01@@V?$Texture2DArray@...@@USamplerComparisonState@@UShadowConstants@@V?$vector@M$02@@@Z
 *   float2 EvaluateShadowPCF(Texture2DArray, SamplerComparisonState, ShadowConstants, float3)
 * The inlined body matches donut's shadows.hlsli EvaluateShadowPCF exactly.
 * Returns float2(shadow * fade, fade).
 */

#ifndef SPACE_SHADOWS_HLSLI
#define SPACE_SHADOWS_HLSLI

#include "light_cb.h"

float2 EvaluateShadowPCF(Texture2DArray ShadowMapArray, SamplerComparisonState ShadowSampler, ShadowConstants shadowParams, float3 worldPos)
{
    float4 uvzwShadow = mul(float4(worldPos, 1), shadowParams.matWorldToUvzwShadow);

    if (uvzwShadow.w <= 0)
        return 0;

    float3 uvzShadow = uvzwShadow.xyz / uvzwShadow.w;
    float2 fadeUV = saturate(abs(uvzShadow.xy - shadowParams.shadowMapCenterUV) * shadowParams.shadowFadeScale + shadowParams.shadowFadeBias);
    float fade = fadeUV.x * fadeUV.y;
    if (fade == 0)
        return 0;

    float3 sampleLocation = float3(uvzShadow.xy, shadowParams.shadowMapArrayIndex);

    float shadow = ShadowMapArray.SampleCmpLevelZero(ShadowSampler, sampleLocation, min(uvzShadow.z, 0.999999));
    return float2(shadow * fade, fade);
}

#endif // SPACE_SHADOWS_HLSLI
