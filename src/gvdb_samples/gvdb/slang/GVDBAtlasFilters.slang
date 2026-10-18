// SPDX-License-Identifier: Apache-2.0
// Separate source/destination per pass; one invocation owns each complete raw word.
#include "GVDBTypes.slang"
struct FilterParameters {
    VolumeInfo volume;
    uint4 work;
    uint4 operation; // operation, apron pass, format, reserved
    float4 values;
    uint4 boundaryBits;
};
ConstantBuffer<FilterParameters> parameters : register(b0);
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

uint hashValue(uint value) {
    value += value << 10; value ^= value >> 6;
    value += value << 3; value ^= value >> 11; value += value << 15;
    return value;
}
float randomValue(uint3 coordinate) {
    uint3 bits = asuint(float3(coordinate));
    uint hash = hashValue(bits.x ^ hashValue(bits.y) ^ hashValue(bits.z));
    return asfloat((hash & 0x007fffffu) | 0x3f800000u) - 1.0;
}
uint voxelAddress(uint3 coordinate) {
    return flatten3(coordinate, parameters.volume.atlasResolutionAndApron.xyz) * parameters.work.x;
}
uint neighborAddress(uint3 coordinate, uint side) {
    // Called only for interiors with >=1 apron, so every address is inside the source atlas.
    if (side == 0) coordinate.x -= 1;
    else if (side == 1) coordinate.x += 1;
    else if (side == 2) coordinate.y -= 1;
    else if (side == 3) coordinate.y += 1;
    else if (side == 4) coordinate.z -= 1;
    else coordinate.z += 1;
    return voxelAddress(coordinate);
}
int3 colorValue(uint address, float weight) {
    uint packed = sourceAtlas.Load(address);
    return int3(float3(packed & 255u, (packed >> 8) & 255u, (packed >> 16) & 255u) * weight);
}
uint filteredByte(uint address, uint3 coordinate, int3 worldCell, uint component) {
    uint operation = parameters.operation.x;
    float4 p = parameters.values;
    uint result = sourceByte(address);
    [branch] if (operation == 4) {
        if (result == 0) {
            uint label = uint(p.x);
            bool adjacent = false;
            [loop] for (uint side = 0; side < 6; ++side)
                adjacent = adjacent || sourceByte(neighborAddress(coordinate, side)) == label;
            if (adjacent) result = uint(p.y);
        }
    } else if (operation == 5) {
        int3 color = colorValue(voxelAddress(coordinate), p.x);
        [loop] for (uint side = 0; side < 6; ++side) color += colorValue(neighborAddress(coordinate, side), p.y);
        int maximum = max(color.x, max(color.y, color.z));
        [branch] if (maximum > 255) {
            int3 numerator = color * 255;
            uint3 quotient = uint3(abs(numerator)) / uint(maximum);
            color = int3(numerator.x < 0 ? -int(quotient.x) : int(quotient.x),
                         numerator.y < 0 ? -int(quotient.y) : int(quotient.y),
                         numerator.z < 0 ? -int(quotient.z) : int(quotient.z));
        }
        result = uint(component == 0 ? color.x : component == 1 ? color.y : color.z) & 255u;
        if (component == 3) result = 1;
    } else {
        uint componentOffset = (component / 4) * 4;
        float value = asfloat(sourceAtlas.Load(voxelAddress(coordinate) + componentOffset));
        if (operation == 0) {
            value *= p.x;
            [loop] for (uint side = 0; side < 6; ++side)
                value += asfloat(sourceAtlas.Load(neighborAddress(coordinate, side) + componentOffset));
            value = value / (p.x + 6.0) + p.y;
        } else if (operation == 1) {
            if (value != 0.0) value += p.x * 10.0;
            if (value < 0.01) value = 0.0;
        } else if (operation == 2) {
            if (value > 0.01) value += randomValue(coordinate) * p.x;
        } else if (operation == 3) {
            // Integer cell x<50 is equivalent to retained (cell.x + 0.5)<50.
            if (worldCell.x < 50 && value > 0.0 && value < 2.0) value = 0.02;
        }
        result = (asuint(value) >> ((component % 4) * 8)) & 255u;
    }
    return result;
}
uint filterByte(uint address) {
    uint stride = parameters.work.x;
    uint voxel = address / stride;
    uint component = address % stride;
    if (voxel >= parameters.work.y) return 0;
    uint3 coordinate = unflatten3(voxel, parameters.volume.atlasResolutionAndApron.xyz);
    uint pitch = parameters.volume.atlasBrickCountAndWidth.w;
    uint slot = flatten3(coordinate / pitch, parameters.volume.atlasBrickCountAndWidth.xyz);
    int4 mapping = atlasMap[slot];
    if (mapping.w < 0) return 0;
    uint apron = parameters.volume.atlasResolutionAndApron.w;
    uint3 local = coordinate % pitch;
    bool interior = all(local >= apron) && all(local < pitch - apron);
    int3 position = mapping.xyz + int3(local) - int(apron);
    [branch] if (parameters.operation.y == 0) {
        if (!interior) return sourceByte(address);
        return filteredByte(address, coordinate, position, component);
    }
    if (interior) return sourceByte(address);
    VDBNode leaf;
    uint leafIndex;
    if (!findIntegerLeaf(position, leaf, leafIndex)) return bitsByte(parameters.boundaryBits, component);
    uint3 adjacent = uint3(leaf.atlasPosition + (position - leaf.position));
    return sourceByte(voxelAddress(adjacent) + component);
}
[shader("compute")]
[numthreads(256, 1, 1)]
void filterAtlas(uint3 id : SV_DispatchThreadID) {
    uint word = id.x + id.y * (65535u * 256u);
    if (word >= parameters.work.z) return;
    uint packed = 0;
    [loop] for (uint lane = 0; lane < 4; ++lane) {
        if (word + 1 < parameters.work.z || lane < parameters.work.w)
            packed |= filterByte(word * 4 + lane) << (lane * 8);
    }
    destinationAtlas.Store(word * 4, packed);
}
