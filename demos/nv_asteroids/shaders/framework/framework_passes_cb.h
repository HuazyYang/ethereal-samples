/*
* Asteroids (2018 donut framework) - constant buffer layouts of the framework post-processing and
* light probe passes (passes/light_probe.hlsl, histogram_cs.hlsl, exposure_cs.hlsl,
* tonemapping_ps.hlsl, bloom_ps.hlsl).
*
* donut main has structs with the same names (LightProbeConstants, ToneMappingConstants,
* BloomConstants) in donut/shaders/{light_probe,tonemapping,bloom}_cb.h, and BloomConstants has a
* different layout there. On the C++ side these 2018 structs therefore live in namespace
* framework2018.
*
* Compiles as HLSL and as C++ (donut convention):
*   HLSL : #include "../framework_passes_cb.h"
*   C++  : #include <donut/core/math/math.h>
*          using namespace donut::math;
*          #include "framework_passes_cb.h"   // -> framework2018::ToneMappingConstants etc.
*/

#ifndef FRAMEWORK_PASSES_CB_2018_H
#define FRAMEWORK_PASSES_CB_2018_H

#ifdef __cplusplus
#include <cstddef>
namespace framework2018
{
#endif

// cbuffer c_LightProbe : register(b0), member g_LightProbe. Size 16.
// Used by diffuse_probe_ps and specular_probe_ps (mip_ps and environment_brdf_ps have no cbuffer).
struct LightProbeConstants
{
    uint sampleCount;       // 0
    float lodBias;          // 4
    float roughness;        // 8   specular only
    float inputCubeSize;    // 12  specular only: width of mip 0 of t_EnvironmentMap, in texels
};

// cbuffer c_ToneMapping : register(b0), member g_ToneMapping. Size 80. Same layout as donut main.
// Shared by histogram_cs, exposure_cs and tonemapping_ps.
struct ToneMappingConstants
{
    uint2 viewOrigin;                   // 0
    uint2 viewSize;                     // 8

    float logLuminanceScale;            // 16
    float logLuminanceBias;             // 20
    float histogramLowPercentile;       // 24
    float histogramHighPercentile;      // 28

    float eyeAdaptationSpeedUp;         // 32
    float eyeAdaptationSpeedDown;       // 36
    float minAdaptedLuminance;          // 40
    float maxAdaptedLuminance;          // 44

    float frameTime;                    // 48
    float exposureScale;                // 52
    float whitePointInvSquared;         // 56
    uint sourceSlice;                   // 60

    float2 colorLUTTextureSize;         // 64
    float2 colorLUTTextureSizeInv;      // 72
};

// cbuffer c_Bloom : register(b0), member g_Bloom. Size 32.
// 2018 layout: numSamples precedes the padding (donut main swapped them).
struct BloomConstants
{
    float2 pixstep;                     // 0
    float argumentScale;                // 8
    float normalizationScale;           // 12

    float numSamples;                   // 16
    float3 padding;                     // 20
};

#ifdef __cplusplus
static_assert(sizeof(LightProbeConstants) == 16, "LightProbeConstants layout");
static_assert(offsetof(LightProbeConstants, inputCubeSize) == 12, "LightProbeConstants layout");
static_assert(sizeof(ToneMappingConstants) == 80, "ToneMappingConstants layout");
static_assert(offsetof(ToneMappingConstants, logLuminanceScale) == 16, "ToneMappingConstants layout");
static_assert(offsetof(ToneMappingConstants, eyeAdaptationSpeedUp) == 32, "ToneMappingConstants layout");
static_assert(offsetof(ToneMappingConstants, frameTime) == 48, "ToneMappingConstants layout");
static_assert(offsetof(ToneMappingConstants, sourceSlice) == 60, "ToneMappingConstants layout");
static_assert(offsetof(ToneMappingConstants, colorLUTTextureSize) == 64, "ToneMappingConstants layout");
static_assert(offsetof(ToneMappingConstants, colorLUTTextureSizeInv) == 72, "ToneMappingConstants layout");
static_assert(sizeof(BloomConstants) == 32, "BloomConstants layout");
static_assert(offsetof(BloomConstants, numSamples) == 16, "BloomConstants layout");
static_assert(offsetof(BloomConstants, padding) == 20, "BloomConstants layout");
} // namespace framework2018
#endif

#endif // FRAMEWORK_PASSES_CB_2018_H
