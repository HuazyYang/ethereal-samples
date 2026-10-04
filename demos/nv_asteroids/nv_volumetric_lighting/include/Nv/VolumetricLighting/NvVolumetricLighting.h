// NvVolumetricLighting.h
//
// Public API of NVIDIA GameWorks Volumetric Lighting as exported by NvVolumetricLighting.d3d11.dll.
//
// Reconstruction notes:
//   - The exported functions and their signatures are fixed by the DLL's export table (mangled names,
//     see NvVolumetricLighting.d3d11.original.def). This build of the library uses an "Args struct"
//     calling convention (BeginAccumulationArgs, RenderVolumeArgs, EndAccumulationArgs,
//     ApplyLightingArgs) instead of the long parameter lists of the public GitHub release.
//   - Enumerations, descriptor structures and their field names follow the naming of the public
//     NvVolumetricLighting API; every field offset was verified against the accesses made by the DLL
//     (offsets are given in the comments). Fields of the Args structures that the D3D11 backend never
//     reads are marked "unresolved".

#ifndef NV_VOLUMETRICLIGHTING_H
#define NV_VOLUMETRICLIGHTING_H

#include "NvFoundationTypes.h"

#if defined(NV_VOLUMETRICLIGHTING_BUILD_DLL)
#   define NV_VOLUMETRICLIGHTING_API(RET) __declspec(dllexport) RET __cdecl
#   define NV_FOUNDATION_API __declspec(dllexport)
#else
#   define NV_VOLUMETRICLIGHTING_API(RET) __declspec(dllimport) RET __cdecl
#   define NV_FOUNDATION_API __declspec(dllimport)
#endif

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;
struct ID3D11RenderTargetView;

namespace nvidia
{
// Returns the assertion handler installed by OpenLibrary.
// NvVolumetricLighting.d3d11.dll: 0x180005600
NV_FOUNDATION_API NvAssertHandler& __cdecl NvGetAssertHandler();
} // namespace nvidia

namespace Nv
{
namespace VolumetricLighting
{

////////////////////////////////////////////////////////////////////////////////
// Constants

const uint32_t MAX_JITTER_STEPS = 8;        // Halton jitter sequence length (temporal filtering)
const uint32_t MAX_PHASE_TERMS = 4;         // MediumDesc::PhaseTerms
const uint32_t MAX_SHADOWMAP_ELEMENTS = 4;  // ShadowMapDesc::Elements (cascades)

////////////////////////////////////////////////////////////////////////////////
// Enumerations

enum class Status
{
    OK = 0,                     // Success
    FAIL = -1,                  // Unspecified failure
    INVALID_VERSION = -2,       // Mismatch between header and DLL (OpenLibrary)
    UNINITIALIZED = -3,         // API called before OpenLibrary
    UNIMPLEMENTED = -4,         // Not implemented for the platform
    INVALID_PARAMETER = -5,     // One or more invalid parameters
    UNSUPPORTED_DEVICE = -6,    // Device does not support feature level 11_0
    RESOURCE_FAILURE = -7,      // Failed to allocate a resource (texture / constant buffer)
    API_ERROR = -8,             // The graphics API returned an error (shader / state creation)
};

enum class PlatformName
{
    UNKNOWN = -1,
    D3D11 = 0,                  // CreateContext only accepts PlatformDesc::platform == 0
    COUNT
};

enum class DebugFlags : uint32_t
{
    NONE = 0,
    WIREFRAME = 1 << 0,         // Draw the light volume geometry in wireframe (debug PS)
    NO_BLENDING = 1 << 1,       // Composite without modulating the scene (debug blend state)
};

enum class DownsampleMode
{
    UNKNOWN = -1,
    FULL = 0,                   // Internal buffer at full resolution
    HALF = 1,                   // 1/2 resolution
    QUARTER = 2,                // 1/4 resolution
    COUNT
};

enum class MultisampleMode
{
    UNKNOWN = -1,
    SINGLE = 0,
    MSAA2 = 1,
    MSAA4 = 2,
    COUNT
};

enum class FilterMode
{
    UNKNOWN = -1,
    NONE = 0,
    TEMPORAL = 1,               // Jittered accumulation + temporal reprojection filter
    COUNT
};

enum class PhaseFunctionType
{
    UNKNOWN = -1,
    ISOTROPIC = 0,
    RAYLEIGH = 1,
    HENYEYGREENSTEIN = 2,
    MIE_HAZY = 3,
    MIE_MURKY = 4,
    COUNT
};

enum class LightType
{
    UNKNOWN = -1,
    DIRECTIONAL = 0,
    SPOTLIGHT = 1,
    POINT = 2,
    COUNT
};

enum class SpotlightFalloffMode
{
    UNKNOWN = -1,
    NONE = 0,
    FIXED = 1,
    CUSTOM = 2,
    COUNT
};

enum class AttenuationMode
{
    UNKNOWN = -1,
    NONE = 0,
    POLYNOMIAL = 1,
    INV_POLYNOMIAL = 2,
    COUNT
};

enum class ShadowMapLayout
{
    UNKNOWN = -1,
    SIMPLE = 0,
    CASCADE_ATLAS = 1,
    CASCADE_ARRAY = 2,
    PARABOLOID = 3,             // dual paraboloid (2-slice array) used by point lights
    COUNT
};

enum class TessellationQuality
{
    UNKNOWN = -1,
    LOW = 0,                    // max tess factor 16
    MEDIUM = 1,                 // max tess factor 32
    HIGH = 2,                   // max tess factor 64
    COUNT
};

enum class UpsampleQuality
{
    UNKNOWN = -1,
    POINT = 0,
    BILINEAR = 1,
    BILATERAL = 2,
    COUNT
};

////////////////////////////////////////////////////////////////////////////////
// Platform types (D3D11 backend)

typedef void* Context;
typedef ID3D11DeviceContext* PlatformRenderCtx;
typedef ID3D11ShaderResourceView* PlatformShaderResource;
typedef ID3D11RenderTargetView* PlatformRenderTarget;

////////////////////////////////////////////////////////////////////////////////
// Descriptors

struct VersionDesc
{
    uint32_t Major;             // +0  must be 1
    uint32_t Minor;             // +4  must be 0

    VersionDesc() : Major(1), Minor(0) {}
};

struct PlatformDesc
{
    PlatformName platform;      // +0
    union
    {
        struct
        {
            ID3D11Device* pDevice;  // +8
        } d3d11;
    };
};

struct ContextDesc                          // sizeof == 24 (copied into the context)
{
    struct
    {
        uint32_t uWidth;                    // +0  output (scene) buffer width
        uint32_t uHeight;                   // +4
        uint32_t uSamples;                  // +8  scene MSAA sample count
    } framebuffer;
    DownsampleMode eDownsampleMode;         // +12
    MultisampleMode eInternalSampleMode;    // +16
    FilterMode eFilterMode;                 // +20
};

struct ViewerDesc                           // sizeof == 148 (copied into the context)
{
    NvcMat44 mProj;                         // +0
    NvcMat44 mViewProj;                     // +64
    NvcVec3 vEyePosition;                   // +128
    uint32_t uViewportWidth;                // +140
    uint32_t uViewportHeight;               // +144
};

struct MediumDesc                           // sizeof == 96
{
    NvcVec3 vAbsorption;                    // +0
    uint32_t uNumPhaseTerms;                // +12
    struct
    {
        PhaseFunctionType ePhaseFunc;       // +0
        NvcVec3 vDensity;                   // +4  scattering coefficient of this term
        float fEccentricity;                // +16 Henyey-Greenstein g
    } PhaseTerms[MAX_PHASE_TERMS];          // +16, stride 20
};

struct ShadowMapDesc                        // sizeof == 352
{
    ShadowMapLayout eType;                  // +0
    uint32_t uWidth;                        // +4
    uint32_t uHeight;                       // +8
    uint32_t uElementCount;                 // +12
    struct
    {
        NvcMat44 mViewProj;                 // +0
        uint32_t uOffsetX;                  // +64
        uint32_t uOffsetY;                  // +68
        uint32_t uWidth;                    // +72
        uint32_t uHeight;                   // +76
        uint32_t mArrayIndex;               // +80
    } Elements[MAX_SHADOWMAP_ELEMENTS];     // +16, stride 84
};

struct LightDesc                            // sizeof == 144
{
    LightType eType;                        // +0
    NvcMat44 mLightToWorld;                 // +4
    NvcVec3 vIntensity;                     // +68
    union                                   // +80
    {
        struct
        {
            NvcVec3 vDirection;             // +80
        } Directional;
        struct
        {
            NvcVec3 vDirection;             // +80
            NvcVec3 vPosition;              // +92
            float fZNear;                   // +104
            float fZFar;                    // +108
            SpotlightFalloffMode eFalloffMode;  // +112
            float fFalloff_CosTheta;        // +116
            float fFalloff_Power;           // +120
            AttenuationMode eAttenuationMode;   // +124
            float fAttenuationFactors[4];   // +128
        } Spotlight;
        struct
        {
            NvcVec3 vPosition;              // +80
            float fZNear;                   // +92
            float fZFar;                    // +96
            AttenuationMode eAttenuationMode;   // +100
            float fAttenuationFactors[4];   // +104
        } Omni;
    };
};

struct VolumeDesc                           // sizeof == 16
{
    float fTargetRayResolution;             // +0
    uint32_t uMaxMeshResolution;            // +4
    float fDepthBias;                       // +8
    TessellationQuality eTessQuality;       // +12
};

struct PostprocessDesc                      // sizeof == 100
{
    NvcMat44 mUnjitteredViewProj;           // +0
    float fTemporalFactor;                  // +64
    float fFilterThreshold;                 // +68
    UpsampleQuality eUpsampleQuality;       // +72
    NvcVec3 vFogLight;                      // +76
    float fMultiscatter;                    // +88
    bool bDoFog;                            // +92
    bool bIgnoreSkyFog;                     // +93
    float fBlendfactor;                     // +96
};

////////////////////////////////////////////////////////////////////////////////
// Per-call argument blocks

struct BeginAccumulationArgs
{
    PlatformRenderCtx renderCtx;            // +0
    const ViewerDesc* pViewerDesc;          // +8
    const MediumDesc* pMediumDesc;          // +16
    DebugFlags debugFlags;                  // +24
    void* unresolved32;                     // +32 unresolved: never read by the D3D11 backend
    PlatformShaderResource sceneDepth;      // +40 scene depth buffer (single or multisampled)
};

struct RenderVolumeArgs
{
    PlatformRenderCtx renderCtx;            // +0
    const ShadowMapDesc* pShadowMapDesc;    // +8
    const LightDesc* pLightDesc;            // +16
    const VolumeDesc* pVolumeDesc;          // +24
    PlatformShaderResource shadowMap;       // +32 shadow map (Texture2D or Texture2DArray)
};

struct EndAccumulationArgs
{
    PlatformRenderCtx renderCtx;            // +0
};

struct ApplyLightingArgs
{
    PlatformRenderCtx renderCtx;            // +0
    const PostprocessDesc* pPostprocessDesc;    // +8
    PlatformRenderTarget sceneTarget;       // +16 render target the lighting is composited into
    void* unresolved24;                     // +24 unresolved: never read by the D3D11 backend
    void* unresolved32;                     // +32 unresolved: never read by the D3D11 backend
    PlatformShaderResource sceneDepth;      // +40 scene depth buffer
};

////////////////////////////////////////////////////////////////////////////////
// Exported functions

// NvVolumetricLighting.d3d11.dll: 0x180005610
NV_VOLUMETRICLIGHTING_API(Status) OpenLibrary(nvidia::NvAllocatorCallback* allocator,
                                              nvidia::NvAssertHandler* assertion_handler,
                                              const VersionDesc& version = VersionDesc());

// NvVolumetricLighting.d3d11.dll: 0x1800056B0
NV_VOLUMETRICLIGHTING_API(Status) CloseLibrary();

// NvVolumetricLighting.d3d11.dll: 0x1800056D0
NV_VOLUMETRICLIGHTING_API(Status) CreateContext(Context& out_ctx, const PlatformDesc* pPlatformDesc,
                                                const ContextDesc* pContextDesc);

// NvVolumetricLighting.d3d11.dll: 0x180005780
NV_VOLUMETRICLIGHTING_API(Status) ReleaseContext(Context& ctx);

// NvVolumetricLighting.d3d11.dll: 0x180005800
NV_VOLUMETRICLIGHTING_API(Status) BeginAccumulation(Context ctx, BeginAccumulationArgs* pArgs);

// NvVolumetricLighting.d3d11.dll: 0x180005830
NV_VOLUMETRICLIGHTING_API(Status) RenderVolume(Context ctx, RenderVolumeArgs* pArgs);

// NvVolumetricLighting.d3d11.dll: 0x180005860
NV_VOLUMETRICLIGHTING_API(Status) EndAccumulation(Context ctx, EndAccumulationArgs* pArgs);

// NvVolumetricLighting.d3d11.dll: 0x180005890
NV_VOLUMETRICLIGHTING_API(Status) ApplyLighting(Context ctx, ApplyLightingArgs* pArgs);

} // namespace VolumetricLighting
} // namespace Nv

#endif // NV_VOLUMETRICLIGHTING_H
