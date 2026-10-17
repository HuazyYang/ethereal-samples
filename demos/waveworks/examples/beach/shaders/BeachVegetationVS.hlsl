#ifndef WW_REGISTER
#ifdef WAVEWORKS_SPACE
#define WW_REGISTER(reg) register(reg, WAVEWORKS_SPACE)
#else
#define WW_REGISTER(reg) register(reg)
#endif
#endif
cbuffer BeachFrameConstants : WW_REGISTER(b0)
{
    float4x4 g_viewProjection;
    float3 g_sunDirection; float g_sunIntensity;
    float3 g_eyePosition; float g_haze;
    float3 g_sunRadiance; float g_time;
    float3 g_skyIrradiance; float g_meanSeaLevel;
}

cbuffer BeachVegetationWindConstants : WW_REGISTER(b1)
{
    // xy is the terrain-space xz direction, z is speed, and w is world-to-gust UV scale.
    float4 g_windDirectionSpeed;
    // x is aerodynamic drag, y bending stiffness, z gust advection, and w maximum bend.
    float4 g_windMechanics;
}

Texture2D g_windGusts : WW_REGISTER(t0);
SamplerState g_windSampler : WW_REGISTER(s0);

struct VSInput
{
    float3 root : POSITION;
    float2 facing : FACING;
    float2 bladeOffset : BLADE;
    float2 materialUv : MATERIALUV;
    float windWeight : WINDWEIGHT;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 worldPosition : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float distanceToEye : TEXCOORD2;
    float2 materialUv : TEXCOORD3;
};

VSOutput main(VSInput input)
{
    float2 facing = input.facing * rsqrt(max(dot(input.facing, input.facing), 1e-8));
    // The emitted triangle is left root, right root, tip. Its geometric
    // normal is side x tangent, not tangent x side; reversing this made the
    // two-sided leaf BRDF shade the side facing away from the viewer.
    float3 faceNormal = -float3(facing.x, 0.0, facing.y);
    float3 bladeSide = float3(-facing.y, 0.0, facing.x);
    float2 windDirection = g_windDirectionSpeed.xy * rsqrt(max(
        dot(g_windDirectionSpeed.xy, g_windDirectionSpeed.xy), 1e-8));
    float3 windDirectionWorld = float3(windDirection.x, 0.0, windDirection.y);
    float windSpeed = max(g_windDirectionSpeed.z, 0.0);
    float gustSpatialScale = max(g_windDirectionSpeed.w, 0.0);
    float2 gustUv = input.root.xz * gustSpatialScale - windDirection *
        (g_time * windSpeed * g_windMechanics.z * gustSpatialScale);
    float gust = saturate(g_windGusts.SampleLevel(g_windSampler, gustUv, 0.0).r);

    // Quasi-static Euler-Bernoulli response: aerodynamic load is quadratic in
    // local air speed, while a fixed cantilever's displacement starts at the
    // square of its normalized distance from the root.
    float localWindSpeed = windSpeed * gust;
    float aerodynamicLoad = max(g_windMechanics.x, 0.0) *
        localWindSpeed * localWindSpeed;
    float windWeight = saturate(input.windWeight);
    float compliance = rcp(max(g_windMechanics.y, 1e-5));
    float bend = min(aerodynamicLoad * compliance * windWeight * windWeight,
        max(g_windMechanics.w, 0.0));

    float3 worldPosition = input.root + bladeSide * input.bladeOffset.x +
        float3(0.0, input.bladeOffset.y, 0.0) + windDirectionWorld * bend;
    float bendSlope = input.bladeOffset.y > 1e-5
        ? 2.0 * bend / input.bladeOffset.y : 0.0;
    float3 bladeTangent = normalize(float3(0.0, 1.0, 0.0) +
        windDirectionWorld * bendSlope);
    float3 bentNormal = normalize(cross(bladeSide, bladeTangent));
    // Retain a stable orientation when wind is disabled or facing was degenerate.
    bentNormal = dot(bentNormal, bentNormal) > 1e-6 ? bentNormal : faceNormal;

    VSOutput output;
    output.position = mul(g_viewProjection, float4(worldPosition, 1.0));
    output.worldPosition = worldPosition;
    output.normal = bentNormal;
    output.distanceToEye = distance(g_eyePosition, worldPosition);
    output.materialUv = input.materialUv;
    return output;
}
