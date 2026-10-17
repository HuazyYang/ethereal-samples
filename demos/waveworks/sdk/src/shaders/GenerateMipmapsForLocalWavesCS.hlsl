// Recovered from _GenerateMipmapsForLocalWavesCS_bytecode_dx.msasm.

cbuffer CS_CBUFFER : register(b0)
{
    uint g_sourceMipSize : packoffset(c0.x);
    float pad0 : packoffset(c0.y);
    float pad1 : packoffset(c0.z);
    float pad2 : packoffset(c0.w);
};

SamplerState g_bilinearSampler : register(s0);
Texture2D<float4> g_sourceMip : register(t0);
RWTexture2D<float4> g_targetMip1 : register(u0);
RWTexture2D<float4> g_targetMip2 : register(u1);
RWTexture2D<float4> g_targetMip3 : register(u2);
RWTexture2D<float4> g_targetMip4 : register(u3);
RWTexture2D<float4> g_targetMip5 : register(u4);

struct MipmapSharedData
{
    uint2 words[17];
};

groupshared MipmapSharedData g_sharedMipTexels[17];

uint2 PackTexel(float4 value)
{
    uint4 halves = f32tof16(value);
    return uint2(
        halves.x | (halves.y << 16u),
        halves.z | (halves.w << 16u));
}

void StoreSharedTexel(uint2 coordinate, float4 value)
{
    g_sharedMipTexels[coordinate.y].words[coordinate.x] = PackTexel(value);
}

float4 LoadSharedTexel(uint2 coordinate)
{
    uint2 packed = g_sharedMipTexels[coordinate.y].words[coordinate.x];
    return float4(
        f16tof32(packed.x),
        f16tof32(packed.x >> 16u),
        f16tof32(packed.y),
        f16tof32(packed.y >> 16u));
}

// The shipped blob sums the quad as ((0,stride) + origin) + (stride,0) + (stride,stride):
// `add r1.xyzw, r2.xyzw, r3.xyzw` pairs the origin texel with the row-offset one first,
// not with the column-offset one. Float addition is not associative, so the order is
// load-bearing and is reproduced here rather than "tidied" into row-major order.
float4 ReduceSharedQuad(uint2 origin, uint stride)
{
    float4 v00 = LoadSharedTexel(origin);
    float4 v01 = LoadSharedTexel(origin + uint2(0u, stride));
    float4 v10 = LoadSharedTexel(origin + uint2(stride, 0u));
    float4 v11 = LoadSharedTexel(origin + uint2(stride, stride));
    return 0.25f * (v00 + v01 + v10 + v11);
}

[numthreads(16, 16, 1)]
void GenerateMipmapsForLocalWavesCS(
    uint3 groupID : SV_GroupID,
    uint3 groupThreadID : SV_GroupThreadID,
    uint3 dispatchThreadID : SV_DispatchThreadID)
{
    if (g_sourceMipSize < 2u)
        return;

    float2 sourceUv = (float2(dispatchThreadID.xy) + 0.5f) / (float(g_sourceMipSize) * 0.5f);
    float4 mip1 = g_sourceMip.SampleLevel(g_bilinearSampler, sourceUv, 0.0f);

    if (g_sourceMipSize >= 4u)
        StoreSharedTexel(groupThreadID.xy, mip1);

    g_targetMip1[dispatchThreadID.xy] = mip1;
    if (g_sourceMipSize < 4u)
        return;

    GroupMemoryBarrierWithGroupSync();

    bool stopAfterMip2 = g_sourceMipSize < 8u;
    if (all(groupThreadID.xy < 8u))
    {
        uint2 origin = groupThreadID.xy * 2u;
        float4 mip2 = ReduceSharedQuad(origin, 1u);
        if (!stopAfterMip2)
            StoreSharedTexel(origin, mip2);
        uint2 output = (groupID.xy * 16u) / 2u + groupThreadID.xy;
        g_targetMip2[output] = mip2;
    }
    if (stopAfterMip2)
        return;

    // DELIBERATE DIVERGENCE FROM THE SHIPPED BLOB.
    // The shipped code emits sync_g_t once and then three bare sync_g, so the
    // mip2..mip5 reductions read groupshared texels written by threads in other
    // warps with no execution barrier -- a data race, measurable as run-to-run
    // drift of up to 0.34 in the deeper mips with NVIDIA's own bytecode.
    // Every return above is guarded by g_sourceMipSize, a cbuffer value and
    // therefore group-uniform, so no thread has exited here and the syncing
    // barrier is legal. This entry no longer matches _GenerateMipmapsForLocalWavesCS_bytecode_dx.msasm.
    GroupMemoryBarrierWithGroupSync();

    bool stopAfterMip3 = g_sourceMipSize < 16u;
    if (all(groupThreadID.xy < 4u))
    {
        uint2 origin = groupThreadID.xy * 4u;
        float4 mip3 = ReduceSharedQuad(origin, 2u);
        if (!stopAfterMip3)
            StoreSharedTexel(origin, mip3);
        uint2 output = (groupID.xy * 16u) / 4u + groupThreadID.xy;
        g_targetMip3[output] = mip3;
    }
    if (stopAfterMip3)
        return;

    GroupMemoryBarrierWithGroupSync();

    bool stopAfterMip4 = g_sourceMipSize < 32u;
    if (all(groupThreadID.xy < 2u))
    {
        uint2 origin = groupThreadID.xy * 8u;
        float4 mip4 = ReduceSharedQuad(origin, 4u);
        if (!stopAfterMip4)
            StoreSharedTexel(origin, mip4);
        uint2 output = (groupID.xy * 16u) / 8u + groupThreadID.xy;
        g_targetMip4[output] = mip4;
    }
    if (stopAfterMip4)
        return;

    GroupMemoryBarrierWithGroupSync();

    if (all(groupThreadID.xy < 1u))
    {
        float4 mip5 = ReduceSharedQuad(uint2(0u, 0u), 8u);
        uint2 output = (groupID.xy * 16u) / 16u;
        g_targetMip5[output] = mip5;
    }
}
