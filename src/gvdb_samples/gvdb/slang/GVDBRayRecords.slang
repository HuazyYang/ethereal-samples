// SPDX-License-Identifier: Apache-2.0
#include "GVDBTypes.slang"
struct Parameters {
    uint4 atlasResolutionAndApron, atlasBrickCountAndWidth, hierarchy;
    float4 boundsMin, boundsMax;
    uint4 counts;
    float4 sampling;
};
ConstantBuffer<Parameters> configuration : register(b0);
ByteAddressBuffer g_Atlas : register(t0);
ByteAddressBuffer g_Nodes : register(t1);
ByteAddressBuffer g_Children : register(t2);
StructuredBuffer<LevelInfo> g_Levels : register(t3);
ByteAddressBuffer records : register(t4);
RWByteAddressBuffer outputRecords : register(u0);
RWStructuredBuffer<float4> hits : register(u1);
RWStructuredBuffer<float4> normals : register(u2);
struct QueryParameters { VolumeInfo volume; uint4 counts; float4 sampling; };
QueryParameters queryParameters() {
    QueryParameters p;
    p.volume.atlasResolutionAndApron = configuration.atlasResolutionAndApron;
    p.volume.atlasBrickCountAndWidth = configuration.atlasBrickCountAndWidth;
    p.volume.hierarchy = configuration.hierarchy;
    p.volume.boundsMin = configuration.boundsMin;
    p.volume.boundsMax = configuration.boundsMax;
    p.counts = configuration.counts;
    p.sampling = configuration.sampling;
    return p;
}
#define g_Parameters queryParameters()
#define GVDB_RAY_QUERY_RAW 1
#include "GVDBRayQueryTrace.slang"
[shader("compute")][numthreads(256,1,1)]
void queryRayRecords(uint3 id : SV_DispatchThreadID) {
    uint i = id.x + id.y * (65535u * 256u);
    if (i >= configuration.counts.x) return;
    uint offset = i*64;
    TraceResult result = (TraceResult)0;
    result.hit = float4(1e10,1e10,1e10,0);
    if (configuration.counts.w != 0)
        result = traceRay(asfloat(records.Load3(offset+24)),asfloat(records.Load3(offset+36)));
    // Copy uninterpreted words so NaN payloads and caller metadata survive exactly.
    [unroll] for (uint word=0; word<16; ++word)
        outputRecords.Store(offset+word*4,records.Load(offset+word*4));
    outputRecords.Store3(offset,asuint(result.hit.xyz));
    if (result.hit.w > 0) outputRecords.Store3(offset+12,asuint(result.normal.xyz));
    hits[i] = result.hit;
    normals[i] = result.normal;
}
