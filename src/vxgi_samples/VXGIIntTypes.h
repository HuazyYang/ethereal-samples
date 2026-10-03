#ifndef VXGIINTTYPES_H
#define VXGIINTTYPES_H
#include "VXGITypes.h"
#include "shaders/Common/VXGIPreset.hlsli"
#include <vector>

#ifdef ANDROID
#define VXGI_ALIGN(B) __attribute__((aligned(B)))
#else
#ifdef _WIN32
#define VXGI_ALIGN(B) __declspec(align(B))
#endif
#endif

#define VXGI_ENUM_CLASS_FLAG_OPERATORS(T)                                 \
    inline T operator|(T a, T b) { return T(uint32_t(a) | uint32_t(b)); } \
    inline T operator&(T a, T b) {                                        \
        return T(uint32_t(a) & uint32_t(b));                              \
    } /* NOLINT(bugprone-macro-parentheses) */                            \
    inline T operator~(T a) {                                             \
        return T(~uint32_t(a));                                           \
    } /* NOLINT(bugprone-macro-parentheses) */                            \
    inline bool operator!(T a) { return uint32_t(a) == 0; }               \
    inline bool operator==(T a, uint32_t b) { return uint32_t(a) == b; }  \
    inline bool operator!=(T a, uint32_t b) { return uint32_t(a) != b; }

namespace vxgi {

#define VXGI_SUCCEEDED(expr) ((expr) == Status::OK)
#define VXGI_FAILED(expr) ((expr) != Status::OK)

VXGI_ENUM_CLASS_FLAG_OPERATORS(EmittanceFormat)

struct DerivedVoxelizationParameters : VoxelizationParameters {
    uint totalLevels;
    uint pageLevels;
    int indirectIrradianceMapLod;
    uint indirectIrradianceMapSize;
    int allocationMapLod;
    uint allocationMapSize;
    bool useOpacityInterpolation;
};

struct VxgiBox4i {
    int4 lower;
    int4 upper;
};

struct VxgiBox4f {
    float4 lower;
    float4 upper;
};

struct Frustum {
    float4 planes[6];
};

struct MultiListParams {
    uint segmentOffset;
    uint maxElements;
    uint logPageSize;
    uint dummy;
};

struct PerGPUData {
    std::vector<box3> m_RegionsToInvalidate;
    std::vector<frustum> m_LightFrustaToInvalidate;
    std::vector<box3> m_PreviousLevelRegions;
    int3 m_ToroidalOffsetPreviousFrame = {0};
    float3 m_ClipmapAnchorPreviousFrame = {0.f};
    float4 m_VoxelizationGridCenterPreviousFrame = {0.f};
    float m_ClipRangePreviousFrame = 0.f;
    bool m_AllocationCleared = false;
    bool m_OpacityCleared = false;
    bool m_EmittanceCleared = false;
    bool m_IndirectIrradianceCleared = false;
    bool m_AlternatingFrameIndex = false;
};

inline uint log2_ceil(uint x) {
    uint y = 0;
    for (; (1 << y) < x; ++y)
        ;
    return y;
}

inline uint log8_ceil(uint x) { return (log2_ceil(x) + 2) / 3; }

inline bool IsPowerOf2(uint x) { return (x & (x - 1u)) == 0; }

inline
void ExtentFrustumForConservativeVoxelization(frustum &f, float voxelSize) {
    // Move every plane outwards in the direction of its normal to get conservative
    // voxelization with regular sampling
    for (int i = 0; i < frustum::PLANES_COUNT; ++i) {
        plane& p = f.planes[i];
        p.distance +=
            (abs(p.normal.x) + abs(p.normal.y) + abs(p.normal.z)) * voxelSize * 0.5f;
    }
}

struct AbstractTracingConstants {
    float4 m_vrOpacityTextureSize;
    float4 m_vrEmittanceTextureSize;
    float4 m_vClipmapAnchor;
    float4 m_vSceneBoundaryLower;
    float4 m_vSceneBoundaryUpper;
    float4 m_vClipmapCenter;
    float4 m_vToroidalOffset;
    float m_fEmittancePackingStride;
    float m_fFinestVoxelSize;
    float m_fStackTextureSize;
    float m_frNearestLevel0Boundary;
    float m_fMaxMipmapLevel;
    float m_frEmittanceStorageScale;
    float m_frClipmapSizeWorld;
    uint m_bUse6DOpacity;
};

struct CacheLevelConstants {
    float4 m_TranslationParameters[MAX_TOTAL_LEVELS];
    float4 m_TranslationParameters2[MAX_TOTAL_LEVELS];
};

inline float GetConeFactor(float coneAngleDegrees) {
    if(coneAngleDegrees >= 140.f)
        coneAngleDegrees = 140.f;

    float basicFactor = 2.f * std::tanf(dm::radians(coneAngleDegrees) / 2.f);
    return basicFactor / (basicFactor * 0.25f + 1.f);
}

inline int bitrev(int x, int n) {
    uint y = 0;
    for (int i = 0; i < n; ++i) {
        if((1 << i) & x)
            y |= (1 << (n - i - 1));
    }

    return y;
}

inline int zcurve(int x, int y, int n) {
    uint z = 0;
    for (int i = 0; i < n; ++i)
        z |= (((y >> i) & 1) << (2 * i + 1)) | (((x >> i) & 1) << (2 * i));
    return z;
}

inline void InvalidateFramebuffer(nvrhi::IDevice *device, const nvrhi::FramebufferDesc &fbDec, nvrhi::IFramebuffer **framebuffer) {
    bool invalidated = true;
    if(*framebuffer) {
        auto curFbDesc = (*framebuffer)->getDesc();
        invalidated = memcmp(&curFbDesc, &fbDec, sizeof(fbDec)) != 0;
        if(invalidated)
            (*framebuffer) = nullptr;
    }

    if(invalidated) {
        nvrhi::FramebufferHandle newFb;
        device->createFramebuffer(fbDec, &newFb);
        (*framebuffer) = newFb;
        newFb->AddRef();
    }
}

enum class UserShaderBindingID {
  // clang-format off
    UNKNOWN                        = 999,
    VOXELIZE_MATERIAL_CB           = 1000,
    VOXELIZE_CB                    = 1001,
    SCISSOR_REGIONS_SRV            = 1002,
    IRRADIANCE_MAP_SRV             = 1003,
    INVALIDATE_BITMAP_SRV          = 1004,
    COVERAGE_MASKS_SRV             = 1005,
    ALLOCATION_MAP_UAV             = 1006,
    EMITTANCE_EVEN_R_UAV           = 1007,
    EMITTANCE_EVEN_G_UAV           = 1008,
    EMITTANCE_EVEN_B_UAV           = 1009,
    EMITTANCE_ODD_R_UAV            = 1010,
    EMITTANCE_ODD_G_UAV            = 1011,
    EMITTANCE_ODD_B_UAV            = 1012,
    COVERAGE_POS_UAV               = 1013,
    COVERAGE_NEG_UAV               = 1014,
    SCISSOR_STATS_UAV              = 1015,
    IRRADIANCE_MAP_SAMPLER         = 1016,
  // clang-format on
};

}  // namespace vxgi


#endif /* VXGIINTTYPES_H */
