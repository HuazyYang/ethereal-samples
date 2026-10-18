// Sparse topology editing without GPU pointers or 64-bit atomics.
// SPDX-License-Identifier: Apache-2.0
#include "GVDBTypes.slang"
struct TopologyParameters { VolumeInfo volume; uint4 operation; }; // operation.x: level
ConstantBuffer<TopologyParameters> g_Parameters : register(b0);
StructuredBuffer<LevelInfo> g_Levels : register(t0);
StructuredBuffer<uint> g_Markers : register(t1);
RWByteAddressBuffer g_Nodes : register(u0);
RWByteAddressBuffer g_Children : register(u1);

VDBNode mutableNode(uint level, uint index) {
    uint address = g_Levels[level].nodes.x + index * g_Levels[level].nodes.y;
    VDBNode node;
    node.metadata = g_Nodes.Load(address);
    node.position = asint(g_Nodes.Load3(address + 4));
    node.atlasPosition = asint(g_Nodes.Load3(address + 16));
    node.range = asfloat(g_Nodes.Load3(address + 28));
    node.parent = g_Nodes.Load2(address + 40);
    node.childList = g_Nodes.Load2(address + 48);
    node.mask = g_Nodes.Load2(address + 56);
    return node;
}
uint2 nodeReference(uint level, uint index) { return uint2((index << 16) | (level << 8), index >> 16); }
bool childListAddress(uint2 reference, out uint address) {
    address = 0;
    uint level = referenceLevel(reference);
    if (nullReference(reference) || !referenceFitsIndex(reference) || level >= g_Parameters.volume.hierarchy.z) return false;
    LevelInfo info = g_Levels[level];
    uint index = referenceIndex(reference);
    if (index >= info.children.z) return false;
    address = info.children.x + index * info.children.y;
    return true;
}
bool findAncestor(float3 position, uint targetLevel, out uint index) {
    uint level = g_Parameters.volume.hierarchy.x;
    index = g_Parameters.volume.hierarchy.y;
    if (targetLevel > level || level >= g_Parameters.volume.hierarchy.z) return false;
    for (uint depth = 0; depth < 10; ++depth) {
        LevelInfo info = g_Levels[level];
        if (index >= info.nodes.z) return false;
        VDBNode node = mutableNode(level, index);
        float3 relative = position - float3(node.position);
        if (any(relative < 0) || any(relative >= float3(info.nodeExtent.xyz))) return false;
        if (level == targetLevel) return true;
        uint list;
        if (!childListAddress(node.childList, list)) return false;
        uint3 cell = uint3(relative / info.childExtent.xyz);
        uint2 child = g_Children.Load2(list + flatten3(cell, uint3(info.children.w)) * 8);
        if (nullReference(child) || !referenceFitsIndex(child)) return false;
        index = referenceIndex(child);
        --level;
    }
    return false;
}
void setActive(uint level, uint index, VDBNode node, bool active) {
    uint address = g_Levels[level].nodes.x + index * g_Levels[level].nodes.y;
    g_Nodes.Store(address, (node.metadata & ~0xff00u) | (active ? 0x100u : 0u));
}
void unlinkParent(uint level, uint index, VDBNode node) {
    if (nullReference(node.parent) || !referenceFitsIndex(node.parent)) return;
    uint parentLevel = referenceLevel(node.parent);
    uint parentIndex = referenceIndex(node.parent);
    if (parentLevel >= g_Parameters.volume.hierarchy.z || parentIndex >= g_Levels[parentLevel].nodes.z) return;
    VDBNode parent = mutableNode(parentLevel, parentIndex);
    uint list;
    if (!childListAddress(parent.childList, list)) return;
    LevelInfo info = g_Levels[parentLevel];
    int3 cell = int3(float3(node.position - parent.position) / info.childExtent.xyz);
    if (any(cell < 0) || any(cell >= int3(info.children.w))) return;
    uint slot = list + flatten3(uint3(cell), uint3(info.children.w)) * 8;
    // Unique node coordinates guarantee a single writer per parent slot.
    uint2 current = g_Children.Load2(slot);
    if (all(current == nodeReference(level, index))) g_Children.Store2(slot, uint2(GVDB_INVALID));
    g_Nodes.Store2(g_Levels[level].nodes.x + index * g_Levels[level].nodes.y + 40, uint2(GVDB_INVALID));
}
[shader("compute")]
[numthreads(256, 1, 1)]
void initializeChildLists(uint3 id : SV_DispatchThreadID) {
    LevelInfo info = g_Levels[g_Parameters.operation.x];
    uint references = info.children.z * (info.children.y / 8);
    if (id.x < references) g_Children.Store2(info.children.x + id.x * 8, uint2(GVDB_INVALID));
}
[shader("compute")]
[numthreads(256, 1, 1)]
void applyLeafMarkers(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_Levels[0].nodes.z) return;
    setActive(0, id.x, mutableNode(0, id.x), g_Markers[id.x] != 0);
}
[shader("compute")]
[numthreads(256, 1, 1)]
void delinkLeafBricks(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_Levels[0].nodes.z) return;
    VDBNode node = mutableNode(0, id.x);
    if (((node.metadata >> 8) & 255u) == 0) unlinkParent(0, id.x, node);
}
// Dispatch levels bottom-up, with a UAV barrier after every level.
[shader("compute")]
[numthreads(256, 1, 1)]
void delinkInternalBricks(uint3 id : SV_DispatchThreadID) {
    uint level = g_Parameters.operation.x;
    if (level == 0 || id.x >= g_Levels[level].nodes.z) return;
    VDBNode node = mutableNode(level, id.x);
    uint list;
    bool active = false;
    if (childListAddress(node.childList, list)) {
        uint width = g_Levels[level].children.w;
        for (uint child = 0; child < width * width * width; ++child) {
            if (!nullReference(g_Children.Load2(list + child * 8))) { active = true; break; }
        }
    }
    setActive(level, id.x, node, active);
    if (!active) unlinkParent(level, id.x, node);
}
// Dispatch levels top-down. All ancestors must already have linked child lists.
[shader("compute")]
[numthreads(256, 1, 1)]
void linkBricks(uint3 id : SV_DispatchThreadID) {
    uint level = g_Parameters.operation.x;
    if (level >= g_Parameters.volume.hierarchy.x || id.x >= g_Levels[level].nodes.z) return;
    VDBNode node = mutableNode(level, id.x);
    if (((node.metadata >> 8) & 255u) == 0) return;
    uint parentIndex;
    if (!findAncestor(float3(node.position), level + 1, parentIndex)) return;
    VDBNode parent = mutableNode(level + 1, parentIndex);
    uint list;
    if (!childListAddress(parent.childList, list)) return;
    LevelInfo info = g_Levels[level + 1];
    int3 cell = int3(float3(node.position - parent.position) / info.childExtent.xyz);
    if (any(cell < 0) || any(cell >= int3(info.children.w))) return;
    g_Children.Store2(list + flatten3(uint3(cell), uint3(info.children.w)) * 8, nodeReference(level, id.x));
    g_Nodes.Store2(g_Levels[level].nodes.x + id.x * g_Levels[level].nodes.y + 40, nodeReference(level + 1, parentIndex));
}
