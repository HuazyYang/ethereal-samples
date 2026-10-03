#include "Page.hlsli"
#include "AllocationMapCB.hlsli"
RWTexture3D<uint> u_AllocationMap: register(u0);
groupshared uint s_Pages[10][10];
[numthreads(8, 8, 8)]
void main(in uint3 gfsdk_GroupIdx: SV_GroupID, in uint3 gfsdk_GroupThreadIdx: SV_GroupThreadID, in uint3 gfsdk_GlobalIdx: SV_DispatchThreadID)
{
    int3 groupBase = int3(gfsdk_GlobalIdx.xyz - gfsdk_GroupThreadIdx.xyz);
    float linearIdx = float(gfsdk_GroupThreadIdx.z) * 64 + float(gfsdk_GroupThreadIdx.y) * 8 + float(gfsdk_GroupThreadIdx.x);
    int3 reorderIdx;
    linearIdx = (linearIdx + 0.25) * 0.1;
    reorderIdx.x = int(frac(linearIdx) * 10);
    linearIdx = (floor(linearIdx) + 0.25) * 0.1;
    reorderIdx.y = int(frac(linearIdx) * 10);
    linearIdx = floor(linearIdx);
    reorderIdx.z = int(linearIdx);
    if (linearIdx < 100)
    {
        s_Pages[reorderIdx.y][reorderIdx.x] = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    if (linearIdx < 500)
    {
        [unroll]
            for (int zoffset = 0; zoffset < 10; zoffset += 5)
        {
            int3 groupAddr = reorderIdx + int3(0, 0, zoffset);
            uint allocationValue = u_AllocationMap[int3((groupBase + groupAddr - 1) & (g_TextureSize - 1))].x;
            if ((allocationValue & AMAP_EMISSIVE_BIT) != 0)
            {
                InterlockedOr(s_Pages[groupAddr.y][groupAddr.x], 1u << groupAddr.z);
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if ((s_Pages[gfsdk_GroupThreadIdx.y + 1][gfsdk_GroupThreadIdx.x + 1] & (2u << gfsdk_GroupThreadIdx.z)) != 0)
        return;
    uint accumulator = 0;
    int2 offset;
    for (offset.y = 0; offset.y <= 2; offset.y++)
        for (offset.x = 0; offset.x <= 2; offset.x++)
        {
            int2 address = int2(gfsdk_GroupThreadIdx.xy) + offset;
            accumulator |= s_Pages[address.y][address.x];
        }
    if (((accumulator >> gfsdk_GroupThreadIdx.z) & 7) != 0)
    {
        InterlockedOr(u_AllocationMap[int3(gfsdk_GlobalIdx)], AMAP_DILATED_EMITTANCE_BIT);
    }
}
