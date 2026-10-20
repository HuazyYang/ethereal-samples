// SPDX-License-Identifier: Apache-2.0
struct Parameters { uint4 work; };
ConstantBuffer<Parameters> parameters : register(b0);
StructuredBuffer<uint> source : register(t0);
StructuredBuffer<uint> destination : register(t1);
RWStructuredBuffer<uint> output : register(u0);
[shader("compute")][numthreads(256,1,1)]
void compositeOver(uint3 id : SV_DispatchThreadID) {
    uint i=id.x+id.y*(65535u*256u);
    if (i>=parameters.work.x) return;
    uint s=source[i],d=destination[i];
    // Exact rational byte arithmetic makes truncation deterministic on every API.
    // CUDA's approximate division/FMA can differ by one at integer boundaries.
    uint inverseAlpha=255-(s>>24);
    uint result=0;
    [unroll] for (uint c=0;c<4;++c) {
        uint numerator=((s>>(c*8))&255)*255+((d>>(c*8))&255)*inverseAlpha;
        result|=min(255u,numerator/255u)<<(c*8);
    }
    output[i]=result;
}
