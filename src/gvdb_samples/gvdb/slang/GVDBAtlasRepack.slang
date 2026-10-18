// SPDX-License-Identifier: Apache-2.0
#include "GVDBTypes.slang"
struct RepackParameters {
    VolumeInfo volume;
    uint4 sourceResolutionAndStride;
    uint4 work; // destination voxels, words, valid bytes in last word, active leaves
    uint4 voidBits;
};
ConstantBuffer<RepackParameters> parameters : register(b0);
ByteAddressBuffer nodePool : register(t0);
ByteAddressBuffer childPool : register(t1);
StructuredBuffer<LevelInfo> levelMetadata : register(t2);
StructuredBuffer<int4> atlasMap : register(t3);
StructuredBuffer<int4> oldInteriorByLeaf : register(t4);
ByteAddressBuffer sourceAtlas : register(t5);
RWByteAddressBuffer destinationAtlas : register(u0);

// The owner validates coordinates and node extents. Integer traversal also avoids
// rounding relative coordinates at the edge of the accepted float metadata domain.
bool findIntegerLeaf(int3 position, out VDBNode leaf, out uint leafIndex) {
    leaf = (VDBNode)0;
    leafIndex = GVDB_INVALID;
    uint level = parameters.volume.hierarchy.x;
    uint index = parameters.volume.hierarchy.y;
    [loop] for (uint depth = 0; depth < 10; ++depth) {
        if (level >= parameters.volume.hierarchy.z) return false;
        LevelInfo info = levelMetadata[level];
        if (index >= info.nodes.z) return false;
        VDBNode node = loadNode(nodePool, info.nodes.x + index * info.nodes.y);
        // Compare before subtracting. CPU endpoint validation makes the subtraction safe.
        if (any(position < node.position) || any(position >= node.position + info.nodeExtent.xyz)) return false;
        uint3 relative = uint3(position - node.position);
        if (level == 0) {
            if (((node.metadata >> 8) & 255u) == 0) return false;
            leaf = node;
            leafIndex = index;
            return true;
        }
        uint2 list = node.childList;
        if (nullReference(list) || !referenceFitsIndex(list)) return false;
        uint listLevel = referenceLevel(list);
        if (listLevel >= parameters.volume.hierarchy.z) return false;
        LevelInfo lists = levelMetadata[listLevel];
        uint listIndex = referenceIndex(list);
        if (listIndex >= lists.children.z) return false;
        uint3 childPosition = relative / uint3(info.childExtent.xyz);
        uint childBit = flatten3(childPosition, uint3(info.children.w));
        uint2 child = childPool.Load2(lists.children.x + listIndex * lists.children.y + childBit * 8);
        if (nullReference(child) || !referenceFitsIndex(child)) return false;
        index = referenceIndex(child);
        --level;
    }
    return false;
}
uint backgroundByte(uint component) {
    return (parameters.voidBits[component / 4] >> ((component % 4) * 8)) & 255u;
}
uint remapByte(uint address) {
    uint stride = parameters.sourceResolutionAndStride.w;
    uint voxel = address / stride;
    uint component = address % stride;
    if (voxel >= parameters.work.x) return 0;
    uint3 coordinate = unflatten3(voxel, parameters.volume.atlasResolutionAndApron.xyz);
    uint pitch = parameters.volume.atlasBrickCountAndWidth.w;
    uint slot = flatten3(coordinate / pitch, parameters.volume.atlasBrickCountAndWidth.xyz);
    int4 mapping = atlasMap[slot];
    if (mapping.w < 0) return 0; // Unused slots are zero, independently of void value.
    int3 position = mapping.xyz + int3(coordinate % pitch) - int(parameters.volume.atlasResolutionAndApron.w);
    VDBNode leaf;
    uint index;
    if (!findIntegerLeaf(position, leaf, index)) return backgroundByte(component);
    int4 old = oldInteriorByLeaf[index];
    if (old.w < 0 || parameters.sourceResolutionAndStride.x == 0) return backgroundByte(component);
    uint3 source = uint3(old.xyz + (position - leaf.position));
    uint sourceAddress = flatten3(source, parameters.sourceResolutionAndStride.xyz) * stride + component;
    uint value = sourceAtlas.Load(sourceAddress & ~3u);
    return (value >> ((sourceAddress & 3u) * 8)) & 255u;
}
[shader("compute")]
[numthreads(256, 1, 1)]
void relocateAtlas(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint word = dispatchThreadId.x + dispatchThreadId.y * (65535u * 256u);
    if (word >= parameters.work.y) return;
    uint value = 0;
    [unroll] for (uint byteIndex = 0; byteIndex < 4; ++byteIndex) {
        if (word + 1 < parameters.work.y || byteIndex < parameters.work.z)
            value |= remapByte(word * 4 + byteIndex) << (byteIndex * 8);
    }
    destinationAtlas.Store(word * 4, value);
}
