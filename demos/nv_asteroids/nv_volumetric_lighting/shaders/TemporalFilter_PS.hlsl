// TemporalFilter_PS.hlsl
//
// Temporal filtering of the resolved accumulation buffer (ContextImp_D3D11::ApplyLighting_TemporalFilter,
// only with FilterMode::TEMPORAL). Inputs: the resolved accumulation/depth moments of this frame
// (t0/t2) and the filtered result of the previous frame (t1/t3, ping-pong "NvVl::Filtered *").
// Outputs the new filtered accumulation (SV_TARGET0) and filtered depth moments (SV_TARGET1).
//
// The current value is a 3x3 tent-filtered neighbourhood average in tonemapped YCoCg space; the
// history sample is reprojected with g_mHistoryXform, clipped against the neighbourhood luma range
// and blended with a factor attenuated by screen-space motion and by depth-moment disagreement.
// Blob: table ps_TemporalFilter @ 0x1801FE048 (1 entry)

#include "ShaderCommon.hlsli"
#include "PostProcess.hlsli"

Texture2D<float4> tCurrBuffer : register(t0);
Texture2D<float4> tLastBuffer : register(t1);
Texture2D<float2> tCurrDepth : register(t2);
Texture2D<float2> tLastDepth : register(t3);

struct FILTER_OUTPUT
{
    float3 color : SV_TARGET0;
    float2 depth : SV_TARGET1;
};

static const float FILTER_WEIGHTS[3] = { 0.125f, 1.0f, 0.125f };

float3 Tonemap(float3 c)
{
    return c / (c + 1.0f);
}

float3 InverseTonemap(float3 c)
{
    return c / (1.0f - c);
}

float3 RGBToYCoCg(float3 rgb)
{
    float rb = (rgb.r + rgb.b) * 0.25f;
    return float3(rgb.g * 0.5f + rb, (rgb.r - rgb.b) * 0.5f, rgb.g * 0.5f - rb);
}

float3 YCoCgToRGB(float3 ycocg)
{
    float t = ycocg.x - ycocg.z;
    return float3(t + ycocg.y, ycocg.x + ycocg.z, t - ycocg.y);
}

FILTER_OUTPUT main(PS_QUAD_INPUT input)
{
    int2 viewportSize = int2(g_vViewportSize);
    float2 bufferSize = float2(viewportSize);
    int2 basePos = int2(floor(input.vTex * bufferSize));

    // Gather the 3x3 neighbourhood
    float3 samples[9];
    float2 moments[9];
    float weights[9];
    bool valid[9];
    float3 colorSum = float3(0, 0, 0);
    [unroll]
    for (int oy = -1; oy <= 1; ++oy)
    {
        [unroll]
        for (int ox = -1; ox <= 1; ++ox)
        {
            int i = (oy + 1) * 3 + (ox + 1);
            int2 pos = max(min(viewportSize, basePos + int2(ox, oy)), 0);
            float3 color = max(tCurrBuffer.Load(int3(pos, 0)).rgb, 0);
            moments[i] = tCurrDepth.Load(int3(pos, 0));
            samples[i] = RGBToYCoCg(Tonemap(color));
            valid[i] = all(isfinite(color));
            weights[i] = FILTER_WEIGHTS[ox + 1] * FILTER_WEIGHTS[oy + 1];
            if (valid[i])
                colorSum += weights[i] * samples[i];
        }
    }

    // Luma range and total weight of the valid samples
    float maxY = 0;
    float minY = 0;
    float totalWeight = -1;
    [unroll]
    for (int j = 0; j < 9; ++j)
    {
        if (valid[j])
        {
            if (totalWeight <= 0)
            {
                maxY = samples[j].x;
                minY = samples[j].x;
                totalWeight = weights[j];
            }
            else
            {
                maxY = max(maxY, samples[j].x);
                minY = min(minY, samples[j].x);
                totalWeight += weights[j];
            }
        }
    }
    float3 currColor = (totalWeight > 0) ? colorSum / totalWeight : float3(0, 0, 0);

    float2 momentSum = float2(0, 0);
    [unroll]
    for (int k = 0; k < 9; ++k)
    {
        if (valid[k])
            momentSum += weights[k] * moments[k];
    }
    float2 currMoments = (totalWeight > 0) ? momentSum / totalWeight : float2(1, 1);

    // Reproject into the previous frame
    float z = currMoments.x;
    float depth = z * ((g_fZFar / g_fZNear) + 1.0f) / ((z * g_fZFar) / g_fZNear + 1.0f);
    float2 clipPos = input.vTex * float2(2, -2) + float2(-1, 1);
    float4 lastClip = mul(g_mHistoryXform, float4(clipPos, depth, 1));
    float2 lastPos = lastClip.xy / lastClip.w;
    float2 lastTex = saturate(lastPos * float2(0.5f, -0.5f) + 0.5f);
    int2 lastPixel = int2(bufferSize * lastTex);
    float3 lastColor = tLastBuffer.Load(int3(lastPixel, 0)).rgb;
    float2 lastMoments = tLastDepth.Load(int3(lastPixel, 0));

    // Clip the history luma against the neighbourhood range
    float3 history = all(isfinite(lastColor)) ? RGBToYCoCg(Tonemap(lastColor)) : currColor;
    float clampedY = max(minY, min(maxY, history.x));
    float3 historyDelta = history - currColor;
    float clipRatio = (clampedY - currColor.x) / historyDelta.x;
    clipRatio = (abs(historyDelta.x) > 0.0001f) ? abs(clipRatio) : 1.0f;
    historyDelta = clipRatio * historyDelta;

    // Blend factor: reject off-screen history, attenuate with motion and depth disagreement
    float2 motion = lastPos - clipPos;
    float blend = all(abs(lastPos) <= 1) ? g_fHistoryFactor : 0;
    motion = motion * g_vViewportSize * g_vViewportSize_Inv.x;
    float motionFactor = saturate(1 - length(motion) / g_fFilterThreshold);
    blend = blend * (motionFactor * motionFactor * motionFactor);
    float depthDelta = currMoments.x - lastMoments.x;
    float currVariance = currMoments.y - currMoments.x * currMoments.x;
    float lastVariance = lastMoments.y - lastMoments.x * lastMoments.x;
    float sigma = max(abs(currVariance) + abs(lastVariance), 0.0001f);
    blend *= sigma / (sigma + saturate(abs(depthDelta) - sigma));
    float blendClamped = min(blend, 0.98f);

    FILTER_OUTPUT output;
    float3 result = (blend > 0) ? currColor + blendClamped * historyDelta : currColor;
    output.depth = (blend > 0) ? currMoments + blendClamped * (lastMoments - currMoments) : currMoments;
    output.color = InverseTonemap(YCoCgToRGB(result));
    return output;
}
