// NVIDIA GVDB portable execution representation. SPDX-License-Identifier: Apache-2.0
#ifndef GVDB_PORTABLE_TYPES_SLANG
#define GVDB_PORTABLE_TYPES_SLANG
static const uint GVDB_INVALID = 0xffffffffu;
static const uint GVDB_SCAN_BLOCK_SIZE = 256;
struct LevelInfo {
    uint4 nodes;
    uint4 children;
    float4 childExtent;
    int4 nodeExtent;
};
struct VolumeInfo {
    uint4 atlasResolutionAndApron;
    uint4 atlasBrickCountAndWidth;
    uint4 hierarchy;
    float4 boundsMin;
    float4 boundsMax;
};
struct VDBNode {
    uint metadata;
    int3 position;
    int3 atlasPosition;
    float3 range;
    uint2 parent;
    uint2 childList;
    uint2 mask;
};
struct KeyValue { uint2 key; uint value; uint reserved; };
struct Reduction { float minimum; float maximum; float sum; uint count; };

bool nullReference(uint2 ref) { return all(ref == uint2(GVDB_INVALID)); }
uint referenceLevel(uint2 ref) { return (ref.x >> 8) & 255u; }
uint referenceIndex(uint2 ref) { return (ref.y << 16) | (ref.x >> 16); }
bool referenceFitsIndex(uint2 ref) { return (ref.y >> 16) == 0; }
bool keyLess(uint2 a, uint2 b) { return a.y < b.y || (a.y == b.y && a.x < b.x); }
uint flatten3(uint3 p, uint3 res) { return (p.z * res.y + p.y) * res.x + p.x; }
uint3 unflatten3(uint index, uint3 res) {
    uint3 p;
    p.x = index % res.x;
    index /= res.x;
    p.y = index % res.y;
    p.z = index / res.y;
    return p;
}
VDBNode loadNode(ByteAddressBuffer pools, uint address) {
    VDBNode node;
    node.metadata = pools.Load(address);
    node.position = asint(pools.Load3(address + 4));
    node.atlasPosition = asint(pools.Load3(address + 16));
    node.range = asfloat(pools.Load3(address + 28));
    node.parent = pools.Load2(address + 40);
    node.childList = pools.Load2(address + 48);
    node.mask = pools.Load2(address + 56);
    return node;
}
// Pools combine all levels in one binding. Level offsets are supplied explicitly;
// the serialized parent/child references retain their original VBX encoding.
bool findLeaf(ByteAddressBuffer pools, ByteAddressBuffer childPools,
    StructuredBuffer<LevelInfo> levels, VolumeInfo volume, float3 position,
    out VDBNode leaf, out uint leafIndex) {
    leaf = (VDBNode)0;
    leafIndex = GVDB_INVALID;
    uint level = volume.hierarchy.x;
    uint index = volume.hierarchy.y;
    if (level >= volume.hierarchy.z || volume.hierarchy.z > 10) return false;
    for (uint depth = 0; depth < 10; ++depth) {
        LevelInfo info = levels[level];
        if (index >= info.nodes.z) return false;
        VDBNode node = loadNode(pools, info.nodes.x + index * info.nodes.y);
        float3 relative = position - float3(node.position);
        if (any(relative < 0) || any(relative >= float3(info.nodeExtent.xyz))) return false;
        if (level == 0) {
            if (((node.metadata >> 8) & 255u) == 0) return false;
            leaf = node;
            leafIndex = index;
            return true;
        }
        uint2 list = node.childList;
        if (nullReference(list) || !referenceFitsIndex(list)) return false;
        uint listLevel = referenceLevel(list);
        if (listLevel >= volume.hierarchy.z) return false;
        LevelInfo childInfo = levels[listLevel];
        uint listIndex = referenceIndex(list);
        if (listIndex >= childInfo.children.z) return false;
        uint3 childPosition = uint3(relative / info.childExtent.xyz);
        uint childBit = flatten3(childPosition, uint3(info.children.w));
        uint2 child = childPools.Load2(childInfo.children.x + listIndex * childInfo.children.y + childBit * 8);
        if (nullReference(child) || !referenceFitsIndex(child)) return false;
        index = referenceIndex(child);
        --level;
    }
    return false;
}
bool atlasToWorld(StructuredBuffer<int4> atlasMap, VolumeInfo volume, uint3 voxel,
    out float3 position) {
    uint brickWidth = volume.atlasBrickCountAndWidth.w;
    uint3 brick = voxel / brickWidth;
    int4 atlas = atlasMap[flatten3(brick, volume.atlasBrickCountAndWidth.xyz)];
    position = float3(atlas.xyz) + float3(int3(voxel % brickWidth) - int(volume.atlasResolutionAndApron.w)) + 0.5;
    return atlas.w >= 0;
}
bool isAtlasInterior(VolumeInfo volume, uint3 voxel) {
    uint3 local = voxel % volume.atlasBrickCountAndWidth.w;
    uint apron = volume.atlasResolutionAndApron.w;
    return all(local >= apron) && all(local < volume.atlasBrickCountAndWidth.w - apron);
}
#endif
