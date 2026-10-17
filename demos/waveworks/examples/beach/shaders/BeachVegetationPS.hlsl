#ifndef WW_REGISTER
#ifdef WAVEWORKS_SPACE
#define WW_REGISTER(reg) register(reg, WAVEWORKS_SPACE)
#else
#define WW_REGISTER(reg) register(reg)
#endif
#endif
#include "BeachLighting.hlsli"

cbuffer BeachFrameConstants : WW_REGISTER(b0)
{
    float4x4 g_viewProjection;
    float3 g_sunDirection; float g_sunIntensity;
    float3 g_eyePosition; float g_haze;
    float3 g_sunRadiance; float g_time;
    float3 g_skyIrradiance; float g_meanSeaLevel;
}

cbuffer BeachVegetationMaterialConstants : WW_REGISTER(b1)
{
    // x is roughness, y Unreal-style dielectric specular, z AO, and w leaf thickness.
    float4 g_vegetationSurface;
    // rgb is the Beer-Lambert absorption coefficient and w transmission scale.
    float4 g_vegetationAbsorption;
    float4 g_vegetationReflectanceScale;
}

Texture2D g_vegetationAlbedo : WW_REGISTER(t0);
SamplerState g_vegetationSampler : WW_REGISTER(s0);

struct PSInput
{
    float4 position : SV_Position;
    float3 worldPosition : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float distanceToEye : TEXCOORD2;
    float2 materialUv : TEXCOORD3;
};

struct VegetationOutput
{
    float4 color : SV_Target0;
    float depth : SV_Target1;
};

VegetationOutput main(PSInput input, bool isFrontFace : SV_IsFrontFace)
{
    float3 geometricNormal = normalize(input.normal);
    float3 N = isFrontFace ? geometricNormal : -geometricNormal;
    float3 V = normalize(g_eyePosition - input.worldPosition);
    float3 L = normalize(g_sunDirection);
    float3 baseColor = saturate(g_vegetationAlbedo.Sample(
        g_vegetationSampler, input.materialUv).bgr * g_vegetationReflectanceScale.rgb);
    float roughness = BeachApplySpecularAA(N, saturate(g_vegetationSurface.x));
    float specular = saturate(g_vegetationSurface.y);
    float ambientOcclusion = saturate(g_vegetationSurface.z);
    float3 incidentRadiance = max(g_sunRadiance, 0.0) * max(g_sunIntensity, 0.0);
    incidentRadiance *= BeachSunVisibility(input.worldPosition, N);
    float3 skyIrradiance = BeachDiffuseIrradiance(N.xzy) * g_sunIntensity;

    float3 color = BeachEvaluateDefaultLitDirect(baseColor, 0.0, specular,
        roughness, N, V, L, incidentRadiance);
    color += BeachEvaluateLambertAmbient(baseColor, 0.0, specular,
        ambientOcclusion, skyIrradiance);
    color += BeachSpecularEnvironment(N.xzy, V.xzy, roughness,
        BeachDielectricF0(specular)) * g_sunIntensity * ambientOcclusion;

    // A thin leaf transmits illumination arriving on the side opposite the
    // viewer. Beer-Lambert absorption accounts for the longer grazing path;
    // interface Fresnel gates the leaf's texture-defined scattering albedo
    // before the Lambertian BTDF is evaluated.
    float backNoL = saturate(dot(-N, L));
    float NoV = saturate(dot(N, V));
    float thickness = max(g_vegetationSurface.w, 0.0);
    float opticalPathLength = thickness / max(backNoL, 1e-3);
    float3 volumeTransmittance = exp(-max(g_vegetationAbsorption.rgb, 0.0) *
        opticalPathLength) * saturate(g_vegetationAbsorption.w);
    float3 F0 = BeachDielectricF0(specular);
    float3 lightInterfaceFresnel = BeachFresnelSchlick(backNoL, F0);
    float3 viewInterfaceFresnel = BeachFresnelSchlick(NoV, F0);
    float3 transmissionBtdf = (1.0 - lightInterfaceFresnel) *
        (1.0 - viewInterfaceFresnel) * baseColor *
        volumeTransmittance / BEACH_PI;
    color += transmissionBtdf * incidentRadiance * backNoL;

    float radialDepth = distance(g_eyePosition, input.worldPosition);
    color = BeachAerialPerspective(color, input.worldPosition.xzy,
        g_eyePosition.xzy, g_sunIntensity);

    VegetationOutput output;
    output.color = float4(max(color, 0.0), 1.0);
    output.depth = radialDepth;
    return output;
}
