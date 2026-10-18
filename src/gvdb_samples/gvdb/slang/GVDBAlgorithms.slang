// Portable replacement for CUDA/Thrust scan, sort and reduction.
// SPDX-License-Identifier: Apache-2.0
#include "GVDBTypes.slang"
struct AlgorithmParameters { uint count; uint bit; uint reserved0; uint reserved1; };
ConstantBuffer<AlgorithmParameters> g_Parameters : register(b0);
StructuredBuffer<KeyValue> g_Keys : register(t0);
StructuredBuffer<uint> g_Input : register(t1);
StructuredBuffer<uint> g_BlockOffsets : register(t2);
StructuredBuffer<float> g_Values : register(t3);
StructuredBuffer<Reduction> g_Reductions : register(t4);
RWStructuredBuffer<uint> g_Output : register(u0);
RWStructuredBuffer<uint> g_BlockSums : register(u1);
RWStructuredBuffer<KeyValue> g_OutputKeys : register(u2);
RWStructuredBuffer<Reduction> g_OutputReductions : register(u3);

groupshared uint s_Scan[256];
groupshared float s_Minimum[256];
groupshared float s_Maximum[256];
groupshared float s_Sum[256];
groupshared uint s_Count[256];

uint inclusiveScan(uint value, uint lane) {
    s_Scan[lane] = value;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint offset = 1; offset < 256; offset <<= 1) {
        uint addend = lane >= offset ? s_Scan[lane - offset] : 0;
        GroupMemoryBarrierWithGroupSync();
        s_Scan[lane] += addend;
        GroupMemoryBarrierWithGroupSync();
    }
    return s_Scan[lane];
}
// Host: recursively scan block sums, then add the resulting block offsets.
// Never early-return before a group barrier: the last group is zero-padded.
[shader("compute")]
[numthreads(256, 1, 1)]
void scanBlocks(uint3 id : SV_DispatchThreadID, uint lane : SV_GroupIndex, uint3 group : SV_GroupID) {
    uint value = id.x < g_Parameters.count ? g_Input[id.x] : 0;
    uint inclusive = inclusiveScan(value, lane);
    if (id.x < g_Parameters.count) g_Output[id.x] = inclusive - value;
    if (lane == 255) g_BlockSums[group.x] = inclusive;
}
[shader("compute")]
[numthreads(256, 1, 1)]
void addScanBlockOffsets(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID) {
    if (id.x < g_Parameters.count) g_Output[id.x] += g_BlockOffsets[group.x];
}
uint radixZero(uint2 key) {
    uint word = g_Parameters.bit < 32 ? key.x : key.y;
    return 1u - ((word >> (g_Parameters.bit & 31)) & 1u);
}
// Stable LSD binary radix pass. The host scans these flags globally, dispatches
// scatter, swaps input/output, and repeats bits 0..63. No 64-bit arithmetic,
// wave operations or float atomics are required on any target.
[shader("compute")]
[numthreads(256, 1, 1)]
void radixFlags(uint3 id : SV_DispatchThreadID) {
    if (id.x < g_Parameters.count) g_Output[id.x] = radixZero(g_Keys[id.x].key);
}
[shader("compute")]
[numthreads(256, 1, 1)]
void radixScatter(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_Parameters.count) return;
    KeyValue value = g_Keys[id.x];
    uint zerosBefore = g_Input[id.x];
    uint last = g_Parameters.count - 1;
    uint totalZeros = g_Input[last] + radixZero(g_Keys[last].key);
    uint destination = radixZero(value.key) != 0 ? zerosBefore : totalZeros + id.x - zerosBefore;
    g_OutputKeys[destination] = value;
}
Reduction emptyReduction() {
    Reduction result;
    result.minimum = asfloat(0x7f800000u);
    result.maximum = -asfloat(0x7f800000u);
    result.sum = 0;
    result.count = 0;
    return result;
}
void reduceGroup(Reduction input, uint lane, uint group) {
    s_Minimum[lane] = input.minimum;
    s_Maximum[lane] = input.maximum;
    s_Sum[lane] = input.sum;
    s_Count[lane] = input.count;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint stride = 128; stride != 0; stride >>= 1) {
        if (lane < stride) {
            s_Minimum[lane] = min(s_Minimum[lane], s_Minimum[lane + stride]);
            s_Maximum[lane] = max(s_Maximum[lane], s_Maximum[lane + stride]);
            s_Sum[lane] += s_Sum[lane + stride];
            s_Count[lane] += s_Count[lane + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (lane == 0) {
        Reduction result;
        result.minimum = s_Minimum[0];
        result.maximum = s_Maximum[0];
        result.sum = s_Sum[0];
        result.count = s_Count[0];
        g_OutputReductions[group] = result;
    }
}
// NaNs are omitted. Empty inputs produce {+inf,-inf,0,0}; the host supplies
// that identity without dispatching zero groups. Infinities retain IEEE behavior.
[shader("compute")]
[numthreads(256, 1, 1)]
void reduceFloatBlocks(uint3 id : SV_DispatchThreadID, uint lane : SV_GroupIndex, uint3 group : SV_GroupID) {
    Reduction value = emptyReduction();
    if (id.x < g_Parameters.count) {
        float scalar = g_Values[id.x];
        // Integer classification survives the legacy FXC finite-math optimization.
        if ((asuint(scalar) & 0x7fffffffu) <= 0x7f800000u) {
            value.minimum = scalar;
            value.maximum = scalar;
            value.sum = scalar;
            value.count = 1;
        }
    }
    reduceGroup(value, lane, group.x);
}
[shader("compute")]
[numthreads(256, 1, 1)]
void reducePartialBlocks(uint3 id : SV_DispatchThreadID, uint lane : SV_GroupIndex, uint3 group : SV_GroupID) {
    Reduction value = emptyReduction();
    if (id.x < g_Parameters.count) value = g_Reductions[id.x];
    reduceGroup(value, lane, group.x);
}
