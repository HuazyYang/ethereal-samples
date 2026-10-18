// Copyright (c) 2026. SPDX-License-Identifier: Apache-2.0
// Host-side layouts for the portable Slang GVDB execution representation.
#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace gvdb::slang {
struct UInt2 { uint32_t x, y; };
struct UInt4 { uint32_t x, y, z, w; };
struct Int4 { int32_t x, y, z, w; };
struct Float4 { float x, y, z, w; };
constexpr uint32_t kInvalidIndex = 0xffffffffu;
constexpr uint32_t kScanBlockSize = 256;
constexpr uint32_t kMaxLevels = 10;

// The VBX node is copied verbatim into a raw buffer. Keeping it in words avoids
// differences in float3 alignment between DXBC, SPIR-V, and CUDA.
struct alignas(16) SerializedNode { uint32_t words[16]{}; };
struct alignas(16) LevelInfo {
    UInt4 nodes;          // byte offset, stride, count, log2 child resolution
    UInt4 children;       // byte offset, stride, count, child resolution
    Float4 childExtent;  // child size in index space; w unused
    Int4 nodeExtent;     // node size in index space; w unused
};
struct alignas(16) VolumeInfo {
    UInt4 atlasResolutionAndApron;
    UInt4 atlasBrickCountAndWidth;
    UInt4 hierarchy;      // top level, root index, level count, reserved
    Float4 boundsMin;
    Float4 boundsMax;
};
struct alignas(16) AtlasNode { int32_t x, y, z, leaf; };
struct KeyValue { UInt2 key; uint32_t value, reserved; };
struct alignas(16) Reduction { float minimum, maximum, sum; uint32_t count; };
struct alignas(16) AlgorithmParameters { uint32_t count, bit, reserved0, reserved1; };
struct alignas(16) OperatorParameters {
    VolumeInfo volume;
    Float4 value;
    UInt4 sourceResolution;
    Float4 transformRows[3];
    Float4 remap;         // source min/max, destination min/max
};

// Values match the legacy GVDB AtlasFormat enum (not VBX channel type codes). Raw storage preserves RGB32
// at 12 bytes and R8 at one byte, independent of backend texture capabilities.
enum class AtlasFormat : uint32_t { R8_UINT=0, RGBA8_UINT=1, R32_FLOAT=2, RGB32_FLOAT=3, RGBA32_FLOAT=4 };
constexpr uint32_t atlasStride(AtlasFormat format) {
    switch(format) {
    case AtlasFormat::R8_UINT: return 1;
    case AtlasFormat::RGBA8_UINT: case AtlasFormat::R32_FLOAT: return 4;
    case AtlasFormat::RGB32_FLOAT: return 12;
    case AtlasFormat::RGBA32_FLOAT: return 16;
    default: return 0;
    }
}
constexpr uint64_t atlasStorageBytes(AtlasFormat format,uint32_t voxels) {
    return (uint64_t(atlasStride(format))*voxels+3u)&~uint64_t(3);
}
struct alignas(16) RawOperatorParameters {
    VolumeInfo volume;
    Float4 value;
    UInt4 channel; // format, byte stride, voxel count, dispatch invocation count
};
static_assert(sizeof(RawOperatorParameters)==112);
struct alignas(16) ParticleParameters {
    VolumeInfo volume;
    UInt4 counts;
    Float4 translationAndSupport;
};
struct alignas(16) PointKeyParameters {
    UInt4 counts; // points, levels, total point-level keys, selected level
    Float4 translation;
    UInt4 grid; // active-brick grid dimensions; reserved
};
static_assert(sizeof(PointKeyParameters)==48);
struct alignas(16) GatherCell {
    Int4 worldOriginAndWidth; // integer voxel origin, cube width in [1,8]
    UInt4 atlasOrigin; // xyz, reserved
    UInt4 points; // first point, count, reserved, reserved
};
struct alignas(16) GatherParameters {
    UInt4 counts; // cell count, atlas voxel count, colors present, accumulate
    Float4 kernel; // radius, initial scalar value, reserved, reserved
    UInt4 atlasResolution;
};
static_assert(sizeof(GatherCell)==48&&sizeof(GatherParameters)==48);
enum class VoxelFormat:uint32_t { R8_UINT=0, R32_FLOAT=3, R32_SINT=6 };
struct alignas(16) Triangle {Float4 a,b,c;};
struct alignas(16) VoxelizeParameters {
    Float4 boundsMinimum,boundsMaximum;
    UInt4 resolution; // xyz, voxel count
    Float4 values; // surface, inside, outside, y-bin width
    UInt4 counts; // vertices, triangles, y bins, VoxelFormat
    Float4 transformRows[3];
};
static_assert(sizeof(Triangle)==48&&sizeof(VoxelizeParameters)==128);
struct alignas(16) VelocityParameters {
    VolumeInfo volume;
    UInt4 counts; // particles, advect, reserved, reserved
    Float4 motion; // time step, FLIP weight [0,1], reserved, reserved
};
static_assert(sizeof(VelocityParameters)==112);
struct alignas(16) SubcellParameters {
    VolumeInfo volume;
    UInt4 counts; // particles, leaves, subcell width, subcells per leaf
    Float4 translationAndRadius;
    UInt4 layout; // subcells per leaf side, total cells, colors present, reserved
};
static_assert(sizeof(SubcellParameters)==128);
struct alignas(16) PointConversionParameters {
    UInt4 source; // count, component bytes (1/2/4/8), byte offset, byte stride
    Float4 minimum,delta,scale,translation;
};
static_assert(sizeof(PointConversionParameters)==80);
struct alignas(16) SphParameters {
    Float4 gridMinimumAndCellWidth;
    UInt4 gridResolutionAndCount;
    Float4 physical;
    Float4 kernels;
    Float4 boundaryMinimum;
    Float4 boundaryMaximum;
    Float4 boundary;
    Float4 integration;
    Float4 gravityAndSlope;
    // frequency, left force, right force, mode: 0 preserves pool phase/stiffness;
    // 1 uses FLUIDS-v3 moving walls and interior insertion cells [1,resolution-3].
    Float4 wall;
};
static_assert(sizeof(ParticleParameters) == 112 && sizeof(SphParameters) == 160);
struct alignas(16) TopologyParameters { VolumeInfo volume; UInt4 operation; };
struct alignas(16) RaycastParameters {
    VolumeInfo volume;
    UInt4 image;
    Float4 rayOrigin;
    // xyz: lower image-corner direction; w: 0 keeps ray-distance near/far,
    // 1 treats march.y/z as view-Z planes along the normalized image-center ray.
    Float4 direction00;
    Float4 directionDx;
    Float4 directionDy;
    Float4 march;
    Float4 valueRange;
    Float4 background;
    Float4 light;
};
static_assert(sizeof(TopologyParameters) == 96 && sizeof(RaycastParameters) == 224);
constexpr UInt2 makeReference(uint8_t group, uint8_t level, uint64_t index) {
    const uint64_t value = uint64_t(group) | (uint64_t(level) << 8) | (index << 16);
    return {uint32_t(value), uint32_t(value >> 32)};
}
constexpr uint64_t referenceIndex(UInt2 value) {
    return (uint64_t(value.y) << 16) | (value.x >> 16);
}
constexpr uint32_t referenceGroup(UInt2 value) { return value.x & 255u; }
constexpr uint32_t referenceLevel(UInt2 value) { return (value.x >> 8) & 255u; }
constexpr bool isNull(UInt2 value) { return value.x == kInvalidIndex && value.y == kInvalidIndex; }
constexpr bool keyLess(UInt2 a, UInt2 b) { return a.y < b.y || (a.y == b.y && a.x < b.x); }
// Portable kernels address buffers with 32-bit offsets. The host must reject a
// pool beyond that limit instead of truncating a 48-bit serialized node index.
constexpr bool addressable(UInt2 ref, const LevelInfo& level, bool children) {
    const UInt4 pool = children ? level.children : level.nodes;
    const uint64_t index = referenceIndex(ref);
    return !isNull(ref) && index < pool.z && pool.y != 0 &&
        uint64_t(pool.x) + index * pool.y + pool.y <= uint64_t(UINT32_MAX) + 1;
}
constexpr uint32_t scanBlockCount(uint32_t count) {
    return count / kScanBlockSize + uint32_t(count % kScanBlockSize != 0);
}
static_assert(sizeof(SerializedNode) == 64 && alignof(SerializedNode) == 16);
static_assert(sizeof(LevelInfo) == 64 && sizeof(VolumeInfo) == 80);
static_assert(sizeof(AtlasNode) == 16 && sizeof(KeyValue) == 16 && sizeof(Reduction) == 16);
static_assert(sizeof(OperatorParameters) == 176);
static_assert(std::is_trivially_copyable_v<SerializedNode>);
} // namespace gvdb::slang
