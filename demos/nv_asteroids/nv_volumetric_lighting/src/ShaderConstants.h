// ShaderConstants.h
//
// C++ mirrors of the HLSL constant buffers declared in shaders/ShaderCommon.hlsli.
// Offsets follow the shader reflection data and the stores made by the ContextImp::SetupCB_* functions.

#pragma once

#include "VectorMath.h"

#include <stdint.h>

namespace Nv
{
namespace VolumetricLighting
{

// cbContext (b0), 48 bytes. Filled once per context by ContextImp::SetupCB_PerContext.
struct PerContextCB
{
    Vec2 vOutputSize;               // +0   framebuffer size
    Vec2 vOutputSize_Inv;           // +8
    Vec2 vBufferSize;               // +16  internal buffer size
    Vec2 vBufferSize_Inv;           // +24
    float fResMultiplier;           // +32  1 / internal scale
    uint32_t uSampleCount;          // +36  internal MSAA sample count
    uint32_t pad[2];
};
static_assert(sizeof(PerContextCB) == 48, "cbContext layout");

// cbFrame (b1), 416 bytes. Filled per BeginAccumulation by ContextImp::SetupCB_PerFrame.
struct PerFrameCB
{
    Mat44 mProj;                    // +0
    Mat44 mViewProj;                // +64
    Mat44 mViewProj_Inv;            // +128
    Vec2 vOutputViewportSize;       // +192
    Vec2 vOutputViewportSize_Inv;   // +200
    Vec2 vViewportSize;             // +208 internal viewport size
    Vec2 vViewportSize_Inv;         // +216
    Vec3 vEyePosition;              // +224
    float pad0;
    Vec2 vJitterOffset;             // +240
    float fZNear;                   // +248
    float fZFar;                    // +252
    Vec3 vScatterPower;             // +256 1 - exp(-total scattering)
    uint32_t uNumPhaseTerms;        // +268
    Vec3 vSigmaExtinction;          // +272 total scattering + absorption
    float pad1;
    struct
    {
        uint32_t value;
        uint32_t pad[3];
    } uPhaseFunc[4];                // +288
    Vec4 vPhaseParams[4];           // +352 xyz = density, w = eccentricity
};
static_assert(sizeof(PerFrameCB) == 416, "cbFrame layout");

// cbVolume (b2), 816 bytes. Filled per RenderVolume by ContextImp::SetupCB_PerVolume.
struct PerVolumeCB
{
    Mat44 mLightToWorld;            // +0
    float fLightFalloffAngle;       // +64  Spotlight.fFalloff_CosTheta
    float fLightFalloffPower;       // +68  Spotlight.fFalloff_Power
    float fGridSectionSize;         // +72
    float fLightToEyeDepth;         // +76  never written by the D3D11 backend
    float fLightZNear;              // +80
    float fLightZFar;               // +84
    float pad0[2];
    Vec4 vAttenuationFactors;       // +96
    Mat44 mLightProj[4];            // +112
    Mat44 mLightProj_Inv[4];        // +368
    Vec3 vLightDir;                 // +624
    float fDepthBias;               // +636 g_fGodrayBias
    Vec3 vLightPos;                 // +640
    uint32_t uMeshResolution;       // +652
    Vec3 vLightIntensity;           // +656
    float fTargetRaySize;           // +668
    Vec4 vElementOffsetAndScale[4]; // +672
    Vec4 vShadowMapDim;             // +736
    struct
    {
        uint32_t value;
        uint32_t pad[3];
    } uElementIndex[4];             // +752
};
static_assert(sizeof(PerVolumeCB) == 816, "cbVolume layout");

// cbApply (b3), 96 bytes. Filled per ApplyLighting by ContextImp::SetupCB_PerApply.
struct PerApplyCB
{
    Mat44 mHistoryXform;            // +0  current clip -> previous clip
    float fFilterThreshold;         // +64
    float fHistoryFactor;           // +68
    float pad0[2];
    Vec3 vFogLight;                 // +80
    float fMultiScattering;         // +92
};
static_assert(sizeof(PerApplyCB) == 96, "cbApply layout");

} // namespace VolumetricLighting
} // namespace Nv
