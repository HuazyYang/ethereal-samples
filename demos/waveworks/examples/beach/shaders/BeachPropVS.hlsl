#ifndef WW_REGISTER
#ifdef WAVEWORKS_SPACE
#define WW_REGISTER(reg) register(reg, WAVEWORKS_SPACE)
#else
#define WW_REGISTER(reg) register(reg)
#endif
#endif
#define BEACH_SHADOW_VERTEX
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
Texture2D g_gusts : WW_REGISTER(t0);
SamplerState g_materialSampler : WW_REGISTER(s0);
struct PropInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 rootScale : INSTANCE0;
    float4 rotationHeight : INSTANCE1;
};
struct PropOutput
{
    float4 position : SV_Position;
    float3 worldPosition : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
};
PropOutput main(PropInput input)
{
    float2 cs = input.rotationHeight.xy;
    float3 local = input.position*input.rootScale.w;
    local.xz = float2(local.x*cs.x-local.z*cs.y,local.x*cs.y+local.z*cs.x);
    float3 N = input.normal;
    N.xz = float2(N.x*cs.x-N.z*cs.y,N.x*cs.y+N.z*cs.x);
    float2 wind = g_propWind.xy * rsqrt(max(dot(g_propWind.xy,g_propWind.xy),1e-6));
    float2 gustUv = input.rootScale.xz*.025 - wind*g_time*g_propWind.z*.025;
    float speed = max(g_propWind.z,0)*g_gusts.SampleLevel(g_materialSampler,gustUv,0).r;
    float deflection = min(.001875*speed*speed,g_propWind.w)*input.rotationHeight.w;
    float height = max(input.rotationHeight.z,.01);
    float flex = saturate(local.y/height);
    local.xz += wind*deflection*flex*flex;
    // Inverse transpose of the cantilever deformation gradient.
    N.y -= dot(N.xz,wind)*2*deflection*flex/height;
    PropOutput output;
    output.worldPosition = input.rootScale.xyz + local;
#ifdef BEACH_PROP_SHADOW
    output.position = mul(g_worldToShadow,float4(output.worldPosition,1));
#else
    output.position = mul(g_viewProjection,float4(output.worldPosition,1));
#endif
    output.normal = normalize(N);
    output.uv = input.uv;
    return output;
}
