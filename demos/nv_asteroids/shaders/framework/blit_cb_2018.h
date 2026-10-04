#ifndef ASTEROIDS_BLIT_CB_2018_H
#define ASTEROIDS_BLIT_CB_2018_H

// 2018 framework BlitConstants (cbuffer c_Blit, b0), from the sharpen_ps / rect_vs reflection.
// Differs from donut main's blit_cb.h: 'sourceSlice' sits at +32 and 'sharpenFactor' at +36.

#ifdef __cplusplus
namespace blit2018
{
#endif

struct BlitConstants
{
    float2  sourceOrigin;   //  0
    float2  sourceSize;     //  8

    float2  targetOrigin;   // 16
    float2  targetSize;     // 24

    uint    sourceSlice;    // 32
    float   sharpenFactor;  // 36
};

#ifdef __cplusplus
} // namespace blit2018
static_assert(sizeof(blit2018::BlitConstants) == 40, "BlitConstants layout");
#endif

#endif // ASTEROIDS_BLIT_CB_2018_H
