// CPU-readback statistics are separate from the original GPU-average module.
// Slang reflects unused globals, so keeping u1 here preserves the original
// downsampleRegion entry's b0/t0/u0 binding contract.
// SPDX-License-Identifier: Apache-2.0
#include "GVDBResample.slang"
RWStructuredBuffer<uint> g_SampleCounts:register(u1);
// Host topology classification reads back these statistics and performs the
// final division on the CPU. Approximate GPU division can move an average
// exactly on a strict activation threshold upward by one float ULP.
[shader("compute")][numthreads(8,8,8)]
void downsampleRegionStats(uint3 id:SV_DispatchThreadID) {
    uint3 destination=g_Parameters.volume.atlasResolutionAndApron.xyz;
    if(any(id>=destination))return;
    RegionStatistics statistics=regionStatistics(id);
    uint index=flatten3(id,destination);
    g_Destination[index]=statistics.sum;
    g_SampleCounts[index]=statistics.count;
}
