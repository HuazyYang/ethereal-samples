/*
 * Constant buffer / structured buffer layouts shared by the meshlet geometry shaders
 * (asteroidTS/MS, basicTS/MS, debugTS/MS) and the C++ renderer.
 *
 * Reconstructed from the DXIL reflection of the 2018 Asteroids demo. Struct names, field
 * names, field order and offsets are exactly the ones recorded in the shipped shaders.
 *
 * Usage follows the donut convention (see donut/shaders/view_cb.h):
 *   HLSL : #include "meshlet_cb.h"
 *   C++  : #include <donut/core/math/math.h>
 *          using namespace donut::math;
 *          #include "meshlet_cb.h"
 *
 * Matrices are row_major on the HLSL side (as recorded in the reflection); donut::math
 * matrices are row-major in memory, so both sides agree without transposition.
 */

#ifndef MESHLET_CB_H
#define MESHLET_CB_H

#ifdef __cplusplus
#include <cstddef> // offsetof in the layout checks below
#define MESHLET_ROW_MAJOR
#else
#define MESHLET_ROW_MAJOR row_major
#endif

// Meshlet limits the assets are built for (MeshSet maxVerts / maxPrims) and the
// task / mesh workgroup size used by the 2018 PSO (NvPsoTaskShaderDesc::numThreads).
#define MESHLET_MAX_VERTICES    64
#define MESHLET_MAX_PRIMITIVES  100
#define MESHLET_GROUP_SIZE      32

// cbuffer cbFrame : register(b0, space1)                          size 560
struct FrameCB
{
    MESHLET_ROW_MAJOR float4x4 matWorldToClip;          //   0
    MESHLET_ROW_MAJOR float4x4 matWorldToView;          //  64
    MESHLET_ROW_MAJOR float4x4 matViewToClip;           // 128
    MESHLET_ROW_MAJOR float4x4 matProjectionForLod;     // 192
    float4      cameraPos;                              // 256
    MESHLET_ROW_MAJOR float4x4 blueNoise;               // 272
    float4      preViewTranslation;                     // 336
    float4      preViewTranslationPrevious;             // 352
    float4      frustumPlanes[6];                       // 368  xyz = normal, w = distance; inside: dot(n, p) - w <= 0
    float2      rtDims;                                 // 464
    float       lodBias;                                // 472
    float       lodSlope;                               // 476
    float2      invZFarDims;                            // 480  1 / (Hi-Z level 0 size)
    float       invZFarTileSize;                        // 488  1 / (pixels per Hi-Z level 0 texel), i.e. 1/8
    float       zFarNumLevels;                          // 492
    uint        enableLod;                              // 496
    uint        alphaPreset;                            // 500
    uint        forcedLod;                              // 504
    uint        enableDistanceLod;                      // 508
    uint        visualizeLods;                          // 512
    float       transitionRange;                        // 516
    float2      padding;                                // 520
    uint        enableAsteroidsCulling;                 // 528
    uint        showBBoxes;                             // 532
    uint        enableViewDistanceFade;                 // 536
    float       time;                                   // 540
    uint        orthographicProjection;                 // 544
    uint        enableZCull;                            // 548
    uint        enableZCullStats;                       // 552
    uint        padding2;                               // 556
};

// cbuffer cbMeshletInfo : register(b1, space1)                    size 8
struct MeshletInfoCB
{
    uint        numMeshlets;                            //   0
    uint        firstMeshlet;                           //   4
};

// cbuffer cbInstance : register(b2, space1), cbInstancePrev : register(b3, space1)   size 48
struct InstanceCB
{
    MESHLET_ROW_MAJOR float3x4 instanceMat;             //   0  object -> world, worldPos = mul(instanceMat, float4(p, 1))
};

struct BoundingBox
{
    float4      bboxMin;                                //   0
    float4      bboxMax;                                //  16
};

// cbuffer cbObjectInfo : register(b4, space1)                     size 404
struct ObjectConstants
{
    BoundingBox bbox;                                   //   0  whole object (all LODs)
    BoundingBox bboxLods[10];                           //  32  per LOD; meshlet culling data is quantized against these
    float3      center;                                 // 352  bounding sphere (object space)
    float       radius;                                 // 364
    float       lodBias;                                // 368
    float       maxLevelToRender;                       // 372
#ifdef __cplusplus
    uint        _alignPadding[2];                       // 376  HLSL starts the array below on a new register
    uint4       padding[2];                             // 384  HLSL 'uint padding[2]': one register per element
#else
    uint        padding[2];                             // 384
#endif
};

// cbuffer cbSectorInfo : register(b5, space1)                     size 16
struct SectorInfo
{
    float3      sectorOffset;                           //   0  added to the instance's world position
    uint        asteroidId;                             //  12
};

// StructuredBuffer<LODInfo> lodInfoBuffer : register(t10, space1)  stride 32
struct LODInfo
{
    uint        indexStart;                             //   0  base added to meshlet.vertexOffset (u32 vertex-index stream)
    uint        numIndices;                             //   4
    uint        vertexStart;                            //   8  base added to every fetched vertex index
    uint        numVertices;                            //  12
    uint        primStart;                              //  16  base added to meshlet.primOffset (bytes into primBuffer)
    uint        numPrims;                               //  20
    uint        minfoStart;                             //  24  first meshlet of this LOD in meshletBuffer
    uint        numMinfo;                               //  28  meshlet count of this LOD
};

// StructuredBuffer<AsteroidInstance> instanceInfoBuffer : register(t11, space1)  stride 64
struct AsteroidInstance
{
    MESHLET_ROW_MAJOR float3x4 instanceMat;             //   0
    float       uniformScale;                           //  48  scales ObjectConstants::radius and meshlet spheres
    float       padding[3];                             //  52
};

#ifdef __cplusplus
static_assert(sizeof(FrameCB) == 560, "FrameCB layout");
static_assert(offsetof(FrameCB, frustumPlanes) == 368, "FrameCB layout");
static_assert(offsetof(FrameCB, rtDims) == 464, "FrameCB layout");
static_assert(offsetof(FrameCB, enableLod) == 496, "FrameCB layout");
static_assert(offsetof(FrameCB, enableAsteroidsCulling) == 528, "FrameCB layout");
static_assert(offsetof(FrameCB, padding2) == 556, "FrameCB layout");
static_assert(sizeof(MeshletInfoCB) == 8, "MeshletInfoCB layout");
static_assert(sizeof(InstanceCB) == 48, "InstanceCB layout");
static_assert(sizeof(BoundingBox) == 32, "BoundingBox layout");
static_assert(offsetof(ObjectConstants, center) == 352, "ObjectConstants layout");
static_assert(offsetof(ObjectConstants, maxLevelToRender) == 372, "ObjectConstants layout");
static_assert(offsetof(ObjectConstants, padding) == 384, "ObjectConstants layout");
static_assert(sizeof(ObjectConstants) >= 404, "ObjectConstants layout");
static_assert(sizeof(SectorInfo) == 16, "SectorInfo layout");
static_assert(sizeof(LODInfo) == 32, "LODInfo layout");
static_assert(sizeof(AsteroidInstance) == 64, "AsteroidInstance layout");
#endif

#endif // MESHLET_CB_H
