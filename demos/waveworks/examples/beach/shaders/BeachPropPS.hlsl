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
};
cbuffer PropMaterialConstants : WW_REGISTER(b1)
{
    float4 g_propSurface;
    float4 g_propWind;
};
Texture2D g_propAlbedo : WW_REGISTER(t0);
Texture2D g_propNormal : WW_REGISTER(t1);
Texture2D g_propArm : WW_REGISTER(t2);
SamplerState g_materialSampler : WW_REGISTER(s0);
struct PropInput
{
    float4 position : SV_Position;
    float3 worldPosition : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
};
#ifdef BEACH_PROP_SHADOW
void main(PropInput input)
{
    clip(g_propAlbedo.Sample(g_materialSampler,input.uv).a-g_propSurface.w);
}
#else
struct PropOutput
{
    float4 color : SV_Target0;
    float depth : SV_Target1;
};
PropOutput main(PropInput input)
{
    float4 albedo = g_propAlbedo.Sample(g_materialSampler,input.uv).bgra;
    clip(albedo.a-g_propSurface.w);
    float3 V = normalize(g_eyePosition-input.worldPosition);
    float3 geometricNormal = normalize(input.normal);
    // Face the geometric normal towards the viewer for two-sided scanned leaves.
    if (dot(geometricNormal,V) < 0) geometricNormal = -geometricNormal;
    float3 tangentNormal = g_propNormal.Sample(g_materialSampler,input.uv).bgr*2-1;
    float meanLength = saturate(length(tangentNormal));
    float variance = (1-meanLength)/max(meanLength,.05);
    float3 dx = ddx(input.worldPosition), dy = ddy(input.worldPosition);
    float2 du = ddx(input.uv), dv = ddy(input.uv);
    float3 px = cross(dy,geometricNormal), py = cross(geometricNormal,dx);
    float3 T = px*du.x+py*dv.x, B = px*du.y+py*dv.y;
    float inv = rsqrt(max(max(dot(T,T),dot(B,B)),1e-8));
    float3 N = normalize((T*tangentNormal.x+B*tangentNormal.y)*inv +
                          geometricNormal*max(tangentNormal.z,.05));
    float3 arm = g_propArm.Sample(g_materialSampler,input.uv).bgr;
    float roughness = BeachApplySpecularAA(N,sqrt(sqrt(saturate(pow(arm.g,4)+variance))));
    float3 L = normalize(g_sunDirection);
    float visibility = BeachSunVisibility(input.worldPosition,geometricNormal);
    float3 incident = g_sunRadiance*g_sunIntensity*visibility;
    float3 color = BeachEvaluateDefaultLitDirect(albedo.rgb,0,g_propSurface.z,
        roughness,N,V,L,incident);
    color += BeachEvaluateLambertAmbient(albedo.rgb,0,g_propSurface.z,arm.r,
        BeachDiffuseIrradiance(N.xzy)*g_sunIntensity);
    color += BeachSpecularEnvironment(N.xzy,V.xzy,roughness,
        BeachDielectricF0(g_propSurface.z))*g_sunIntensity*arm.r;
    if (g_propSurface.x > .5)
    {
        float backNoL = saturate(dot(-N,L));
        float3 F0 = BeachDielectricF0(g_propSurface.z);
        float3 transmission = exp(-float3(2600,700,3500)*.0003/max(backNoL,.001))*.65;
        float3 btdf = (1-BeachFresnelSchlick(backNoL,F0))*
            (1-BeachFresnelSchlick(saturate(dot(N,V)),F0))*albedo.rgb*transmission/BEACH_PI;
        color += btdf*incident*backNoL;
    }
    PropOutput output;
    output.color = float4(BeachAerialPerspective(color,input.worldPosition.xzy,
        g_eyePosition.xzy,g_sunIntensity),1);
    output.depth = distance(g_eyePosition,input.worldPosition);
    return output;
}
#endif
