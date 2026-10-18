// NVIDIA GVDB point operations using explicit buffers. SPDX-License-Identifier: Apache-2.0
#include "GVDBTypes.slang"
struct ParticleParameters {
    VolumeInfo volume;
    uint4 counts; // particle count, leaf count, reserved, reserved
    float4 translationAndSupport; // translation.xyz, support-point distance
};
ConstantBuffer<ParticleParameters> g_Parameters : register(b0);
StructuredBuffer<float4> g_Positions : register(t0);
ByteAddressBuffer g_Nodes : register(t1);
ByteAddressBuffer g_Children : register(t2);
StructuredBuffer<LevelInfo> g_Levels : register(t3);
StructuredBuffer<uint> g_PointCells : register(t4);
StructuredBuffer<uint> g_PointOffsets : register(t5);
StructuredBuffer<uint> g_CellOffsets : register(t6);
StructuredBuffer<float4> g_Directions : register(t7);
RWStructuredBuffer<uint> g_CellCounts : register(u0);
RWStructuredBuffer<uint> g_OutputPointCells : register(u1);
RWStructuredBuffer<uint> g_OutputPointOffsets : register(u2);
RWStructuredBuffer<float4> g_OutputPositions : register(u3);

[shader("compute")]
[numthreads(256, 1, 1)]
void clearCellCounts(uint3 id : SV_DispatchThreadID) {
    if (id.x < g_Parameters.counts.y) g_CellCounts[id.x] = 0;
}
void insertPoint(uint index, float3 support) {
    g_OutputPointCells[index] = GVDB_INVALID;
    g_OutputPointOffsets[index] = 0;
    float3 p = g_Positions[index].xyz;
    [branch] if (p.z == 1.0e10 || !all(isfinite(p))) return;
    VDBNode leaf;
    uint leafIndex;
    bool found = findLeaf(g_Nodes, g_Children, g_Levels, g_Parameters.volume,
        p + g_Parameters.translationAndSupport.xyz + support, leaf, leafIndex);
    [branch] if (!found) return;
    uint offset;
    InterlockedAdd(g_CellCounts[leafIndex], 1, offset);
    g_OutputPointCells[index] = leafIndex;
    g_OutputPointOffsets[index] = offset;
}
[shader("compute")]
[numthreads(256, 1, 1)]
void insertPoints(uint3 id : SV_DispatchThreadID) {
    if (id.x < g_Parameters.counts.x) insertPoint(id.x, float3(0));
}
[shader("compute")]
[numthreads(256, 1, 1)]
void insertSupportPoints(uint3 id : SV_DispatchThreadID) {
    if (id.x < g_Parameters.counts.x)
        insertPoint(id.x, g_Directions[id.x].xyz * g_Parameters.translationAndSupport.w);
}
// Dispatch after a global exclusive scan of g_CellCounts. Each insertion owns
// exactly one (cell,offset), so scattering needs neither atomics nor barriers.
[shader("compute")]
[numthreads(256, 1, 1)]
void scatterPoints(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_Parameters.counts.x) return;
    uint cell = g_PointCells[id.x];
    if (cell >= g_Parameters.counts.y) return;
    uint destination = g_CellOffsets[cell] + g_PointOffsets[id.x];
    float4 point = g_Positions[id.x];
    point.xyz += g_Parameters.translationAndSupport.xyz;
    g_OutputPositions[destination] = point;
}
[shader("compute")]
[numthreads(256, 1, 1)]
void transformPoints(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_Parameters.counts.x) return;
    float4 point = g_Positions[id.x];
    point.xyz = point.xyz * g_Parameters.translationAndSupport.w + g_Parameters.translationAndSupport.xyz;
    g_OutputPositions[id.x] = point;
}
[shader("compute")]
[numthreads(256, 1, 1)]
void markLinkedLeaves(uint3 id : SV_DispatchThreadID) {
    LevelInfo level = g_Levels[0];
    if (id.x >= level.nodes.z) return;
    VDBNode node = loadNode(g_Nodes, level.nodes.x + id.x * level.nodes.y);
    g_OutputPointCells[id.x] = nullReference(node.parent) ? 0 : 1;
}
