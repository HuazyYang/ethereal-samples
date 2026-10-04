/*
 * Light / shadow constant layouts of the 2018 (early donut) framework, as embedded in the
 * cbuffers of the Asteroids demo shaders (particles, lensflare, fog, deferred lighting, ...).
 *
 * Reconstructed from the DXIL reflection. Field names, order, types and offsets are exactly
 * the ones recorded in the shipped shaders (this is the pre-2021 donut light_cb.h: the field
 * that modern donut calls 'intensity' is still 'radiance', 'lightType' is a uint and there is
 * no 'shadowChannel').
 *
 * Usage follows the donut convention (see donut/shaders/view_cb.h):
 *   HLSL : #include "light_cb.h"
 *   C++  : #include <donut/core/math/math.h>
 *          using namespace donut::math;
 *          #include "light_cb.h"
 *
 * C++: donut main's <donut/shaders/light_cb.h> defines different structs with the same names
 * (its LightConstants has an extra shadowChannel), so the 2018 structs live in namespace light2018
 * and other headers refer to them through LIGHT2018_NS. The include guard is distinct from donut's.
 */

#ifndef ASTEROIDS_LIGHT_CB_2018_H
#define ASTEROIDS_LIGHT_CB_2018_H

#ifdef __cplusplus
#define LIGHT_CB_ROW_MAJOR
#define LIGHT2018_NS light2018::
namespace light2018
{
#else
#define LIGHT_CB_ROW_MAJOR row_major
#define LIGHT2018_NS
#endif

// size 112 (7 registers)
struct ShadowConstants
{
    LIGHT_CB_ROW_MAJOR float4x4 matWorldToUvzwShadow;   //   0

    float2      shadowFadeScale;                        //  64
    float2      shadowFadeBias;                         //  72

    float2      shadowMapCenterUV;                      //  80
    float       shadowFalloffDistance;                  //  88
    int         shadowMapArrayIndex;                    //  92

    float2      shadowMapSizeTexels;                    //  96
    float2      shadowMapSizeTexelsInv;                 // 104
};

// size 96 (6 registers)
struct LightConstants
{
    float3      direction;                              //   0
    uint        lightType;                              //  12

    float3      position;                               //  16
    float       radius;                                 //  28

    float3      color;                                  //  32
    float       radiance;                               //  44

    float       angularSizeOrInvRange;                  //  48  angular size for directional lights, 1/range for spot and point lights
    float       innerAngle;                             //  52
    float       outerAngle;                             //  56
    float       outOfBoundsShadow;                      //  60

    int4        shadowCascades;                         //  64  indices into the shadow constant array, -1 terminates
    int4        perObjectShadows;                       //  80  indices into the shadow constant array, -1 = unused
};

// size 128 (8 registers)
struct LightProbeConstants
{
    float       diffuseScale;                           //   0
    float       specularScale;                          //   4
    float       mipLevels;                              //   8
    float       padding1;                               //  12

    uint        diffuseArrayIndex;                      //  16
    uint        specularArrayIndex;                     //  20
    uint2       padding2;                               //  24

    float4      frustumPlanes[6];                       //  32
};

#ifdef __cplusplus
} // namespace light2018
static_assert(sizeof(light2018::ShadowConstants) == 112, "ShadowConstants layout");
static_assert(sizeof(light2018::LightConstants) == 96, "LightConstants layout");
static_assert(sizeof(light2018::LightProbeConstants) == 128, "LightProbeConstants layout");
#endif

#endif // ASTEROIDS_LIGHT_CB_2018_H
