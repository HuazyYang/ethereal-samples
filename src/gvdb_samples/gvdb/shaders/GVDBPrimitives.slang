// Device-wide primitives of GVDB (the reference used Thrust for the sort and the min/max):
// - prefixSum / prefixFixup   - exclusive prefix sum of uints (cuda_gvdb_particles.cuh)
// - gvdbRadixCount / gvdbRadixScatter
//                             - one pass of an LSD radix sort of 64-bit keys (8 bits per pass)
// - gvdbMinMaxPartial / gvdbMinMaxFinal
//                             - min and max of floats
// - gvdbCopyBlock*            - a tightly packed block of voxels from a buffer into an atlas (per format)
#include "GVDB.slangh"

//------- Prefix sum (reference PrefixSum: two elements per thread, 512 threads per group)

#define SCAN_BLOCKSIZE 512

groupshared uint scanArray[SCAN_BLOCKSIZE << 1];

[shader("compute")]
[numthreads(SCAN_BLOCKSIZE, 1, 1)]
void prefixFixup(uniform uint* input, uniform uint* aux, uniform int len, uint3 group : SV_GroupID,
                 uint3 thread : SV_GroupThreadID)
{
    uint t = thread.x;
    uint start = t + 2 * group.x * SCAN_BLOCKSIZE;
    if (start < uint(len)) input[start] += aux[group.x];
    if (start + SCAN_BLOCKSIZE < uint(len)) input[start + SCAN_BLOCKSIZE] += aux[group.x];
}

[shader("compute")]
[numthreads(SCAN_BLOCKSIZE, 1, 1)]
void prefixSum(uniform uint* input, uniform uint* output, uniform uint* aux, uniform int len, uniform int zeroff,
               uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    uint t1 = thread.x + 2 * group.x * SCAN_BLOCKSIZE;
    uint t2 = t1 + SCAN_BLOCKSIZE;

    // Pre-load into shared memory
    scanArray[thread.x] = (t1 < uint(len)) ? input[t1] : 0;
    scanArray[thread.x + SCAN_BLOCKSIZE] = (t2 < uint(len)) ? input[t2] : 0;
    GroupMemoryBarrierWithGroupSync();

    // Reduction
    int stride;
    for (stride = 1; stride <= SCAN_BLOCKSIZE; stride <<= 1) {
        int index = int(thread.x + 1) * stride * 2 - 1;
        if (index < 2 * SCAN_BLOCKSIZE)
            scanArray[index] += scanArray[index - stride];
        GroupMemoryBarrierWithGroupSync();
    }

    // Post reduction
    for (stride = SCAN_BLOCKSIZE >> 1; stride > 0; stride >>= 1) {
        int index = int(thread.x + 1) * stride * 2 - 1;
        if (index + stride < 2 * SCAN_BLOCKSIZE)
            scanArray[index + stride] += scanArray[index];
        GroupMemoryBarrierWithGroupSync();
    }
    GroupMemoryBarrierWithGroupSync();

    // Output values & aux
    if (t1 + uint(zeroff) < uint(len)) output[t1 + uint(zeroff)] = scanArray[thread.x];
    if (t2 + uint(zeroff) < uint(len))
        output[t2 + uint(zeroff)] = (thread.x == SCAN_BLOCKSIZE - 1 && zeroff != 0) ? 0 : scanArray[thread.x + SCAN_BLOCKSIZE];
    if (thread.x == 0) {
        if (zeroff != 0) output[0] = 0;
        if (aux != nullptr) aux[group.x] = scanArray[2 * SCAN_BLOCKSIZE - 1];
    }
}

//------- Radix sort of 64-bit keys
//
// One pass sorts by the 8-bit digit at `shift` (a stable counting sort), in tiles of RADIX_TILE keys, one key per
// thread:
//   gvdbRadixCount    counts the digits of every tile into counts[digit * numTiles + tile]
//   (host)            exclusive prefix sum of counts: where the keys of a digit and tile go
//   gvdbRadixScatter  writes every key to its place; keys of one digit keep their order (rank within the warp from
//                     matchDigit, then the counts of the warps before)

#define RADIX_TILE 256
#define RADIX_WARPS (RADIX_TILE / 32)

groupshared uint radixCounts[256];
groupshared uint warpDigitCounts[RADIX_WARPS][256];

uint keyDigit(uint64_t key, uint shift) { return uint((key >> shift) & 0xFF); }

// The lanes of the warp whose 9-bit digit equals this lane's, from one ballot per bit. (Slang's WaveMatch is
// __match_all_sync on CUDA, which matches only when every lane has the same value.)
uint matchDigit(uint digit)
{
    uint mask = 0xFFFFFFFFu;
    for (uint bit = 0; bit < 9; ++bit) {
        const uint ballot = WaveActiveBallot(((digit >> bit) & 1) != 0).x;
        mask &= ((digit >> bit) & 1) != 0 ? ballot : ~ballot;
    }
    return mask;
}

[shader("compute")]
[numthreads(RADIX_TILE, 1, 1)]
void gvdbRadixCount(uniform uint64_t* keys, uniform uint count, uniform uint shift, uniform uint* counts,
                    uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    const uint numTiles = (count + RADIX_TILE - 1) / RADIX_TILE;
    radixCounts[thread.x] = 0;
    GroupMemoryBarrierWithGroupSync();

    const uint i = group.x * RADIX_TILE + thread.x;
    if (i < count) InterlockedAdd(radixCounts[keyDigit(keys[i], shift)], 1u);
    GroupMemoryBarrierWithGroupSync();

    counts[thread.x * numTiles + group.x] = radixCounts[thread.x];
}

[shader("compute")]
[numthreads(RADIX_TILE, 1, 1)]
void gvdbRadixScatter(uniform uint64_t* keys, uniform uint count, uniform uint shift, uniform uint* offsets,
                      uniform uint64_t* sorted, uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    const uint numTiles = (count + RADIX_TILE - 1) / RADIX_TILE;
    const uint warp = thread.x / 32;
    const uint lane = thread.x % 32;
    for (uint w = 0; w < RADIX_WARPS; ++w) warpDigitCounts[w][thread.x] = 0;
    GroupMemoryBarrierWithGroupSync();

    const uint i = group.x * RADIX_TILE + thread.x;
    const bool valid = i < count;
    const uint64_t key = valid ? keys[i] : 0;
    // Keys past the end take part in the match with a digit no key has.
    const uint digit = valid ? keyDigit(key, shift) : 256;

    // The lanes of this warp with the same digit; the rank of this key among them.
    const uint match = matchDigit(digit);
    const uint rankInWarp = countbits(match & ((1u << lane) - 1u));
    if (valid && rankInWarp == 0) warpDigitCounts[warp][digit] = countbits(match);
    GroupMemoryBarrierWithGroupSync();

    if (!valid) return;
    uint rank = rankInWarp;
    for (uint w = 0; w < warp; ++w) rank += warpDigitCounts[w][digit];
    sorted[offsets[digit * numTiles + group.x] + rank] = key;
}

//------- Min / max of floats

#define MINMAX_GROUP 256

groupshared float2 minMaxShared[MINMAX_GROUP];

void reduceMinMax(uint thread)
{
    for (uint stride = MINMAX_GROUP / 2; stride > 0; stride >>= 1) {
        if (thread < stride) {
            const float2 a = minMaxShared[thread], b = minMaxShared[thread + stride];
            minMaxShared[thread] = float2(min(a.x, b.x), max(a.y, b.y));
        }
        GroupMemoryBarrierWithGroupSync();
    }
}

// partial[group] = (min, max) of the values group, group + numGroups, ... (in steps of the whole grid).
[shader("compute")]
[numthreads(MINMAX_GROUP, 1, 1)]
void gvdbMinMaxPartial(uniform float* values, uniform uint count, uniform uint numGroups, uniform float2* partial,
                       uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    float2 mm = float2(3.402823466e+38f, -3.402823466e+38f);
    for (uint i = group.x * MINMAX_GROUP + thread.x; i < count; i += numGroups * MINMAX_GROUP) {
        const float v = values[i];
        mm = float2(min(mm.x, v), max(mm.y, v));
    }
    minMaxShared[thread.x] = mm;
    GroupMemoryBarrierWithGroupSync();
    reduceMinMax(thread.x);
    if (thread.x == 0) partial[group.x] = minMaxShared[0];
}

// result[0] = (min, max) of the `count` partial results (one group).
[shader("compute")]
[numthreads(MINMAX_GROUP, 1, 1)]
void gvdbMinMaxFinal(uniform float2* partial, uniform uint count, uniform float2* result, uint3 thread : SV_GroupThreadID)
{
    float2 mm = float2(3.402823466e+38f, -3.402823466e+38f);
    for (uint i = thread.x; i < count; i += MINMAX_GROUP) {
        const float2 p = partial[i];
        mm = float2(min(mm.x, p.x), max(mm.y, p.y));
    }
    minMaxShared[thread.x] = mm;
    GroupMemoryBarrierWithGroupSync();
    reduceMinMax(thread.x);
    if (thread.x == 0) result[0] = minMaxShared[0];
}

//------- Atlas block copies: dst[offset + t] = src[t] for t in [0, size)

void copyBlock<T : ITexelElement>(T* src, uint3 size, int3 offset, RWTexture3D<T>.Handle dst, uint3 t)
{
    if (t.x >= size.x || t.y >= size.y || t.z >= size.z) return;
    dst[uint3(int3(t) + offset)] = src[(t.z * size.y + t.y) * size.x + t.x];
}

[shader("compute")]
[numthreads(8, 8, 8)]
void gvdbCopyBlockF(uniform float* src, uniform uint3 size, uniform int3 offset, uniform RWTexture3D<float>.Handle dst,
                    uint3 t : SV_DispatchThreadID)
{
    copyBlock<float>(src, size, offset, dst, t);
}

[shader("compute")]
[numthreads(8, 8, 8)]
void gvdbCopyBlockF4(uniform float4* src, uniform uint3 size, uniform int3 offset, uniform RWTexture3D<float4>.Handle dst,
                     uint3 t : SV_DispatchThreadID)
{
    copyBlock<float4>(src, size, offset, dst, t);
}

[shader("compute")]
[numthreads(8, 8, 8)]
void gvdbCopyBlockC(uniform uint8_t* src, uniform uint3 size, uniform int3 offset, uniform RWTexture3D<uint8_t>.Handle dst,
                    uint3 t : SV_DispatchThreadID)
{
    copyBlock<uint8_t>(src, size, offset, dst, t);
}

[shader("compute")]
[numthreads(8, 8, 8)]
void gvdbCopyBlockC4(uniform uint8_t4* src, uniform uint3 size, uniform int3 offset, uniform RWTexture3D<uint8_t4>.Handle dst,
                     uint3 t : SV_DispatchThreadID)
{
    copyBlock<uint8_t4>(src, size, offset, dst, t);
}
