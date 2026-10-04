/*
 * Constant buffer / structured buffer layouts of the space-FX shaders of the 2018 Asteroids demo:
 * real stars, textured star field, sun disk, planets (+rings), rect pass, lens flare and fog.
 *
 * Reconstructed from the DXIL reflection; struct names, field names, order, types and offsets are
 * exactly the ones recorded in the shipped shaders. Where a shader declares its cbuffer members
 * directly (no struct member), the struct below exists for the C++ side only (__cplusplus) so that
 * the HLSL reflection stays identical to the original.
 *
 * Usage follows the donut convention (see donut/shaders/view_cb.h):
 *   HLSL : #include "space_cb.h"
 *   C++  : #include <donut/core/math/math.h>
 *          using namespace donut::math;
 *          #include "space_cb.h"
 */

#ifndef SPACE_CB_H
#define SPACE_CB_H

#include "light_cb.h"

#ifdef __cplusplus
#define SPACE_ROW_MAJOR
#else
#define SPACE_ROW_MAJOR row_major
#endif

// ---------------------------------------------------------------------------------------------
// real_stars.hlsl
// ---------------------------------------------------------------------------------------------

// Number of stars drawn per instance by real_stars VS (3 vertices each, 192 vertices per instance).
#define REAL_STARS_PER_INSTANCE 64

// StructuredBuffer<StarInstance> t_StarInstances : register(t0)   stride 16
// This is exactly the record format of media/SkyAndStars/Stars.buf (258944 records, sorted by
// ascending magnitude; record 0 is Sirius = SAO 151881).
struct StarInstance
{
    float       Ra;                                     //   0  right ascension, degrees [0, 360)
    float       Dec;                                    //   4  declination, degrees [-90, 90]
    float       Mag;                                    //   8  apparent visual magnitude
    int         CatNumber;                              //  12  SAO catalogue number (not used by the shaders)
};

#ifdef __cplusplus
// cbuffer CelestialConstants : register(b0)   size 84  (members declared directly in the cbuffer)
struct CelestialConstants
{
    float4x4    ViewProjectionToClip;                   //   0  equatorial direction -> clip
    float       ST;                                     //  64  sidereal time        (not used by the shaders)
    float       SinLAT;                                 //  68  sin(observer latitude) (not used by the shaders)
    float       CosLAT;                                 //  72  cos(observer latitude) (not used by the shaders)
    float       Aspect;                                 //  76  viewport width / height
    float       Brightness;                             //  80
};
static_assert(sizeof(CelestialConstants) == 84, "CelestialConstants layout");
#endif

// ---------------------------------------------------------------------------------------------
// RectPass_vs.hlsl   cbuffer cbRectConstants : register(b0) { RectConstants g_param; }   size 128
// ---------------------------------------------------------------------------------------------

struct RectConstants
{
    float4      vertices[4];                            //   0  clip-space corners, indexed by SV_VertexID & 3 (triangle strip)
    float4      directions[4];                          //  64  world-space view direction at each corner (xyz)
};

// ---------------------------------------------------------------------------------------------
// sun_disk_ps.hlsl   cbuffer cbSunDisk : register(b0) { SunConstants g_Sun; }   size 16
// ---------------------------------------------------------------------------------------------

struct SunConstants
{
    float3      color;                                  //   0
    float       padding;                                //  12
};

// ---------------------------------------------------------------------------------------------
// PlanetFx.hlsl / PlanetWithRingsFx.hlsl   cbuffer cbPlanetFx : register(b0) { PlanetConstants g_param; }   size 112
// All positions are camera relative (the camera is at the origin).
// ---------------------------------------------------------------------------------------------

struct PlanetConstants
{
    float3      directionToSun;                         //   0
    float       radiusRatio;                            //  12  planet (inner) radius / atmospheric (outer) radius; >= 1 means no atmosphere
    float3      planetPosition;                         //  16  planet center relative to the camera
    float       atmosphericRadius;                      //  28  outer (atmosphere) radius
    float       atmosphericAlpha;                       //  32  extinction per unit length used for the atmosphere opacity
    float       rotation;                               //  36  added to the longitude texture coordinate
    float       textureLOD;                             //  40
    float       stretchAmount;                          //  44  terminator wrap: saturate(dot(N, L) + stretchAmount)
    float3      sunColor;                               //  48
    float       pad0;                                   //  60
    float       litBrightness;                          //  64
    float       ambientBrightness;                      //  68
    float       atmosphereBrightness;                   //  72
    float       pad1;                                   //  76
    float       atmosphericRadiusRatioSquared;          //  80  (not used by the shaders)
    float       innerRingRadiusSquared;                 //  84  squared distance from the planet center
    float       outerRingRadiusSquared;                 //  88
    float       pad2;                                   //  92
    float4      ringsPlane;                             //  96  xyz = normal, w = d (camera-relative plane equation)
};

// ---------------------------------------------------------------------------------------------
// texturedstarfield_ps.hlsl   cbuffer c_Sky : register(b0) { StarFieldConstants g_Sky; }   size 92
// ---------------------------------------------------------------------------------------------

struct StarFieldConstants
{
    SPACE_ROW_MAJOR float4x4 matClipToTranslatedWorld;  //   0
    float4      directionToSun;                         //  64  (not used by the shader)
    float       nebulaLinearBrightness;                 //  80
    float       starSquareBrightness;                   //  84
    float       starLinearBrightness;                   //  88
};

// ---------------------------------------------------------------------------------------------
// lensflare.hlsl   ConstantBuffer<LensFlareConstants> g_LensFlare : register(b0)   size 804
// ---------------------------------------------------------------------------------------------

#define LENSFLARE_MAX_SHADOWS 5

struct LensFlareConstants
{
    SPACE_ROW_MAJOR float4x4 matWorldToClip;            //   0  (not used by the shaders)
    float4      cameraPos;                              //  64  (not used by the shaders)
    float2      screenScale;                            //  80  render target size (only the ratio y/x is used)
    float2      pos;                                    //  88  sun position relative to the screen center, uv units
    LIGHT2018_NS LightConstants light;                              //  96
    LIGHT2018_NS ShadowConstants shadows[LENSFLARE_MAX_SHADOWS];     // 192
    float       irradiance;                             // 752
#ifdef __cplusplus
    float       _alignPadding[3];                       // 756  HLSL starts the array below on a new register
    float4      pad[3];                                 // 768  HLSL 'float pad[3]': one register per element
#else
    float       pad[3];                                 // 768
#endif
};

// ---------------------------------------------------------------------------------------------
// fog.hlsl   ConstantBuffer<FogConstants> g_Fog : register(b0)   size 704
// ---------------------------------------------------------------------------------------------

#define FOG_MAX_SHADOWS 3

struct FogConstants
{
    SPACE_ROW_MAJOR float4x4 matClipToView;             //   0
    SPACE_ROW_MAJOR float4x4 matViewToWorld;            //  64  camera-relative world (translation removed)
    float4      cameraPos;                              // 128
    float4      noisePattern[4];                        // 144  4x4 ray-start dither pattern
    float3      randomOffset;                           // 208  xy: dither pattern offset (pixels), z: temporal jitter
    float       maxShadowDistance;                      // 220
    float       projectionA;                            // 224  linearDepth = projectionB / (depth - projectionA)
    float       projectionB;                            // 228
    float2      padding2;                               // 232
    float       meanHeight;                             // 240  fog layer center (world y)
    float       thickness;                              // 244  gaussian sigma of the fog layer
    float       densityScale;                           // 248
    float       visibilityThreshold;                    // 252  ray marching stops below this transparency
    LIGHT2018_NS LightConstants light;                              // 256
    LIGHT2018_NS ShadowConstants shadows[FOG_MAX_SHADOWS];           // 352
    float3      color;                                  // 688
    float       intensity;                              // 700
};

#ifdef __cplusplus
// cbuffer CB : register(b0)   size 8  (show_cubemap_ps; members declared directly in the cbuffer)
struct ShowCubemapConstants
{
    uint        g_ArrayIndex;                           //   0
    uint        g_MipLevel;                             //   4
};

static_assert(sizeof(StarInstance) == 16, "StarInstance layout");
static_assert(sizeof(RectConstants) == 128, "RectConstants layout");
static_assert(sizeof(SunConstants) == 16, "SunConstants layout");
static_assert(sizeof(PlanetConstants) == 112, "PlanetConstants layout");
static_assert(offsetof(PlanetConstants, ringsPlane) == 96, "PlanetConstants layout");
static_assert(offsetof(StarFieldConstants, nebulaLinearBrightness) == 80, "StarFieldConstants layout");
static_assert(offsetof(LensFlareConstants, light) == 96, "LensFlareConstants layout");
static_assert(offsetof(LensFlareConstants, shadows) == 192, "LensFlareConstants layout");
static_assert(offsetof(LensFlareConstants, irradiance) == 752, "LensFlareConstants layout");
static_assert(offsetof(LensFlareConstants, pad) == 768, "LensFlareConstants layout");
static_assert(sizeof(LensFlareConstants) >= 804, "LensFlareConstants layout");
static_assert(offsetof(FogConstants, randomOffset) == 208, "FogConstants layout");
static_assert(offsetof(FogConstants, meanHeight) == 240, "FogConstants layout");
static_assert(offsetof(FogConstants, light) == 256, "FogConstants layout");
static_assert(offsetof(FogConstants, shadows) == 352, "FogConstants layout");
static_assert(offsetof(FogConstants, color) == 688, "FogConstants layout");
static_assert(sizeof(FogConstants) == 704, "FogConstants layout");
#endif

#endif // SPACE_CB_H
