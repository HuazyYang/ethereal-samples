/*
* Asteroids (2018 donut framework) - constant buffer layouts of the framework geometry passes
* that are not already covered by demo/include/surface_cb.h.
*
* GBufferFillConstants (c_GBuffer), ForwardShadingConstants (c_Forward) and MaterialConstants
* (c_Material) live in surface_cb.h and are shared with the demo pixel shaders.
*
* Compiles as HLSL and as C++ (donut convention):
*   HLSL : #include "framework_cb.h"
*   C++  : #include <donut/core/math/math.h>
*          using namespace donut::math;
*          #include "framework_cb.h"
*/

#ifndef FRAMEWORK_CB_H
#define FRAMEWORK_CB_H

#ifdef __cplusplus
#define FRAMEWORK_ROW_MAJOR
#else
#define FRAMEWORK_ROW_MAJOR row_major
#endif

// cbuffer c_Depth : register(b0), member g_Depth. Size 64. (passes/depth_vs.hlsl)
struct DepthPassConstants
{
    FRAMEWORK_ROW_MAJOR float4x4 matWorldToClip;    // 0
};

// Value written to NV_VIEWPORT_MASK by the single-pass-stereo vertex shaders:
// viewport 0 for the left eye (low half) and viewport 1 for the right eye (high half).
#define FRAMEWORK_SPS_VIEWPORT_MASK 0x00020001

#ifdef __cplusplus
static_assert(sizeof(DepthPassConstants) == 64, "DepthPassConstants layout");
#endif

#endif // FRAMEWORK_CB_H
