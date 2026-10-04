/*
 * Layouts shared by the space-dust particle shaders (particles.hlsl: vs_main / ps_main /
 * update_cs_main, particles_ms.hlsl: ms_main) and the C++ particle system.
 *
 * Reconstructed from the DXIL reflection of the 2018 Asteroids demo; names, order, types and
 * offsets are exactly the recorded ones.
 *
 *   ConstantBuffer<ParticleConstants>      g_Particles : register(b0)   size 656
 *   StructuredBuffer<ParticleInfo>         t_Particles : register(t1)   stride 32  (vs / ms)
 *   RWStructuredBuffer<ParticleInfo>       u_Particles : register(u0)   stride 32  (update cs)
 *   Texture2DArray                         t_ShadowMapArray : register(t0)
 *   SamplerComparisonState                 s_ShadowSampler  : register(s0)
 *
 * Usage follows the donut convention (see donut/shaders/view_cb.h):
 *   HLSL : #include "particles_cb.h"
 *   C++  : #include <donut/core/math/math.h>
 *          using namespace donut::math;
 *          #include "particles_cb.h"
 */

#ifndef PARTICLES_CB_H
#define PARTICLES_CB_H

#include "light_cb.h"

#ifdef __cplusplus
#define PARTICLES_ROW_MAJOR
#else
#define PARTICLES_ROW_MAJOR row_major
#endif

// Threads per group of update_cs_main.
#define PARTICLES_UPDATE_GROUP_SIZE 256

// Number of shadow constant slots in ParticleConstants::shadows (cascades only).
#define PARTICLES_MAX_SHADOWS 4

// stride 32
struct ParticleInfo
{
    float3      position;                               //   0  position inside the wrapping volume
    float       brightness;                             //  12
    float3      velocity;                               //  16
    float       padding;                                //  28
};

// size 656
struct ParticleConstants
{
    PARTICLES_ROW_MAJOR float4x4 matWorldToClip;        //   0
    float3      positionOffset;                         //  64  added to ParticleInfo::position -> camera-relative world position
    float       maxDistance;                            //  76  particles further than this are culled / faded out
    float2      screenScale;                            //  80  clip-space size of a particle at size factor 1 (aspect corrected)
    float       time;                                   //  88
    float       deltaTime;                              //  92
    float3      volumeSize;                             //  96  wrap size of the particle volume (x and z are wrapped)
    uint        particlesPerBatch;                      // 108  particles per instance / mesh-shader group
    LIGHT2018_NS LightConstants light;                              // 112
    LIGHT2018_NS ShadowConstants shadows[PARTICLES_MAX_SHADOWS];     // 208
};

#ifdef __cplusplus
static_assert(sizeof(ParticleInfo) == 32, "ParticleInfo layout");
static_assert(offsetof(ParticleConstants, light) == 112, "ParticleConstants layout");
static_assert(offsetof(ParticleConstants, shadows) == 208, "ParticleConstants layout");
static_assert(sizeof(ParticleConstants) == 656, "ParticleConstants layout");
#endif

#endif // PARTICLES_CB_H
