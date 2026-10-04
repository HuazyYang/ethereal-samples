// Apply_PS.hlsl
//
// Composites the volumetric lighting onto the application's scene render target
// (ContextImp_D3D11::ApplyLighting_Composite). Uses dual-source blending (bs_Additive_Modulate_:
// dst = src0 * BlendFactor + dst * src1): SV_TARGET0 carries the in-scattered light, SV_TARGET1 the
// transmittance the scene colour is multiplied with.
// Blobs: table ps_Apply @ 0x1801FF3C0 (32 entries, 18 used)
//   SAMPLEMODE   (bit 0)    scene depth single-sampled / MSAA (ContextDesc::framebuffer.uSamples > 1)
//   UPSAMPLEMODE (bits 1-2) PostprocessDesc::eUpsampleQuality: 0 point, 1 bilinear, 2 bilateral
//   FOGMODE      (bits 3-4) 0 no fog, 1 analytic fog except on the sky (bIgnoreSkyFog), 2 fog everywhere

#include "ShaderCommon.hlsli"
#include "PostProcess.hlsli"

#define UPSAMPLEMODE_POINT 0
#define UPSAMPLEMODE_BILINEAR 1
#define UPSAMPLEMODE_BILATERAL 2

#define FOGMODE_NONE 0
#define FOGMODE_NOSKY 1
#define FOGMODE_FULL 2

Texture2D<float4> tGodraysBuffer : register(t0);
#if (SAMPLEMODE == SAMPLEMODE_MSAA)
Texture2DMS<float> tSceneDepth : register(t1);
#else
Texture2D<float> tSceneDepth : register(t1);
#endif
Texture2D<float2> tGodraysDepth : register(t2);

struct APPLY_OUTPUT
{
    float4 inscatter : SV_TARGET0;
    float4 transmission : SV_TARGET1;
};

// Hardware depth -> linear depth (normalized to the far plane)
float LinearizeDepth(float d)
{
    return (d * g_fZNear) / (g_fZFar - (g_fZFar - g_fZNear) * d);
}

float3 Tonemap(float3 c)
{
    return c / (c + 1.0f);
}

float3 InverseTonemap(float3 c)
{
    return c / (1.0f - c);
}

APPLY_OUTPUT main(PS_QUAD_INPUT input
#if (SAMPLEMODE == SAMPLEMODE_MSAA)
    , uint sampleID : SV_SAMPLEINDEX
#endif
    )
{
    float2 uv = input.vTex * g_vViewportSize * g_vBufferSize_Inv;
#if (SAMPLEMODE == SAMPLEMODE_MSAA)
    float sceneDepth = tSceneDepth.Load(int2(input.vTex * g_vOutputViewportSize), sampleID);
#else
    float sceneDepth = tSceneDepth.SampleLevel(sPoint, uv, 0);
#endif
    float sceneZ = LinearizeDepth(sceneDepth);

    float3 inscatter;
#if (UPSAMPLEMODE == UPSAMPLEMODE_POINT)
    inscatter = tGodraysBuffer.SampleLevel(sPoint, uv, 0).rgb;
#elif (UPSAMPLEMODE == UPSAMPLEMODE_BILINEAR)
    inscatter = tGodraysBuffer.SampleLevel(sBilinear, uv, 0).rgb;
#elif (UPSAMPLEMODE == UPSAMPLEMODE_BILATERAL)
    // Joint bilateral upsampling: spatial weight times a depth weight derived from the
    // depth moments (mean, mean^2) stored by the resolve/temporal passes.
    float2 bufferSize = floor(g_vViewportSize);
    float2 centerPos = input.vTex * bufferSize;
    int2 samplePos[9];
    float sampleWeight[9];
    float totalWeight = 0;
    [unroll]
    for (int oy = -1; oy <= 1; ++oy)
    {
        [unroll]
        for (int ox = -1; ox <= 1; ++ox)
        {
            int i = (oy + 1) * 3 + (ox + 1);
            float2 pos = floor(max(min(bufferSize, centerPos + float2(ox, oy)), 0)) + 0.5f;
            float2 delta = pos - centerPos;
            float spatial = max(1.0f - dot(delta, delta) / 64.0f, 0.0f);
            spatial *= spatial;
            spatial *= spatial;
            spatial *= spatial;
            spatial *= spatial;
            spatial *= spatial;
            samplePos[i] = int2(pos);
            float2 moments = tGodraysDepth.Load(int3(samplePos[i], 0));
            float depthDelta = sceneZ - moments.x;
            float sigma = sqrt(abs(moments.y - moments.x * moments.x));
            float depthTerm = max(1.0f - abs(depthDelta) * 10.0f, 0.0f);
            float depthWeight = (depthTerm * depthTerm) * (1.0f - sigma);
            sampleWeight[i] = spatial * depthWeight;
            totalWeight += sampleWeight[i];
        }
    }
    if (totalWeight > 0)
    {
        float3 sum = float3(0, 0, 0);
        [unroll]
        for (int j = 0; j < 9; ++j)
            sum += sampleWeight[j] * Tonemap(tGodraysBuffer.Load(int3(samplePos[j], 0)).rgb);
        inscatter = InverseTonemap(sum / totalWeight);
    }
    else
    {
        inscatter = tGodraysBuffer.SampleLevel(sBilinear, uv, 0).rgb;
    }
#endif

    float3 transmission = float3(1, 1, 1);
#if (FOGMODE != FOGMODE_NONE)
    // Analytic homogeneous fog along the view ray up to the scene depth
#if (FOGMODE == FOGMODE_NOSKY)
    if (sceneZ < 1.0f)
#endif
    {
        float dist = sceneZ * g_fZFar;
        float3 fogColor = g_fMultiScattering * g_vFogLight * g_vScatterPower;
        float3 fogTransmission = exp(dist * -g_vSigmaExtinction);
        float3 fogInscatter = fogColor * (1 - fogTransmission) / g_vSigmaExtinction;
        inscatter += fogInscatter;
        transmission = fogTransmission;
    }
#endif

    APPLY_OUTPUT output;
    output.inscatter = float4(inscatter, 1);
    output.transmission = float4(transmission, 1);
    return output;
}
