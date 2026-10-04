#pragma once

// Runtime meshlet builder used for assimp-loaded space objects (the cargo ship, test monolith).
// Asteroids.exe: Build 0x14006C5B0 (entry thunk 0x14006D800), greedy clustering 0x14006CBA0,
// culling data 0x14006C000, Forsyth vertex-cache optimiser 0x14006EBB0 (tables 0x14006E320).
// The output matches the .chk meshlet layout (docs/formats.md): u32 vertex-index lists,
// u8 meshlet-local triangle indices and 16-byte meshlet headers with quantized bounds.

#include "meshlets/ChunkMeshSet.h"

#include <donut/core/math/math.h>

#include <memory>
#include <vector>

struct MeshletSubset
{
    uint32_t indexOffset;   // first index of the subset in the index list
    uint32_t numPrims;
    uint32_t vertexOffset;  // added to the subset's indices when 'rebaseSubsets' is set
};

struct MeshletBuildDesc
{
    const uint8_t* positions = nullptr;     // float3 positions (first attribute with semantic <= POSITION)
    uint32_t positionStride = 12;
    uint32_t numVertices = 0;
    const uint32_t* indices = nullptr;
    uint32_t numIndices = 0;
    const MeshletSubset* subsets = nullptr;
    uint32_t numSubsets = 0;
    bool rebaseSubsets = true;
};

// The 2018 options dword: [7:0] maxVerts, [15:8] maxPrims, bit16 vertex-cache optimisation,
// [24:17] cache size (<= 64), bits 25..27 vertex remap / index padding (never used by the demo,
// which passes 0x00816440).
struct MeshletBuildOptions
{
    uint32_t maxVerts = 64;
    uint32_t maxPrims = 100;
    bool optimizeVertexCache = true;
    uint32_t cacheSize = 64;
};

// 2018 MeshletData (0x98 bytes; also filled by the .chk loader)
struct SceneMeshletData
{
    std::vector<uint32_t> vertexIndices;
    std::vector<uint8_t> primIndices;
    std::vector<chunk2018::MeshletHeader> meshlets;
    donut::math::box3 bounds = donut::math::box3::empty();
    uint32_t numVertices = 0;
    uint32_t numPrims = 0;
    std::vector<donut::math::uint2> subsets;    // {firstMeshlet, numMeshlets} per input subset
};

// Returns null when there are no positions.
std::unique_ptr<SceneMeshletData> BuildMeshlets(const MeshletBuildDesc& desc, const MeshletBuildOptions& options);

// Forsyth "linear-speed vertex cache optimisation" on a 32-bit triangle list, in place.
// No-op if any vertex is referenced by more than 254 triangles.
void OptimizeVertexCache(uint32_t* indices, size_t numIndices, uint32_t cacheSize);
