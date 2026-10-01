#pragma pack_matrix(row_major)

#include <donut/shaders/gbuffer.hlsli>
#include <donut/shaders/lighting.hlsli>
#include <donut/shaders/shadows.hlsli>
#include <donut/shaders/deferred_lighting_cb.h>
#include <donut/shaders/binding_helpers.hlsli>

cbuffer c_Deferred : register(b0)
{
    DeferredLightingConstants g_Deferred;
};

Texture2DArray t_ShadowMapArray : register(t0);
TextureCubeArray t_DiffuseLightProbe : register(t1);
TextureCubeArray t_SpecularLightProbe : register(t2);
Texture2D t_EnvironmentBrdf : register(t3);

SamplerState s_ShadowSampler : register(s0);
SamplerComparisonState s_ShadowSamplerComparison : register(s1);
SamplerState s_LightProbeSampler : register(s2);
SamplerState s_BrdfSampler : register(s3);

Texture2D t_GBufferDepth : register(t8);
Texture2D t_GBuffer0 : register(t9);
// Texture2D t_GBuffer1 : register(t10);
Texture2D t_GBuffer2 : register(t11);
// Texture2D t_GBuffer3 : register(t12);

Texture2D t_IndirectDiffuse : register(t14);
Texture2D t_IndirectSpecular : register(t15);
Texture2D t_ShadowBuffer : register(t16);
Texture2D t_AmbientOcclusion : register(t17);

VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> u_Output : register(u0);

float GetRandom(float2 pos)
{
    int x = int(pos.x) & 3;
    int y = int(pos.y) & 3;
    return g_Deferred.noisePattern[y][x];
}

float Luminance(float3 color)
{
    return 0.2126 * color.r + 0.7152 * color.g + 0.0722 * color.b;
}

float3 ConvertToLDR(float3 color)
{
    float srcLuminance = Luminance(color);

    float sqrWhiteLuminance = 50;
    float scaledLuminance = srcLuminance * 8;
    float mappedLuminance = (scaledLuminance * (1 + scaledLuminance / sqrWhiteLuminance)) / (1 + scaledLuminance);

    return color * (mappedLuminance / srcLuminance);
}

[numthreads(16, 16, 1)]
void main(int2 i_globalIdx: SV_DispatchThreadID)
{
    if (any(i_globalIdx.xy >= int2(g_Deferred.view.viewportSize)))
        return;

    int2 pixelPosition = i_globalIdx.xy + int2(g_Deferred.view.viewportOrigin);

#if 0
    // float4 albedo = t_GBuffer0[pixelPosition];
    // float4 normal = t_GBuffer2[pixelPosition];
    // float4 indirectDiffuse = t_IndirectDiffuse[pixelPosition];
    // float3 indirectSpecular = t_IndirectSpecular[pixelPosition].rgb;
    // float ao = t_AmbientOcclusion.SampleLevel(s_BrdfSampler, float2(pixelPosition) * g_Deferred.view.viewportSizeInv, 0).r;

    // float3 surfaceWorldPos = ReconstructWorldPosition(g_Deferred.view, float2(pixelPosition) + 0.5, t_GBufferDepth[pixelPosition].x);

    // float3 diffuseTerm = 0;
    // float3 specularTerm = 0;

    // LightConstants light = g_Deferred.lights[0];

    // float3 posLightSpace = mul(float4(surfaceWorldPos, 1.0), g_Deferred.shadows[0].matWorldToUvzwShadow).xyz;
    float3 posLightSpace = float3(float2(pixelPosition) / 2048, 0.5f);
    posLightSpace.xy = saturate(posLightSpace.xy);
    float shadow = t_ShadowMapArray.SampleCmpLevelZero(s_ShadowSamplerComparison, float3(posLightSpace.xy, 0.0), posLightSpace.z);

    u_Output[pixelPosition] = float4(shadow.xxx, 1.0);

#else
    float4 albedo = t_GBuffer0[pixelPosition];
    float4 normal = t_GBuffer2[pixelPosition];
    float4 indirectDiffuse = t_IndirectDiffuse[pixelPosition];
    float3 indirectSpecular = t_IndirectSpecular[pixelPosition].rgb;
    float ao = t_AmbientOcclusion.SampleLevel(s_BrdfSampler, float2(pixelPosition) * g_Deferred.view.viewportSizeInv, 0).r;

    float3 surfaceWorldPos = ReconstructWorldPosition(g_Deferred.view, float2(pixelPosition) + 0.5, t_GBufferDepth[pixelPosition].x);

    float3 diffuseTerm = 0;
    float3 specularTerm = 0;
    float angle = GetRandom(i_globalIdx.xy * g_Deferred.randomOffset);
    float2 sincos = float2(sin(angle), cos(angle));

    LightConstants light = g_Deferred.lights[0];

    float2 combinedShadow = EvaluateShadowPoisson(t_ShadowMapArray, s_ShadowSamplerComparison, g_Deferred.shadows[light.shadowCascades[0]], surfaceWorldPos, sincos, 3.0);
    float shadow = combinedShadow.x + (1 - combinedShadow.y) * light.outOfBoundsShadow;

    float NdotL = saturate(-dot(normal.xyz, light.direction));

    float3 result = albedo.rgb * (light.color.rgb * (shadow * NdotL) + lerp(g_Deferred.ambientColorTop.rgb, indirectDiffuse.rgb, indirectDiffuse.a)) * ao
            + albedo.a * indirectSpecular.rgb;

    result = ConvertToLDR(result);

    u_Output[pixelPosition] = float4(result, 1);
#endif
}
