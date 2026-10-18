// SPDX-License-Identifier: Apache-2.0
// One invocation owns each complete destination word, including R8 brick/row boundaries.
#include "GVDBTypes.slang"
struct MutationParameters {
    VolumeInfo volume;
    uint4 work; // stride, voxels, words, last word valid bytes
    uint4 operation; // clear, fill, copy, apron, legacy sine
    uint4 fillBits;
    uint4 voidBits;
};
ConstantBuffer<MutationParameters> parameters : register(b0);
ByteAddressBuffer nodePool : register(t0);
ByteAddressBuffer childPool : register(t1);
StructuredBuffer<LevelInfo> levelMetadata : register(t2);
StructuredBuffer<int4> atlasMap : register(t3);
ByteAddressBuffer sourceAtlas : register(t4);
RWByteAddressBuffer destinationAtlas : register(u0);
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

uint sourceByte(uint address) {
    return (sourceAtlas.Load(address & ~3u) >> ((address & 3u) * 8)) & 255u;
}
uint bitsByte(uint4 bits, uint component) {
    return (bits[component / 4] >> ((component % 4) * 8)) & 255u;
}
uint mutationByte(uint address) {
    uint stride = parameters.work.x;
    uint voxel = address / stride;
    uint component = address % stride;
    if (voxel >= parameters.work.y) return 0;
    uint3 coordinate = unflatten3(voxel, parameters.volume.atlasResolutionAndApron.xyz);
    uint pitch = parameters.volume.atlasBrickCountAndWidth.w;
    uint slot = flatten3(coordinate / pitch, parameters.volume.atlasBrickCountAndWidth.xyz);
    int4 mapping = atlasMap[slot];
    if (mapping.w < 0) return 0;
    uint operation = parameters.operation.x;
    if (operation == 0) return 0;
    if (operation == 1) return bitsByte(parameters.fillBits, component);
    if (operation == 2) return sourceByte(address);
    if (operation == 4) {
        float3 wave = sin(float3(coordinate) * (12.0 / (3.141592 * 30.0)));
        uint bits = asuint(wave.x + wave.y + wave.z);
        return (bits >> (component * 8)) & 255u;
    }
    uint apron = parameters.volume.atlasResolutionAndApron.w;
    uint3 local = coordinate % pitch;
    if (all(local >= apron) && all(local < pitch - apron)) return sourceByte(address);
    int3 position = mapping.xyz + int3(local) - int(apron);
    VDBNode leaf;
    uint leafIndex;
    if (!findIntegerLeaf(position, leaf, leafIndex)) return bitsByte(parameters.voidBits, component);
    uint3 adjacent = uint3(leaf.atlasPosition + (position - leaf.position));
    uint neighbor = flatten3(adjacent, parameters.volume.atlasResolutionAndApron.xyz);
    return sourceByte(neighbor * stride + component);
}
[shader("compute")]
[numthreads(256, 1, 1)]
void mutateAtlas(uint3 id : SV_DispatchThreadID) {
    uint word = id.x + id.y * (65535u * 256u);
    if (word >= parameters.work.z) return;
    uint packed = 0;
    [unroll] for (uint lane = 0; lane < 4; ++lane) {
        if (word + 1 < parameters.work.z || lane < parameters.work.w)
            packed |= mutationByte(word * 4 + lane) << (lane * 8);
    }
    destinationAtlas.Store(word * 4, packed);
}
