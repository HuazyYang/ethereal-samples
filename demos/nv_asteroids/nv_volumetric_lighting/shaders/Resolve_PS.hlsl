// Resolve_PS.hlsl
//
// Resolves the (possibly multisampled) accumulation buffer into "NvVl::Resolved Accumulation"
// (SV_TARGET0) and "NvVl::Resolved Depth" (SV_TARGET1, R16G16_FLOAT: first and second moment of
// the linear depth) (ContextImp_D3D11::ApplyLighting_Resolve).
// Every output pixel is a weighted 3x3 (x samples) reconstruction filter over the accumulation
// buffer; samples with non-finite values are rejected.
// Blobs: table ps_Resolve @ 0x1801FE2D0 (2 entries)
//   SAMPLEMODE: accumulation buffer single-sampled or MSAA (ContextDesc::eInternalSampleMode).

#include "ShaderCommon.hlsli"
#include "PostProcess.hlsli"

#if (SAMPLEMODE == SAMPLEMODE_MSAA)
Texture2DMS<float4> tGodraysBuffer : register(t0);
Texture2DMS<float> tGodraysDepth : register(t1);
#else
Texture2D<float4> tGodraysBuffer : register(t0);
Texture2D<float> tGodraysDepth : register(t1);
#endif

struct RESOLVE_OUTPUT
{
    float4 color : SV_TARGET0;
    float2 depth : SV_TARGET1;
};

// Hardware depth -> linear view depth
float LinearizeDepth(float d)
{
    return (d * g_fZNear) / (g_fZFar - (g_fZFar - g_fZNear) * d);
}

// Filter weight of a sample at the given offset (in internal-buffer pixels)
float FilterWeight(float2 offset)
{
    float2 scaledOffset = g_fResMultiplier * offset;
    float k = max(1.0f - (scaledOffset.x * scaledOffset.x + scaledOffset.y * scaledOffset.y) / 64.0f, 0.0f);
    k = k * k;
    k = k * k;
    k = k * k;
    k = k * k;
    return k * k;
}

void AccumulateSample(inout float3 totalColor, inout float3 totalDepth, float3 color, float depth, float2 offset)
{
    float z = LinearizeDepth(depth);
    if (all(isfinite(color)))
    {
        float weight = FilterWeight(offset);
        totalColor += weight * color;
        totalDepth.x += weight * z;
        totalDepth.y += z * weight * z;
        totalDepth.z += weight;
    }
}

RESOLVE_OUTPUT main(PS_QUAD_INPUT input)
{
#if (SAMPLEMODE == SAMPLEMODE_MSAA)
    uint bufferWidth, bufferHeight, sampleCount;
    tGodraysBuffer.GetDimensions(bufferWidth, bufferHeight, sampleCount);
#endif
    int2 basePos = int2(input.vTex * g_vViewportSize);
    float3 totalColor = float3(0, 0, 0);
    float3 totalDepth = float3(0, 0, 0);
    [unroll]
    for (int ox = -1; ox <= 1; ++ox)
    {
        int x = basePos.x + ox;
        if (x < 0 || float(x) >= g_vViewportSize.x)
            continue;
        [unroll]
        for (int oy = -1; oy <= 1; ++oy)
        {
            int y = basePos.y + oy;
            if (y < 0 || float(y) >= g_vViewportSize.y)
                continue;
            int2 pos = basePos + int2(ox, oy);
#if (SAMPLEMODE == SAMPLEMODE_MSAA)
            for (uint s = 0; s < sampleCount; ++s)
            {
                float2 offset = tGodraysBuffer.GetSamplePosition(s) + float2(ox, oy);
                AccumulateSample(totalColor, totalDepth, tGodraysBuffer.Load(pos, s).rgb, tGodraysDepth.Load(pos, s), offset);
            }
#else
            AccumulateSample(totalColor, totalDepth, tGodraysBuffer.Load(int3(pos, 0)).rgb, tGodraysDepth.Load(int3(pos, 0)), float2(ox, oy));
#endif
        }
    }

    RESOLVE_OUTPUT output;
    output.color = float4((totalDepth.z > 0) ? totalColor / totalDepth.z : float3(0, 0, 0), 0);
    output.depth = (totalDepth.z > 0) ? totalDepth.xy / totalDepth.z : float2(1, 1);
    return output;
}
