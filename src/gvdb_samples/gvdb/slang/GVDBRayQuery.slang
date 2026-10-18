// Arbitrary sparse scalar rays, preserving GVDB surface wavefront sampling.
// SPDX-License-Identifier: Apache-2.0
#include "GVDBTypes.slang"
struct QueryParameters {
    VolumeInfo volume;
    uint4 counts;
    float4 sampling;
};
ConstantBuffer<QueryParameters> g_Parameters : register(b0);
StructuredBuffer<float> g_Atlas : register(t0);
ByteAddressBuffer g_Nodes : register(t1);
ByteAddressBuffer g_Children : register(t2);
StructuredBuffer<LevelInfo> g_Levels : register(t3);
StructuredBuffer<float4> g_Origins : register(t4);
StructuredBuffer<float4> g_Directions : register(t5);
RWStructuredBuffer<float4> g_Hits : register(u0);
RWStructuredBuffer<float4> g_Normals : register(u1);
#include "GVDBRayQueryTrace.slang"
[shader("compute")][numthreads(256, 1, 1)] void queryVolumeRays(uint3 id
                                                                : SV_DispatchThreadID) {
    uint index = id.x + id.y * (65535u * 256u);
    [branch] if (index >= g_Parameters.counts.x) return;
    TraceResult result = traceRay(g_Origins[index].xyz, g_Directions[index].xyz);
    g_Hits[index] = result.hit;
    g_Normals[index] = result.normal;
}