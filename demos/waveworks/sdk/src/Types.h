#ifndef SRC_WWS_TYPES_H
#define SRC_WWS_TYPES_H
#include <cstddef>
#include <waveworks/GFSDK_WaveWorks_FloatTypes.h>
#include <waveworks/GFSDK_WaveWorks_Common.h>
#include <cstring>
#include "BasicAlgebra_ispc.h"

// NOLINT

using uint = uint32_t;

using float2 = gfsdk_float2;
using float3 = gfsdk_float3;
using float4 = gfsdk_float4;
using float4x4 = gfsdk_float4x4;

using WWRESULT = gfsdk_wwresult;

constexpr WWRESULT WWE_INTERNAL_ERROR = gfsdk_wwresult_INTERNAL_ERROR;
constexpr WWRESULT WWE_FAIL = gfsdk_wwresult_FAIL;
constexpr WWRESULT WWE_OK = gfsdk_wwresult_OK;
constexpr WWRESULT WWE_NONE = gfsdk_wwresult_NONE;
constexpr WWRESULT WWE_WOULD_BLOCK = gfsdk_wwresult_WOULD_BLOCK;

struct uint2 {
    uint x; // offset 0
    uint y; // offset 4
};
struct uint3 {
    uint x; // offset 0
    uint y; // offset 4
    uint z; // offset 8
};
struct uint4 {
    uint x; // offset 0
    uint y; // offset 4
    uint z; // offset 8
    uint w; // offset 12
};

#define VECTOR_TYPE_MAKE_TEMPLATE(stype)                                        \
    inline stype##2 make_##stype##2(stype x) {                                  \
        return {x, x};                                                          \
    }                                                                           \
    inline stype##2 make_##stype##2(stype x, stype y) {                         \
        return {x, y};                                                          \
    }                                                                           \
    inline stype##3 make_##stype##3(stype x) {                                  \
        return {x, x, x};                                                       \
    }                                                                           \
    inline stype##3 make_##stype##3(stype x, stype y, stype z) {                \
        return {x, y, z};                                                       \
    }                                                                           \
    inline stype##3 make_##stype##3(const stype##2 & v, stype z) {              \
        return {v.x, v.y, z};                                                   \
    }                                                                           \
    inline stype##4 make_##stype##4(stype x) {                                  \
        return {x, x, x, x};                                                    \
    }                                                                           \
    inline stype##4 make_##stype##4(stype x, stype y, stype z, stype w) {       \
        return {x, y, z, w};                                                    \
    }                                                                           \
    inline stype##4 make_##stype##4(const stype##2 & v, stype z, stype w) {     \
        return {v.x, v.y, z, w};                                                \
    }                                                                           \
    inline stype##4 make_##stype##4(const stype##2 & v1, const stype##2 & v2) { \
        return {v1.x, v1.y, v2.x, v2.y};                                        \
    }                                                                           \
    inline stype##4 make_##stype##4(const stype##3 & v, stype w) {              \
        return {v.x, v.y, v.z, w};                                              \
    }

VECTOR_TYPE_MAKE_TEMPLATE(float)
VECTOR_TYPE_MAKE_TEMPLATE(uint)

inline float4x4 make_float4x4(float r00, float r01, float r02, float r03, float r10,
                              float r11, float r12, float r13, float r20, float r21,
                              float r22, float r23, float r30, float r31, float r32,
                              float r33) {
    float4x4 m = {
        {{r00, r01, r02, r03, r10, r11, r12, r13, r20, r21, r22, r23, r30, r31, r32, r33}}};

    return m;
}

inline float4x4 make_float4x4(const float4 &r0, const float4 &r1, const float4 &r2,
                              const float4 &r3) {
    float4x4 m = {{{r0.x, r0.y, r0.z, r0.w, r1.x, r1.y, r1.z, r1.w, r2.x, r2.y, r2.z, r2.w,
                    r3.x, r3.y, r3.z, r3.w}}};
    return m;
}

inline float4 operator * (const float4 & a, const float4 &b) {
    return float4{a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w};
}

inline float4x4 operator * (const float4x4 &a, float b) {
    return make_float4x4(a.r0 * b, a.r1 * b, a.r2 * b, a.r3 * b);
}

// Matrix implements

inline float4x4 make_float4x4(const float *v) {
    float4x4 m;
    std::memcpy(&m, v, sizeof(m));
    return m;
}

inline float4x4 mul(const float4x4 &a, const float4x4 &b) {
    float4x4 res;
    ispc::mul_float4x4_float4x4(a.m, b.m, res.m);
    return res;
}

inline float4 mul(const float4 &a, const float4x4 &b) {
    float4 res;
    res.x = a.x * b.m[0] + a.y * b.m[4] + a.z * b.m[8] + a.w * b.m[12];
    res.y = a.x * b.m[1] + a.y * b.m[5] + a.z * b.m[9] + a.w * b.m[13];
    res.z = a.x * b.m[2] + a.y * b.m[6] + a.z * b.m[10] + a.w * b.m[14];
    res.w = a.x * b.m[3] + a.y * b.m[7] + a.z * b.m[11] + a.w * b.m[15];
    return res;
}

inline float4x4 affine_inverse(const float4x4 &m) {
    const float a00 = m.m[0];
    const float a01 = m.m[1];
    const float a02 = m.m[2];
    const float a10 = m.m[4];
    const float a11 = m.m[5];
    const float a12 = m.m[6];
    const float a20 = m.m[8];
    const float a21 = m.m[9];
    const float a22 = m.m[10];

    const float c00 = a11 * a22 - a12 * a21;
    const float c01 = a02 * a21 - a01 * a22;
    const float c02 = a01 * a12 - a02 * a11;
    const float c10 = a12 * a20 - a10 * a22;
    const float c11 = a00 * a22 - a02 * a20;
    const float c12 = a02 * a10 - a00 * a12;
    const float c20 = a10 * a21 - a11 * a20;
    const float c21 = a01 * a20 - a00 * a21;
    const float c22 = a00 * a11 - a01 * a10;

    const float det = a00 * c00 + a01 * c10 + a02 * c20;
    if (det == 0.0f)
        return make_float4x4(1.0f, 0.0f, 0.0f, 0.0f,
                             0.0f, 1.0f, 0.0f, 0.0f,
                             0.0f, 0.0f, 1.0f, 0.0f,
                             0.0f, 0.0f, 0.0f, 1.0f);

    const float invDet = 1.0f / det;
    const float tx = m.m[12];
    const float ty = m.m[13];
    const float tz = m.m[14];

    float4x4 res = make_float4x4(c00 * invDet, c01 * invDet, c02 * invDet, 0.0f,
                                 c10 * invDet, c11 * invDet, c12 * invDet, 0.0f,
                                 c20 * invDet, c21 * invDet, c22 * invDet, 0.0f,
                                 0.0f, 0.0f, 0.0f, 1.0f);
    res.m[12] = -(tx * res.m[0] + ty * res.m[4] + tz * res.m[8]);
    res.m[13] = -(tx * res.m[1] + ty * res.m[5] + tz * res.m[9]);
    res.m[14] = -(tx * res.m[2] + ty * res.m[6] + tz * res.m[10]);
    return res;
}

void diagnostic_message(const wchar_t *fmt, ...);

constexpr float Pif = 3.1415926535897932384626433832795f;
constexpr float TwoPif = 2.f * Pif;

struct Wind_Waves_Detailed_Simulation_Parameters {
    struct Cascade {
        uint numGPUs;                            // offset 0
        uint textureSize;                        // offset 4
        float physicalSize;                      // offset 8
        float displacementScale;                 // offset 12
        float2 baseWindDirection;                // offset 16
        float baseWindSpeed;                     // offset 24
        float baseWindDistance;                  // offset 28
        float baseSpectrumPeaking;               // offset 32
        float baseWindDependency;                // offset 36
        float baseSmallWavesCutoffLength;        // offset 40
        float baseSmallWavesCutoffPower;         // offset 44
        float baseAmplitudeMultiplier;           // offset 48
        float2 swellWindDirection;               // offset 52
        float swellWindSpeed;                    // offset 60
        float swellWindDistance;                 // offset 64
        float swellWindDependency;               // offset 68
        float swellSpectrumPeaking;              // offset 72
        float swellSmallWavesCutoffLength;       // offset 76
        float swellSmallWavesCutoffPower;        // offset 80
        float swellAmplitudeMultiplier;          // offset 84
        bool writeDisplacementMap;               // offset 88
        uint8_t writeDisplacementMapPadding[3];  // offset 89
        uint archiveSlots;                       // offset 92
        float lowerGridRadius;                   // offset 96
        float upperGridRadius;                   // offset 100
        float warpAmplitude;                     // offset 104
        float wrapFrequency;                     // offset 108
        float foamGenerationThreshold;           // offset 112
        float foamGenerationAmount;              // offset 116
        float foamDissipationSpeed;              // offset 120
        float foamFalloffSpeed;                  // offset 124
    };
static_assert(offsetof(Cascade, displacementScale) == 12 &&
                  offsetof(Cascade, writeDisplacementMap) == 88 &&
                  offsetof(Cascade, archiveSlots) == 92 &&
                  offsetof(Cascade, lowerGridRadius) == 96 &&
                  offsetof(Cascade, upperGridRadius) == 100 &&
                  offsetof(Cascade, warpAmplitude) == 104 &&
                  offsetof(Cascade, wrapFrequency) == 108 &&
                  offsetof(Cascade, foamGenerationThreshold) == 112 &&
                  offsetof(Cascade, foamGenerationAmount) == 116 &&
                  offsetof(Cascade, foamDissipationSpeed) == 120 &&
                  offsetof(Cascade, foamFalloffSpeed) == 124 &&
                  sizeof(Cascade) == 128,
              "Bad Alignment");

    float foamWhitecapsThreshold;                                     // offset 0
    Cascade cascades[4];                                               // offset 4
    float timeScale;                                                   // offset 516
    uint numGPUs;                                                       // offset 520
    GFSDK_WaveWorks_Simulation_API simulationAPI;                      // offset 524
    GFSDK_WaveWorks_Simulation_CPU_Threading_Model cpuThreadingModel;  // offset 528
    bool enableGpuTimers;                                              // offset 532
    bool enableCpuTimers;                                              // offset 533
    bool enableGpuDrivenDisplacementCalculation;                       // offset 534
    uint8_t padding[9];                                                // offset 535
};

static_assert(offsetof(Wind_Waves_Detailed_Simulation_Parameters, foamWhitecapsThreshold) == 0 &&
                  offsetof(Wind_Waves_Detailed_Simulation_Parameters, cascades) == 4 &&
                  offsetof(Wind_Waves_Detailed_Simulation_Parameters, timeScale) == 516 &&
                  offsetof(Wind_Waves_Detailed_Simulation_Parameters, numGPUs) == 520 &&
                  offsetof(Wind_Waves_Detailed_Simulation_Parameters, simulationAPI) == 524 &&
                  offsetof(Wind_Waves_Detailed_Simulation_Parameters, cpuThreadingModel) == 528 &&
                  offsetof(Wind_Waves_Detailed_Simulation_Parameters, enableGpuTimers) == 532 &&
                  offsetof(Wind_Waves_Detailed_Simulation_Parameters, enableCpuTimers) == 533 &&
                  offsetof(Wind_Waves_Detailed_Simulation_Parameters,
                           enableGpuDrivenDisplacementCalculation) == 534 &&
                  sizeof(Wind_Waves_Detailed_Simulation_Parameters) == 544,
              "Bad wind waves parameter alignment");

struct Local_Waves_Detailed_Simulation_Parameters {
    float amplitudeMultiplier;                     // offset 0
    float lateralMultiplier;                       // offset 4
    float foamWhitecapsThreshold;                  // offset 8
    float foamGenerationThreshold;                 // offset 12
    float foamGenerationAmount;                    // offset 16
    float foamDissipationSpeed;                    // offset 20
    float foamFalloffSpeed;                        // offset 24
    uint textureSize;                              // offset 28
    float offsetX;                                 // offset 32
    float offsetY;                                 // offset 36
    float physicalSize;                            // offset 40
    GFSDK_WaveWorks_Simulation_API simulationAPI;  // offset 44
    bool enableCpuDrivenDisplacementCalculation;   // offset 48
    bool enableGpuDrivenDisplacementCalculation;   // offset 49
    uint8_t padding0[2];                                               // offset 50
    GFSDK_WaveWorks_Simulation_CPU_Threading_Model cpuThreadingModel;  // offset 52
    bool enableGpuTimers;                                              // offset 56
    bool enableCpuTimers;                                              // offset 57
    uint8_t padding1[2];                                               // offset 58
};

static_assert(offsetof(Local_Waves_Detailed_Simulation_Parameters, textureSize) == 28 &&
                  offsetof(Local_Waves_Detailed_Simulation_Parameters, offsetX) == 32 &&
                  offsetof(Local_Waves_Detailed_Simulation_Parameters, physicalSize) == 40 &&
                  offsetof(Local_Waves_Detailed_Simulation_Parameters, simulationAPI) == 44 &&
                  offsetof(Local_Waves_Detailed_Simulation_Parameters,
                           enableCpuDrivenDisplacementCalculation) == 48 &&
                  offsetof(Local_Waves_Detailed_Simulation_Parameters, cpuThreadingModel) == 52 &&
                  offsetof(Local_Waves_Detailed_Simulation_Parameters, enableGpuTimers) == 56 &&
                  offsetof(Local_Waves_Detailed_Simulation_Parameters, enableCpuTimers) == 57 &&
                  sizeof(Local_Waves_Detailed_Simulation_Parameters) == 60,
              "Bad local waves parameter alignment");

#define WWS_UNUSED(p) (p)

template<typename T, size_t N> const char(*__array_dim_helper(const T(&)[N]))[N];
#define array_dim(Array) (sizeof(*__array_dim_helper(Array)) + 0)

#endif /* SRC_WWS_TYPES_H */
