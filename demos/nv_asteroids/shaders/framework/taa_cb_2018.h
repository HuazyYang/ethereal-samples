#ifndef ASTEROIDS_TAA_CB_2018_H
#define ASTEROIDS_TAA_CB_2018_H

// 2018 framework TemporalAntiAliasingConstants (cbuffer c_TemporalAA, b0, 128 bytes), from the passes/taa_cs and
// passes/motion_vectors_ps reflection and the C++ writers at Asteroids.exe 0x140096BC0 / 0x140096380.
// Differs from donut main's taa_cb.h: no PQ constants, and the whole struct is padded to 128 bytes.

#ifdef __cplusplus
namespace taa2018
{
#endif

struct TemporalAntiAliasingConstants
{
    float4x4 reprojectionMatrix;    //   0  motion vector pass only

    float2  previousViewOrigin;     //  64  previous view extent, shrunk by a 1 pixel margin on each side
    float2  previousViewSize;       //  72

    float2  viewOrigin;             //  80  current view origin (the resolve dispatch covers viewSize pixels)
    float2  viewSize;               //  88

    float2  sourceTextureSizeInv;   //  96  1 / size of the history texture
    float   clampingFactor;         // 104  history clamp width in sigmas, < 0 = no clamping
    float   newFrameWeight;         // 108  weight of the current frame in the exponential blend

    uint    stencilMask;            // 112  motion vector pass: pixels with these stencil bits are skipped
    uint3   padding;                // 116
};

#ifdef __cplusplus
} // namespace taa2018
static_assert(sizeof(taa2018::TemporalAntiAliasingConstants) == 128, "TemporalAntiAliasingConstants layout");
#endif

#endif // ASTEROIDS_TAA_CB_2018_H
