// NVIDIA GVDB voxel operators, rewritten for explicit portable resources.
// Copyright 2017 NVIDIA Corporation. SPDX-License-Identifier: Apache-2.0
#include "GVDBTypes.slang"
struct OperatorParameters {
    VolumeInfo volume;
    float4 value;
    uint4 sourceResolution;
    float4 transformRows[3];
    float4 remap;
};
ConstantBuffer<OperatorParameters> g_Parameters : register(b0);
StructuredBuffer<float> g_Source : register(t0);
StructuredBuffer<int4> g_AtlasMap : register(t1);
ByteAddressBuffer g_Nodes : register(t2);
ByteAddressBuffer g_Children : register(t3);
StructuredBuffer<LevelInfo> g_Levels : register(t4);
RWStructuredBuffer<float> g_Destination : register(u0);

bool validVoxel(uint3 id) { return all(id < g_Parameters.volume.atlasResolutionAndApron.xyz); }
uint voxelIndex(uint3 id) { return flatten3(id, g_Parameters.volume.atlasResolutionAndApron.xyz); }
float loadClamped(int3 p) {
    uint3 res = g_Parameters.volume.atlasResolutionAndApron.xyz;
    return g_Source[flatten3(uint3(clamp(p, int3(0), int3(res) - 1)), res)];
}
float3 sourcePoint(float3 point) {
    float4 p = float4(point, 1);
    return float3(dot(g_Parameters.transformRows[0], p), dot(g_Parameters.transformRows[1], p), dot(g_Parameters.transformRows[2], p));
}
float remapValue(float value) {
    return g_Parameters.remap.z + (value - g_Parameters.remap.x) *
        (g_Parameters.remap.w - g_Parameters.remap.z) / (g_Parameters.remap.y - g_Parameters.remap.x);
}
uint hashValue(uint x) {
    x += x << 10; x ^= x >> 6; x += x << 3; x ^= x >> 11; x += x << 15;
    return x;
}
float randomValue(float3 value) {
    uint3 bits = asuint(value);
    uint hash = hashValue(bits.x ^ hashValue(bits.y) ^ hashValue(bits.z));
    return asfloat((hash & 0x007fffffu) | 0x3f800000u) - 1;
}
[shader("compute")]
[numthreads(8, 8, 8)]
void clearAtlas(uint3 id : SV_DispatchThreadID) {
    if (validVoxel(id)) g_Destination[voxelIndex(id)] = 0;
}
[shader("compute")]
[numthreads(8, 8, 8)]
void fillAtlas(uint3 id : SV_DispatchThreadID) {
    if (!validVoxel(id)) return;
    float value = g_Parameters.value.x;
    if (g_Parameters.value.z < 0) {
        float3 s = sin(float3(id) * (12.0 / (3.141592 * 30.0)));
        value = s.x + s.y + s.z;
    }
    g_Destination[voxelIndex(id)] = value;
}
// Neighborhood passes require separate source and destination buffers. This
// makes inter-group ordering deterministic, unlike in-place CUDA stencils.
[shader("compute")]
[numthreads(8, 8, 8)]
void smoothAtlas(uint3 id : SV_DispatchThreadID) {
    if (!validVoxel(id)) return;
    float value = g_Source[voxelIndex(id)];
    if (isAtlasInterior(g_Parameters.volume, id)) {
        int3 p = int3(id);
        float neighbors = loadClamped(p + int3(-1, 0, 0)) + loadClamped(p + int3(1, 0, 0)) +
            loadClamped(p + int3(0, -1, 0)) + loadClamped(p + int3(0, 1, 0)) +
            loadClamped(p + int3(0, 0, -1)) + loadClamped(p + int3(0, 0, 1));
        value = (g_Parameters.value.x * value + neighbors) / (g_Parameters.value.x + 6) + g_Parameters.value.y;
    }
    g_Destination[voxelIndex(id)] = value;
}
[shader("compute")]
[numthreads(8, 8, 8)]
void noiseAtlas(uint3 id : SV_DispatchThreadID) {
    if (!validVoxel(id)) return;
    float value = g_Source[voxelIndex(id)];
    if (isAtlasInterior(g_Parameters.volume, id) && value > 0.01)
        value += randomValue(float3(id)) * g_Parameters.value.x;
    g_Destination[voxelIndex(id)] = value;
}
[shader("compute")]
[numthreads(8, 8, 8)]
void growAtlas(uint3 id : SV_DispatchThreadID) {
    if (!validVoxel(id)) return;
    float value = g_Source[voxelIndex(id)];
    if (isAtlasInterior(g_Parameters.volume, id)) {
        if (value != 0) value += g_Parameters.value.x * 10;
        if (value < 0.01) value = 0;
    }
    g_Destination[voxelIndex(id)] = value;
}
[shader("compute")]
[numthreads(8, 8, 8)]
void cutAtlas(uint3 id : SV_DispatchThreadID) {
    if (!validVoxel(id)) return;
    float value = g_Source[voxelIndex(id)];
    float3 world;
    if (atlasToWorld(g_AtlasMap, g_Parameters.volume, id, world) &&
        isAtlasInterior(g_Parameters.volume, id) && world.x < 50 && value > 0 && value < 2) value = 0.02;
    g_Destination[voxelIndex(id)] = value;
}
// One invocation per voxel, including edges/corners and arbitrary apron width.
// Read from a stable source; never race duplicate face writes.
[shader("compute")]
[numthreads(8, 8, 8)]
void updateApron(uint3 id : SV_DispatchThreadID) {
    if (!validVoxel(id)) return;
    float value = g_Source[voxelIndex(id)];
    if (!isAtlasInterior(g_Parameters.volume, id)) {
        float3 world;
        if (atlasToWorld(g_AtlasMap, g_Parameters.volume, id, world)) {
            VDBNode leaf;
            uint index;
            value = g_Parameters.value.x;
            if (findLeaf(g_Nodes, g_Children, g_Levels, g_Parameters.volume, world, leaf, index)) {
                uint3 neighbor = uint3(float3(leaf.atlasPosition) + world - float3(leaf.position));
                value = g_Source[voxelIndex(neighbor)];
            }
        }
    }
    g_Destination[voxelIndex(id)] = value;
}
[shader("compute")]
[numthreads(8, 8, 8)]
void resampleAtlas(uint3 id : SV_DispatchThreadID) {
    if (!validVoxel(id)) return;
    float value = g_Parameters.value.x; // explicit background for absent data
    float3 world;
    if (atlasToWorld(g_AtlasMap, g_Parameters.volume, id, world)) {
        int3 source = int3(sourcePoint(world - 0.5));
        uint3 res = g_Parameters.sourceResolution.xyz;
        if (all(source >= 0) && all(source < int3(res)))
            value = remapValue(g_Source[flatten3(uint3(source), res)]);
    }
    g_Destination[voxelIndex(id)] = value;
}
// Each destination voxel integrates a non-overlapping source box. The transform
// must be axis aligned with nonnegative scale (validated by the caller).
[shader("compute")]
[numthreads(8, 8, 8)]
void downsampleDense(uint3 id : SV_DispatchThreadID) {
    if (!validVoxel(id)) return;
    uint3 sourceRes = g_Parameters.sourceResolution.xyz;
    int3 lower = clamp(int3(floor(sourcePoint(float3(id)))), int3(0), int3(sourceRes) - 1);
    int3 upper = clamp(int3(ceil(sourcePoint(float3(id + 1)))) - 1, lower, int3(sourceRes) - 1);
    float sum = 0;
    uint count = 0;
    for (int z = lower.z; z <= upper.z; ++z)
        for (int y = lower.y; y <= upper.y; ++y)
            for (int x = lower.x; x <= upper.x; ++x) {
                sum += remapValue(g_Source[flatten3(uint3(x, y, z), sourceRes)]);
                ++count;
            }
    g_Destination[voxelIndex(id)] = sum / float(count);
}
[shader("compute")]
[numthreads(8, 8, 1)]
void reduceAtlasColumns(uint3 id : SV_DispatchThreadID) {
    VolumeInfo volume = g_Parameters.volume;
    uint apron = volume.atlasResolutionAndApron.w;
    uint width = volume.atlasBrickCountAndWidth.w;
    uint interior = width - 2 * apron;
    uint3 packedRes = volume.atlasBrickCountAndWidth.xyz * interior;
    if (any(id.xy >= packedRes.xy)) return;
    uint2 xy = (id.xy / interior) * width + id.xy % interior + apron;
    float sum = 0;
    for (uint z = 0; z < packedRes.z; ++z) {
        uint az = (z / interior) * width + z % interior + apron;
        sum += g_Source[voxelIndex(uint3(xy, az))];
    }
    g_Destination[id.y * packedRes.x + id.x] = sum;
}
