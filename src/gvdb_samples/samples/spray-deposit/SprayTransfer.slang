// Sample-local transfer between scalar/raw atlas storage and reusable gather buffers.
// SPDX-License-Identifier: Apache-2.0
struct Parameters {
    uint4 counts;
};
ConstantBuffer<Parameters> g_Parameters : register(b0);
StructuredBuffer<float> g_SourceDensity : register(t0);
ByteAddressBuffer g_SourceRawColor : register(t1);
StructuredBuffer<uint> g_SourceColor : register(t2);
RWStructuredBuffer<float> g_Density : register(u0);
RWStructuredBuffer<uint> g_Color : register(u1);
RWByteAddressBuffer g_RawColor : register(u2);
[shader("compute")][numthreads(256, 1, 1)] void seedSprayGather(uint3 id
                                                                : SV_DispatchThreadID) {
    uint i = id.x + id.y * (65535u * 256u);
    if (i >= g_Parameters.counts.x)
        return;
    g_Density[i] = g_SourceDensity[i];
    g_Color[i] = g_SourceRawColor.Load(i * 4);
}[shader("compute")][numthreads(256, 1, 1)] void publishSprayGather(uint3 id
                                                                    : SV_DispatchThreadID) {
    uint i = id.x + id.y * (65535u * 256u);
    if (i >= g_Parameters.counts.x)
        return;
    g_Density[i] = g_SourceDensity[i];
    g_RawColor.Store(i * 4, g_SourceColor[i]);
}
