#ifndef BEACH_PBR_HLSLI
#define BEACH_PBR_HLSLI

static const float BEACH_PI = 3.14159265359;

float BeachDistributionGGX(float NoH, float perceptualRoughness)
{
    float alpha = max(perceptualRoughness * perceptualRoughness, 1e-4);
    float alphaSquared = alpha * alpha;
    float denominator = (1.0 - NoH * NoH) + NoH * NoH * alphaSquared;
    return alphaSquared / max(BEACH_PI * denominator * denominator, 1e-16);
}

// Height-correlated Smith visibility. This includes the 1 / (4 NoV NoL)
// denominator used by the Cook-Torrance specular BRDF.
float BeachVisibilitySmithJointGGX(float NoV, float NoL, float perceptualRoughness)
{
    float alpha = max(perceptualRoughness * perceptualRoughness, 1e-4);
    float alphaSquared = alpha * alpha;
    float smithV = NoL * sqrt(NoV * NoV * (1.0 - alphaSquared) + alphaSquared);
    float smithL = NoV * sqrt(NoL * NoL * (1.0 - alphaSquared) + alphaSquared);
    return 0.5 / max(smithV + smithL, 1e-6);
}

float3 BeachFresnelSchlick(float VoH, float3 F0)
{
    float Fc = pow(1.0 - saturate(VoH), 5.0);
    return F0 + (1.0 - F0) * Fc;
}

float3 BeachSpecularBRDF(float3 N, float3 V, float3 L, float3 F0,
    float roughness)
{
    float NoV = saturate(dot(N, V));
    float NoL = saturate(dot(N, L));
    float3 halfVector = V + L;
    halfVector *= rsqrt(max(dot(halfVector, halfVector), 1e-8));
    float NoH = saturate(dot(N, halfVector));
    float VoH = saturate(dot(V, halfVector));
    float distribution = BeachDistributionGGX(NoH, roughness);
    float visibility = BeachVisibilitySmithJointGGX(NoV, NoL, roughness);
    return distribution * visibility * BeachFresnelSchlick(VoH, F0);
}

float3 BeachDielectricF0(float specular)
{
    // Unreal's scalar Specular parameter maps [0, 1] to dielectric F0 [0, 0.08].
    return (0.08 * saturate(specular)).xxx;
}

float3 BeachMaterialF0(float3 baseColor, float metallic, float specular)
{
    float metalWeight = saturate(metallic);
    return BeachDielectricF0(specular) * (1.0 - metalWeight) +
        baseColor * metalWeight;
}

float BeachApplySpecularAA(float3 shadingNormal, float roughness)
{
    // Normal variance is microfacet slope variance at pixel footprint scale.
    // Variances add, so filtering broadens alpha without changing base color.
    float3 normalDx = ddx(shadingNormal);
    float3 normalDy = ddy(shadingNormal);
    float normalVariance = 0.5 * (dot(normalDx, normalDx) + dot(normalDy, normalDy));
    float alpha = roughness * roughness;
    return sqrt(sqrt(saturate(alpha * alpha + normalVariance)));
}

float3 BeachEvaluateDefaultLitDirect(float3 baseColor, float metallic,
    float specular, float roughness, float3 N, float3 V, float3 L,
    float3 incidentRadiance)
{
    float NoL = saturate(dot(N, L));
    float3 halfVector = V + L;
    halfVector *= rsqrt(max(dot(halfVector, halfVector), 1e-8));
    float3 F0 = BeachMaterialF0(baseColor, metallic, specular);
    float3 F = BeachFresnelSchlick(saturate(dot(V, halfVector)), F0);
    float3 diffuseColor = baseColor * (1.0 - saturate(metallic));
    float3 diffuse = (1.0 - F) * diffuseColor / BEACH_PI;
    float3 specularBrdf = BeachSpecularBRDF(N, V, L, F0, roughness);
    return (diffuse + specularBrdf) * incidentRadiance * NoL;
}

float3 BeachEvaluateLambertAmbient(float3 baseColor, float metallic,
    float specular, float ambientOcclusion, float3 skyIrradiance)
{
    float3 F0 = BeachMaterialF0(baseColor, metallic, specular);
    float3 diffuseColor = baseColor * (1.0 - saturate(metallic));
    return (1.0 - F0) * diffuseColor * skyIrradiance *
        saturate(ambientOcclusion) / BEACH_PI;
}

#endif
