/*
* Asteroids (2018 donut framework) - eye adaptation from the luminance histogram
* (passes/exposure_cs.hlsl).
*
* Permutations: HISTOGRAM_BINS=256, SOURCE_ARRAY={0,1} (SOURCE_ARRAY is unused; both binaries are
* identical). Dispatch(1,1,1) of a single thread. Reads t_Histogram (written by histogram_cs, in
* 1/64 fixed point), computes the average luminance with the low/high percentile outliers removed
* (UE4 ComputeAverageLuminanceWithoutOutlier style, not the donut 2021 cdf window), clamps it to
* [minAdaptedLuminance, maxAdaptedLuminance] and blends it with the previous value stored (as float
* bits) in u_Exposure[0].
*/

#include "../framework_passes_cb.h"

Buffer<uint> t_Histogram : register(t0);
RWBuffer<uint> u_Exposure : register(u0);

cbuffer c_ToneMapping : register(b0)
{
    ToneMappingConstants g_ToneMapping;
};

#define FIXED_POINT_FRAC_BITS 6
#define FIXED_POINT_FRAC_MULTIPLIER (1 << FIXED_POINT_FRAC_BITS)

float GetHistogramBucket(uint i)
{
    return float(t_Histogram[i]) / FIXED_POINT_FRAC_MULTIPLIER;
}

float ComputeHistogramSum()
{
    float Sum = 0;

    [loop]
    for (uint i = 0; i < HISTOGRAM_BINS; ++i)
    {
        Sum += GetHistogramBucket(i);
    }

    return Sum;
}

float GetLuminanceFromHistogramPosition(float histogramPosition)
{
    return exp2(histogramPosition * g_ToneMapping.logLuminanceScale + g_ToneMapping.logLuminanceBias);
}

float ComputeAverageLuminanceWithoutOutlier(float MinFractionSum, float MaxFractionSum)
{
    float2 SumWithoutOutliers = 0;

    [loop]
    for (uint i = 0; i < HISTOGRAM_BINS; ++i)
    {
        float LocalValue = GetHistogramBucket(i);

        // remove outlier at lower end
        float Sub = min(LocalValue, MinFractionSum);
        LocalValue = LocalValue - Sub;
        MinFractionSum -= Sub;
        MaxFractionSum -= Sub;

        // remove outlier at upper end
        LocalValue = min(LocalValue, MaxFractionSum);
        MaxFractionSum -= LocalValue;

        float LuminanceAtBucket = GetLuminanceFromHistogramPosition(i / (float)HISTOGRAM_BINS);

        SumWithoutOutliers += float2(LuminanceAtBucket, 1) * LocalValue;
    }

    return SumWithoutOutliers.x / max(0.0001f, SumWithoutOutliers.y);
}

float ComputeEyeAdaptationExposure()
{
    float HistogramSum = ComputeHistogramSum();

    float UnclampedAdaptedLuminance = ComputeAverageLuminanceWithoutOutlier(
        HistogramSum * g_ToneMapping.histogramLowPercentile,
        HistogramSum * g_ToneMapping.histogramHighPercentile);

    return clamp(UnclampedAdaptedLuminance, g_ToneMapping.minAdaptedLuminance, g_ToneMapping.maxAdaptedLuminance);
}

float ComputeEyeAdaptation(float OldExposure, float TargetExposure, float FrameTime)
{
    float Diff = OldExposure - TargetExposure;

    float AdaptationSpeed = (Diff < 0)
        ? g_ToneMapping.eyeAdaptationSpeedUp
        : g_ToneMapping.eyeAdaptationSpeedDown;

    if (AdaptationSpeed <= 0)
        return TargetExposure;

    return TargetExposure + Diff * exp2(-FrameTime * AdaptationSpeed);
}

[numthreads(1, 1, 1)]
void main()
{
    float TargetExposure = ComputeEyeAdaptationExposure();
    float OldExposure = asfloat(u_Exposure[0]);

    u_Exposure[0] = asuint(ComputeEyeAdaptation(OldExposure, TargetExposure, g_ToneMapping.frameTime));
}
