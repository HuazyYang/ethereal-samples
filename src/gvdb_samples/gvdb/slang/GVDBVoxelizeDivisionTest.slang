// SPDX-License-Identifier: Apache-2.0
#include "GVDBVoxelizeDivision.slang"
StructuredBuffer<uint2> pairs:register(t0);
RWStructuredBuffer<uint> results:register(u0);
[shader("compute")][numthreads(1,1,1)]
void testVoxelDivision(uint3 id:SV_DispatchThreadID){results[id.x]=voxelDivideBits(pairs[id.x].x,pairs[id.x].y);}
