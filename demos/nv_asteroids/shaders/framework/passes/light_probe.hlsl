/*
* Asteroids (2018 donut framework) - light probe processing (passes/light_probe.hlsl).
*
* Entry points (no permutations):
*   cubemap_gs           gs  - replicates a fullscreen triangle to the 6 cube faces (instance = face)
*   mip_ps               ps  - copies the environment cube map (LOD 0 of the bound view) into a face
*   diffuse_probe_ps     ps  - cosine-weighted irradiance convolution
*   specular_probe_ps    ps  - GGX pre-filtered radiance with filtered importance sampling
*   environment_brdf_ps  ps  - split-sum environment BRDF LUT (uv.x = NdotV, uv.y = roughness)
*
* Differences from donut 2021 light_probe.hlsl, all reproduced on purpose:
*   - GenerateBasis bends the "up" vector towards N when N is close to +-Y instead of switching axes.
*   - diffuse/specular rotate the tangent frame per pixel by a hash of SV_Position (angle in [0, 3.1415)).
*   - diffuse weights each sample by NdotL / (2 pi) and divides by the sum of NdotL.
*   - specular samples GGX with alpha = roughness^2 but evaluates D (for the pdf) with alpha = roughness.
*   - environment_brdf reflects -N (not -V) about H, so LdotH == NdotH and the LUT degenerates to
*     roughly G_Smith / NdotV in the red channel and ~0 in green; recon/assets/media/EnvironmentBrdf.dds
*     matches this.
*/

#include "../framework_passes_cb.h"

struct GSInput
{
    float4 position : SV_Position;
    float2 uv : UV;
};

struct GSOutput
{
    GSInput input;
    uint arrayIndex : SV_RenderTargetArrayIndex;
};

[maxvertexcount(3)]
[instance(6)]
void cubemap_gs(
    triangle GSInput Input[3],
    uint instanceID : SV_GSInstanceID,
    inout TriangleStream<GSOutput> Output)
{
    GSOutput OutputVertex;
    OutputVertex.arrayIndex = instanceID;

    OutputVertex.input = Input[0];
    Output.Append(OutputVertex);
    OutputVertex.input = Input[1];
    Output.Append(OutputVertex);
    OutputVertex.input = Input[2];
    Output.Append(OutputVertex);
}

cbuffer c_LightProbe : register(b0)
{
    LightProbeConstants g_LightProbe;
};

TextureCube t_EnvironmentMap : register(t0);
SamplerState s_EnvironmentMapSampler : register(s0);

static const float M_PI = 3.14159265f;

// normalize() as DXC 1.2 lowered it (fmul/fadd dot product, Sqrt, three fdiv). DXC 1.8 lowers the intrinsic to
// Dot3 + Rsqrt, whose lower precision visibly changes environment_brdf_ps at the smallest roughness values
// (H ~ N): with the intrinsic, rows 0-2 of the 64x64 LUT come out up to 2.4x too low on the GPU.
float3 normalize2018(float3 v)
{
    return v / sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

float3 uvToDirection(float2 uv, uint face)
{
    float3 direction = float3(uv.x * 2 - 1, 1 - uv.y * 2, 1);

    switch (face)
    {
    case 0: direction = float3(direction.z, direction.y, -direction.x); break;
    case 1: direction = float3(-direction.z, direction.y, direction.x); break;
    case 2: direction = float3(direction.x, direction.z, -direction.y); break;
    case 3: direction = float3(direction.x, -direction.z, direction.y); break;
    case 4: direction = float3(direction.x, direction.y, direction.z); break;
    case 5: direction = float3(-direction.x, direction.y, -direction.z); break;
    }

    return normalize2018(direction);
}

struct Basis
{
    float3 tangent;
    float3 bitangent;
    float3 normal;
};

Basis GenerateBasis(float3 N)
{
    float3 up = float3(0, 1, 0);

    // '&' instead of '&&' so that HLSL 2021 does not turn this into two branches
    if ((abs(N.y) > abs(N.x)) & (abs(N.y) > abs(N.z)))
    {
        float2 flipped;
        if (N.y < 0)
            flipped = N.xy;
        else
            flipped = -N.xy;

        up.xy = lerp(up.xy, flipped, saturate(length(N.xz) * 1.5));
    }

    Basis basis;
    basis.tangent = normalize2018(cross(N, up));
    basis.bitangent = normalize2018(cross(basis.tangent, N));
    basis.normal = N;
    return basis;
}

// Rotates the tangent frame around the normal by a per-pixel pseudo-random angle.
Basis RotateBasis(Basis basis, float2 pixelPosition)
{
    float angle = frac(sin(dot(pixelPosition, float2(12.9898, 78.233))) * 43758.5453) * 3.1415;
    float sinAngle = sin(angle);
    float cosAngle = cos(angle);

    Basis rotated;
    rotated.bitangent = basis.bitangent * cosAngle + basis.tangent * sinAngle;
    rotated.tangent = basis.tangent * cosAngle - basis.bitangent * sinAngle;
    rotated.normal = basis.normal;
    return rotated;
}

float3 TangentToWorld(float3 v, Basis basis)
{
    return basis.tangent * v.x + basis.bitangent * v.y + basis.normal * v.z;
}

float radicalInverse(uint i)
{
    i = (i & 0x55555555) << 1 | (i & 0xAAAAAAAA) >> 1;
    i = (i & 0x33333333) << 2 | (i & 0xCCCCCCCC) >> 2;
    i = (i & 0x0F0F0F0F) << 4 | (i & 0xF0F0F0F0) >> 4;
    i = (i & 0x00FF00FF) << 8 | (i & 0xFF00FF00) >> 8;
    i = (i << 16) | (i >> 16);
    return float(i) * 2.3283064365386963e-10f;
}

float2 Hammersley(uint i, uint N)
{
    return float2(float(i) / float(N), radicalInverse(i));
}

// GGX half vector in tangent space; a2 = alpha^2 = roughness^4
float3 ImportanceSampleGGX(float2 random, float a2)
{
    float phi = 2 * M_PI * random.x;
    float cosTheta = sqrt((1 - random.y) / (1 + (a2 - 1) * random.y));
    float sinTheta = sqrt(1 - cosTheta * cosTheta);

    return float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
}

void mip_ps(
    in GSOutput i_input,
    out float4 o_color : SV_Target0)
{
    float3 direction = uvToDirection(i_input.input.uv, i_input.arrayIndex);
    o_color = t_EnvironmentMap.SampleLevel(s_EnvironmentMapSampler, direction, 0);
}

void diffuse_probe_ps(
    in GSOutput i_input,
    out float4 o_color : SV_Target0)
{
    float3 N = uvToDirection(i_input.input.uv, i_input.arrayIndex);
    Basis basis = RotateBasis(GenerateBasis(N), i_input.input.position.xy);

    float4 accumulation = 0;
    float totalWeight = 0;

    for (uint i = 0; i < g_LightProbe.sampleCount; i++)
    {
        float2 random = Hammersley(i, g_LightProbe.sampleCount);
        float phi = 2 * M_PI * random.y;
        float r = sqrt(random.x);
        float3 localL = float3(r * cos(phi), r * sin(phi), sqrt(max(0, 1 - random.x)));

        float3 L = normalize2018(TangentToWorld(localL, basis));
        float NdotL = dot(N, L);

        if (NdotL > 0)
        {
            float4 color = t_EnvironmentMap.SampleLevel(s_EnvironmentMapSampler, L, g_LightProbe.lodBias);
            float weight = NdotL / (2 * M_PI);
            accumulation += color * weight;
            totalWeight += NdotL;
        }
    }

    o_color = accumulation / totalWeight;
}

void specular_probe_ps(
    in GSOutput i_input,
    out float4 o_color : SV_Target0)
{
    float3 N = uvToDirection(i_input.input.uv, i_input.arrayIndex);
    Basis basis = RotateBasis(GenerateBasis(N), i_input.input.position.xy);

    float4 accBrdf = 0;
    float accBrdfWeight = 0;

    for (uint i = 0; i < g_LightProbe.sampleCount; i++)
    {
        float2 random = Hammersley(i, g_LightProbe.sampleCount);
        float a = g_LightProbe.roughness * g_LightProbe.roughness;
        float3 H = normalize2018(TangentToWorld(ImportanceSampleGGX(random, a * a), basis));
        float3 L = reflect(-N, H);
        float NdotL = dot(N, L);

        if (NdotL > 0)
        {
            float NdotH = saturate(dot(N, H));
            float LdotH = saturate(dot(L, H));

            // GGX D evaluated with alpha = roughness (not roughness^2 as used for sampling)
            float a2 = g_LightProbe.roughness * g_LightProbe.roughness;
            float d = (NdotH * a2 - NdotH) * NdotH + 1;
            float D = a2 / (d * d) * (1 / M_PI);
            float pdf = D * NdotH / (4 * LdotH);

            // GPU Gems 3, ch. 20 filtered importance sampling
            float saTexel = 4 * M_PI / (6 * g_LightProbe.inputCubeSize * g_LightProbe.inputCubeSize);
            float saSample = 1 / (float(g_LightProbe.sampleCount) * pdf);
            float mipLevel = 0.5 * log2(saSample / saTexel) + g_LightProbe.lodBias;

            float4 color = t_EnvironmentMap.SampleLevel(s_EnvironmentMapSampler, L, mipLevel);
            accBrdf += color * NdotL;
            accBrdfWeight += NdotL;
        }
    }

    o_color = accBrdf / accBrdfWeight;
}

void environment_brdf_ps(
    in GSInput i_input,
    out float4 o_color : SV_Target0)
{
    const uint NumSamples = 1024;

    float NoV = i_input.uv.x;
    float roughness = i_input.uv.y;

    float3 V = float3(sqrt(1 - NoV * NoV), 0, NoV);
    float3 N = float3(0, 0, 1);
    Basis basis = GenerateBasis(N);
    float NdotV = dot(N, V);

    // Schlick-GGX geometry term, k = (roughness + 1)^2 / 8
    float k = (roughness + 1) * (roughness + 1) / 8;
    float G_V = NdotV / (NdotV * (1 - k) + k);

    float2 accumulation = 0;

    for (uint i = 0; i < NumSamples; i++)
    {
        float2 random = Hammersley(i, NumSamples);
        float a = roughness * roughness;
        float3 H = normalize2018(TangentToWorld(ImportanceSampleGGX(random, a * a), basis));
        float3 L = reflect(-N, H); // 2018 bug: should be reflect(-V, H)

        float LdotH = saturate(dot(L, H));
        float NdotL = saturate(dot(N, L));
        float G = G_V * (NdotL / (NdotL * (1 - k) + k));

        if (NdotL > 0 && G > 0)
        {
            float NdotH = saturate(dot(N, H));
            float G_Vis = G * LdotH / (NdotH * NdotV);
            float Fc = pow(1 - LdotH, 5);
            accumulation.x += (1 - Fc) * G_Vis;
            accumulation.y += Fc * G_Vis;
        }
    }

    o_color = float4(accumulation / float(NumSamples), 0, 0);
}
