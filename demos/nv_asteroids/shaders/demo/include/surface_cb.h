/*
* Asteroids (2018) - surface shading constant buffer layouts.
*
* Reconstructed from the reflection data of the shipped DXIL (forward_ps, gbuffer_ps,
* material_id_ps, deferred_lighting_ps, shield_ps). Field names, order and offsets are exact.
* This header compiles as HLSL and as C++ (with donut::math types in scope), like donut's *_cb.h:
*   HLSL : #include "surface_cb.h"
*   C++  : #include <donut/core/math/math.h>
*          using namespace donut::math;
*          #include "surface_cb.h"
*/

#ifndef SURFACE_CB_H
#define SURFACE_CB_H

#ifdef __cplusplus
// C++: donut main defines structs with the same names (MaterialConstants, GBufferFillConstants,
// DeferredLightingConstants, LightConstants) in the global namespace, so the 2018 layouts live in
// namespace surface2018; the 2018 light/shadow/probe structs come from light_cb.h (namespace light2018).
#include <cstddef> // offsetof in the layout checks below
#include <donut/shaders/light_types.h> // LightType_* constants
#include "light_cb.h"                  // 2018 LightConstants / ShadowConstants / LightProbeConstants (namespace light2018)
#define SURFACE_ROW_MAJOR
#else
// HLSL: LightConstants, ShadowConstants and LightProbeConstants (2018 layout) are shared with the
// other demo shaders and live in light_cb.h.
#include "light_cb.h"
#define SURFACE_ROW_MAJOR row_major
static const int LightType_None = 0;
static const int LightType_Directional = 1;
static const int LightType_Spot = 2;
static const int LightType_Point = 3;
#endif

#define SURFACE_MAX_LIGHTS          16
#define SURFACE_MAX_SHADOWS         16
#define SURFACE_MAX_LIGHT_PROBES    16

#ifdef __cplusplus
namespace surface2018
{
using ShadowConstants = light2018::ShadowConstants;
using LightProbeConstants = light2018::LightProbeConstants;
using LightConstants = light2018::LightConstants;
#endif

// specularTextureType values (see EvaluateSceneMaterial)
#define SPECULAR_TEXTURE_NONE           0
#define SPECULAR_TEXTURE_INTENSITY      1   // .r = specular intensity
#define SPECULAR_TEXTURE_SPECULAR_GLOSS 2   // .rgb = specular color, .a = gloss
#define SPECULAR_TEXTURE_ORM            3   // .g = roughness, .b = metalness

// cbuffer c_Material : register(b0), member g_Material. Size 64.
struct MaterialConstants
{
    float3  diffuseColor;               // 0
    int     useDiffuseTexture;          // 12
    float3  specularColor;              // 16
    int     specularTextureType;        // 28
    float3  emissiveColor;              // 32
    int     useEmissiveTexture;         // 44
    float   roughness;                  // 48
    float   opacity;                    // 52
    int     useNormalsTexture;          // 56
    int     materialID;                 // 60
};

// cbuffer c_GBuffer : register(b1), member c_GBuffer. Size 432.
struct GBufferFillConstants
{
    SURFACE_ROW_MAJOR float4x4 matWorldToView;            // 0
    SURFACE_ROW_MAJOR float4x4 matViewToClip;             // 64
    SURFACE_ROW_MAJOR float4x4 matWorldToViewRight;       // 128
    SURFACE_ROW_MAJOR float4x4 matViewToClipRight;        // 192
    SURFACE_ROW_MAJOR float4x4 matWorldToClipPrev;        // 256
    SURFACE_ROW_MAJOR float4x4 matWorldToClipRightPrev;   // 320
    float2   viewportScalePrev;         // 384
    float2   viewportBiasPrev;          // 392
    float2   viewportScaleRightPrev;    // 400
    float2   viewportBiasRightPrev;     // 408
    float2   pixelOffset;               // 416
    float2   padding;                   // 424
};

// cbuffer c_Forward : register(b1), member g_Forward. Size 5552.
struct ForwardShadingConstants
{
    SURFACE_ROW_MAJOR float4x4 matWorldToClip;                    // 0
    float4   worldToClipXRight;                 // 64
    float2   shadowMapTextureSize;              // 80
    float2   shadowMapTextureSizeInv;           // 88
    float4   ambientColorTop;                   // 96
    float4   ambientColorBottom;                // 112
    float4   cameraDirectionOrPosition;         // 128
    float4   cameraDirectionOrPositionRight;    // 144
    uint     numLights;                         // 160
    uint     numLightProbes;                    // 164
    uint2    padding;                           // 168

    LightConstants      lights[SURFACE_MAX_LIGHTS];             // 176
    ShadowConstants     shadows[SURFACE_MAX_SHADOWS];           // 1712
    LightProbeConstants lightProbes[SURFACE_MAX_LIGHT_PROBES];  // 3504
};

// cbuffer c_Deferred : register(b0), member g_Deferred. Size 5648.
struct DeferredLightingConstants
{
    SURFACE_ROW_MAJOR float4x4 matClipToView;                     // 0
    SURFACE_ROW_MAJOR float4x4 matViewToWorld;                    // 64
    float2   shadowMapTextureSize;              // 128
    int      gbufferArraySlice;                 // 136
    float    indirectDiffuseScale;              // 140
    float4   ambientColorTop;                   // 144
    float4   ambientColorBottom;                // 160
    float4   cameraDirectionOrPosition;         // 176
    uint     numLights;                         // 192
    uint     numLightProbes;                    // 196
    float2   randomOffset;                      // 200
    float4   noisePattern[4];                   // 208

    LightConstants      lights[SURFACE_MAX_LIGHTS];             // 272
    ShadowConstants     shadows[SURFACE_MAX_SHADOWS];           // 1808
    LightProbeConstants lightProbes[SURFACE_MAX_LIGHT_PROBES];  // 3600
};

// cbuffer g_Shield : register(b0), member g_Shield. Size 112.
struct ShieldConstants
{
    SURFACE_ROW_MAJOR float4x4 matClipToTranslatedWorld;  // 0
    float3   shipPosition;              // 64
    float    distanceToShip;            // 76
    float    shieldRadius;              // 80
    float    shieldIntensity;           // 84
    float    time;                      // 88
    float    padding;                   // 92
    float3   shieldDirection;           // 96
    float    padding2;                  // 108
};

#ifdef __cplusplus
static_assert(sizeof(LightConstants) == 96, "LightConstants layout");
static_assert(sizeof(ShadowConstants) == 112, "ShadowConstants layout");
static_assert(sizeof(LightProbeConstants) == 128, "LightProbeConstants layout");
static_assert(sizeof(MaterialConstants) == 64, "MaterialConstants layout");
static_assert(sizeof(GBufferFillConstants) == 432, "GBufferFillConstants layout");
static_assert(offsetof(ForwardShadingConstants, lights) == 176, "ForwardShadingConstants layout");
static_assert(offsetof(ForwardShadingConstants, shadows) == 1712, "ForwardShadingConstants layout");
static_assert(offsetof(ForwardShadingConstants, lightProbes) == 3504, "ForwardShadingConstants layout");
static_assert(sizeof(ForwardShadingConstants) == 5552, "ForwardShadingConstants layout");
static_assert(offsetof(DeferredLightingConstants, noisePattern) == 208, "DeferredLightingConstants layout");
static_assert(offsetof(DeferredLightingConstants, lights) == 272, "DeferredLightingConstants layout");
static_assert(offsetof(DeferredLightingConstants, shadows) == 1808, "DeferredLightingConstants layout");
static_assert(offsetof(DeferredLightingConstants, lightProbes) == 3600, "DeferredLightingConstants layout");
static_assert(sizeof(DeferredLightingConstants) == 5648, "DeferredLightingConstants layout");
static_assert(sizeof(ShieldConstants) == 112, "ShieldConstants layout");
} // namespace surface2018
#endif

#endif // SURFACE_CB_H
