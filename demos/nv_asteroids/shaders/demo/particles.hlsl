/*
 * Space dust particles: vertex/pixel shader path and the simulation compute shader.
 * (The mesh-shader path ms_main lives in particles_ms.hlsl and shares particles_cb.h.)
 *
 * Entry points:
 *   vs_main        (vs_6_0)  3 vertices per particle, particlesPerBatch particles per instance
 *   ps_main        (ps_6_0)  soft round sprite
 *   update_cs_main (cs_6_0)  advection through a curl-ish noise field (iq's fbm with analytic derivatives)
 *
 * Reconstructed from DXIL (particles_vs_main, particles_ps_main, particles_update_cs_main).
 * The original source also included NVAPI's nvHLSLExtns.h (NvShaderExtnStruct / g_NvidiaExt appear in
 * the type annotations of all three entry points) for the mesh-shader path; nothing in these three
 * entry points uses it, so it is not included here.
 */

#include "include/particles_cb.h"
#include "include/space_shadows.hlsli"

ConstantBuffer<ParticleConstants> g_Particles : register(b0);

Texture2DArray t_ShadowMapArray : register(t0);
StructuredBuffer<ParticleInfo> t_Particles : register(t1);
RWStructuredBuffer<ParticleInfo> u_Particles : register(u0);
SamplerComparisonState s_ShadowSampler : register(s0);

struct PS_Input
{
    float4 position : SV_Position;
    nointerpolation float4 color : COLOR;
    float2 uv : UV;
};

// Equilateral triangle circumscribing the unit circle.
static const float PI = 3.14159265;

static const float2 TrianglePoints[3] = {
    float2(0, 2),
    float2(1.7320508, -1),
    float2(-1.7320508, -1)
};

// ---------------------------------------------------------------------------------------------
// Lighting
// ---------------------------------------------------------------------------------------------

// Cascaded sun shadow at a world position (donut 2018 deferred-lighting cascade loop).
// (No GetShadow label survives in this blob; the helper name is borrowed from lensflare/fog.)
float GetShadow(LightConstants light, float3 worldPos)
{
    int4 cascadeIndices = light.shadowCascades;
    float2 shadow = 0;
    for (int cascade = 0; cascade < PARTICLES_MAX_SHADOWS; cascade++)
    {
        int shadowIndex = cascadeIndices[cascade];
        if (shadowIndex >= 0)
        {
            float2 cascadeShadow = EvaluateShadowPCF(t_ShadowMapArray, s_ShadowSampler, g_Particles.shadows[shadowIndex], worldPos);

            shadow = saturate(shadow + cascadeShadow * (1.0001 - shadow.y));

            if (shadow.y == 1)
                break;
        }
        else
            break;
    }

    shadow.x += (1 - shadow.y) * light.outOfBoundsShadow;

    return shadow.x;
}

// Computes the clip-space size and the color of one particle.
// Mangled name: ?SetupParticle@@YAXV?$vector@M$02@@V?$vector@M$03@@MMAIAV?$vector@M$01@@AIAV2@@Z
void SetupParticle(float3 worldPos, float4 clipPos, float brightness, float distance, out float2 clipSize, out float4 color)
{
    LightConstants light = g_Particles.light;

    // Keep at least ~0.3 units of screen size; shrinking particles fade out instead.
    float size = max(0.3, clipPos.w * 0.003);
    float alpha = pow(0.3 / size, 1.5);
    alpha *= saturate((1.0 - distance / g_Particles.maxDistance) * 10.0);   // fade out at the far end of the volume
    alpha *= saturate(distance * 0.02 - 1.0);                                // fade out close to the camera

    // Henyey-Greenstein forward scattering (g = 0.9), normalized to 1 in the forward direction,
    // plus an isotropic term.
    // unresolved: 82.51197052 is the exact compiled value; its symbolic origin is unknown
    // (10 / (4 pi) reproduces the second constant bit-exactly).
    const float g = 0.9;
    float cosTheta = dot(worldPos / distance, -light.direction);
    float hg = (1 - g) / sqrt(max(1 + g * g - 2 * g * cosTheta, 0));
    float phase = hg * hg * 82.51197052 * hg + 10.0 / (4.0 * PI);

    // Illuminance of the sun disk: radiance * angularSize^2 / 8
    float irradiance = light.angularSizeOrInvRange * light.angularSizeOrInvRange * (light.radiance * 0.125);

    float shadow = GetShadow(light, worldPos);

    clipSize = g_Particles.screenScale * size;
    color = float4(irradiance * brightness * light.color * (shadow * phase + 0.2), alpha);
}

// ---------------------------------------------------------------------------------------------
// Vertex / pixel shader path
// ---------------------------------------------------------------------------------------------

PS_Input vs_main(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
    float vertexPos = (float(vertexID) + 0.5) / 3.0;
    uint corner = uint(floor(frac(vertexPos) * 3.0));
    uint particleIndex = g_Particles.particlesPerBatch * instanceID + uint(floor(vertexPos));

    ParticleInfo particle = t_Particles[particleIndex];

    float3 worldPos = particle.position + g_Particles.positionOffset;
    float distance = length(worldPos);
    float4 clipPos = mul(float4(worldPos, 1), g_Particles.matWorldToClip);

    PS_Input o;

    if (clipPos.w <= 0 || distance > g_Particles.maxDistance)
    {
        o.position = float4(0, 0, -1, 1);
        o.color = 0;
        o.uv = 0;
        return o;
    }

    float2 clipSize;
    float4 color;
    SetupParticle(worldPos, clipPos, particle.brightness, distance, clipSize, color);

    float2 cornerPos = TrianglePoints[corner];
    o.position = float4(clipPos.xy + clipSize * cornerPos, clipPos.zw);
    o.color = color;
    o.uv = cornerPos;
    return o;
}

float4 ps_main(PS_Input i) : SV_Target
{
    float r = length(i.uv);

    if (r >= 1.0)
        return 0;

    return float4(i.color.rgb, i.color.a * exp(-5.0 * r));
}

// ---------------------------------------------------------------------------------------------
// Simulation
// ---------------------------------------------------------------------------------------------

// Mangled name: ?wrap@@YAXAIAMM@Z
void wrap(inout float x, float size)
{
    if (x < 0)
        x += size;
    else if (x > size)
        x -= size;
}

// Value noise with analytic derivatives, Inigo Quilez ("Noise - value - 3D - deriv"),
// cubic interpolation variant.
float hash(float3 p)
{
    p = 50.0 * frac(p * 0.3183099 + float3(0.71, 0.113, 0.419));
    return -1.0 + 2.0 * frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float4 noised(float3 x)
{
    float3 i = floor(x);
    float3 w = frac(x);

    // cubic interpolation
    float3 u = w * w * (3.0 - 2.0 * w);
    float3 du = 6.0 * w * (1.0 - w);

    float a = hash(i + float3(0.0, 0.0, 0.0));
    float b = hash(i + float3(1.0, 0.0, 0.0));
    float c = hash(i + float3(0.0, 1.0, 0.0));
    float d = hash(i + float3(1.0, 1.0, 0.0));
    float e = hash(i + float3(0.0, 0.0, 1.0));
    float f = hash(i + float3(1.0, 0.0, 1.0));
    float g = hash(i + float3(0.0, 1.0, 1.0));
    float h = hash(i + float3(1.0, 1.0, 1.0));

    float k0 = a;
    float k1 = b - a;
    float k2 = c - a;
    float k3 = e - a;
    float k4 = a - b - c + d;
    float k5 = a - c - e + g;
    float k6 = a - b - e + f;
    float k7 = -a + b + c - d + e - f - g + h;

    return float4(k0 + k1 * u.x + k2 * u.y + k3 * u.z + k4 * u.x * u.y + k5 * u.y * u.z + k6 * u.z * u.x + k7 * u.x * u.y * u.z,
                  du * float3(k1 + k4 * u.y + k6 * u.z + k7 * u.y * u.z,
                              k2 + k5 * u.z + k4 * u.x + k7 * u.z * u.x,
                              k3 + k6 * u.x + k5 * u.y + k7 * u.x * u.y));
}

static const float3x3 m3 = float3x3(
     0.00,  0.80,  0.60,
    -0.80,  0.36, -0.48,
    -0.60, -0.48,  0.64);

static const float3x3 m3i = float3x3(
     0.00, -0.80, -0.60,
     0.80,  0.36, -0.48,
     0.60, -0.48,  0.64);

// fbm of noised() with derivatives (iq). Mangled name: ?fbmd@@YA?AV?$vector@M$03@@V?$vector@M$02@@H@Z
float4 fbmd(float3 x, int octaves)
{
    float f = 1.98;     // could be 2.0
    float s = 0.49;     // could be 0.5
    float a = 0.0;
    float b = 0.5;
    float3 d = 0.0;
    float3x3 m = float3x3(
        1.0, 0.0, 0.0,
        0.0, 1.0, 0.0,
        0.0, 0.0, 1.0);

    for (int i = 0; i < octaves; i++)
    {
        float4 n = noised(x);
        a += b * n.x;               // accumulate values
        d += b * mul(m, n.yzw);     // accumulate derivatives
        b *= s;
        x = f * mul(m3, x);
        m = f * mul(m3i, m);
    }

    return float4(a, d);
}

// Mangled name: ?safeNormalize@@YA?AV?$vector@M$02@@V1@@Z
float3 safeNormalize(float3 v)
{
    float len2 = dot(v, v);
    if (len2 == 0)
        return 0;
    return v / sqrt(len2);
}

[numthreads(PARTICLES_UPDATE_GROUP_SIZE, 1, 1)]
void update_cs_main(uint3 globalIdx : SV_DispatchThreadID)
{
    ParticleInfo particle = u_Particles[globalIdx.x];

    particle.position += particle.velocity * g_Particles.deltaTime;
    wrap(particle.position.x, g_Particles.volumeSize.x);
    wrap(particle.position.z, g_Particles.volumeSize.z);

    float3 force = safeNormalize(fbmd(particle.position * 0.05 + g_Particles.time, 3).yzw) * 10.0;

    particle.velocity += force * g_Particles.deltaTime;
    particle.velocity -= particle.velocity * g_Particles.deltaTime * 0.1;   // drag

    u_Particles[globalIdx.x] = particle;
}
