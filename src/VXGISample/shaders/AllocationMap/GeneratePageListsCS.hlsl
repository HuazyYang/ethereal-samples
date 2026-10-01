#include "Page.hlsli"
#include "AllocationMapCB.hlsli"
Texture3D<uint> t_AllocationMap : register(t0);
RWBuffer<uint> u_OpacityToDownsample : register(u2);
RWBuffer<uint> u_OpacityToVoxelize : register(u3);
RWBuffer<uint> u_EmittanceToDownsample : register(u4);
RWBuffer<uint> u_EmissiveToVoxelize : register(u5);
RWBuffer<uint> u_IrradianceToTrace : register(u6);
groupshared uint s_FirstSlot;
groupshared uint s_NumThreads;
uint AppendPageToIrradianceListCoherent(MultiListParams params, PageData data, bool predicate, bool firstThread)
{
    if (firstThread)
    {
        s_FirstSlot = 0;
        s_NumThreads = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    uint threadOffset;
    InterlockedAdd(s_NumThreads, uint(predicate), threadOffset);
    GroupMemoryBarrierWithGroupSync();
    if (s_NumThreads == 0)
        return 0;
    if (firstThread)
    {
        uint firstSlot;
        InterlockedAdd(u_IrradianceToTrace[params.segmentOffset + MULTILIST_COUNTER_OFFSET], s_NumThreads, firstSlot);
        s_FirstSlot = firstSlot;
    }
    GroupMemoryBarrierWithGroupSync();
    if (predicate)
    {
        uint slot = s_FirstSlot + threadOffset;
        if (slot < params.maxElements)
            u_IrradianceToTrace[slot + params.segmentOffset + MULTILIST_DATA_OFFSET] = data.data;
        return slot;
    }
    return 0;
}
[numthreads(8, 8, 4)]
void main(in uint3 gfsdk_GroupIdx: SV_GroupID, in uint3 gfsdk_GroupThreadIdx: SV_GroupThreadID, in uint3 gfsdk_GlobalIdx: SV_DispatchThreadID)
{
    bool firstThread = all(gfsdk_GroupThreadIdx.xyz == uint3scalar(0));
    int3 pageAddr = TOROIDAL_ADDRESS(int3(gfsdk_GlobalIdx), g_ToroidalOffset.xyz, int3scalar(int(g_TextureSize)));
    uint pageLevel = GetMostDetailedLevelWithPage(gfsdk_GlobalIdx.xyz, g_TextureSize, g_MaxClipLevel);
    uint value = t_AllocationMap[pageAddr].x;
    PageData pageData = PackPageCoordinates(pageLevel, gfsdk_GlobalIdx);
    bool present = (value & AMAP_PRESENT_BIT) != 0;
    bool dirty = (value & AMAP_GEOMETRY_DIRTY_BIT) != 0;
    if (present)
    {
        if (dirty || bool(g_OpacityDownsampleAllPresentPages))
        {
            uint slot;
            for (uint level = pageLevel + 1; level < g_NumPagedLevels; level++)
            {
                AppendPageToMultiList(u_OpacityToDownsample, g_FilteredPageListParams[level], pageData, slot);
            }
        }
        if (dirty)
        {
            uint slot;
            if (bool(g_OpacityVoxelizeAllLevels))
                for (uint level = pageLevel; level <= g_MaxClipLevel; level++)
                {
                    AppendPageToMultiList(u_OpacityToVoxelize, g_FilteredPageListParams[level], pageData, slot);
                }
            else
            {
                AppendPageToMultiList(u_OpacityToVoxelize, g_FilteredPageListParams[pageLevel], pageData, slot);
            }
        }
    }
    if (g_WriteIrradianceMapPages != 0)
    {
        AppendPageToIrradianceListCoherent(g_IrradianceMapPageListParams, pageData, present, firstThread);
    }
    if ((value & AMAP_LIGHTING_DIRTY_BIT) != 0)
    {
        bool emissive = (value & AMAP_EMISSIVE_BIT) != 0;
        bool dilated = (value & AMAP_DILATED_EMITTANCE_BIT) != 0;
        if (emissive || dilated)
        {
            uint slot;
            for (uint level = pageLevel + 1; level < g_NumPagedLevels; level++)
            {
                AppendPageToMultiList(u_EmittanceToDownsample, g_FilteredPageListParams[level], pageData, slot);
            }
        }
        if (emissive && !bool(g_StoreEmittanceInFP16))
        {
            uint slot;
            if (g_EmittanceVoxelizeAllLevels != 0)
            {
                for (uint level = pageLevel; level <= g_MaxClipLevel; level++)
                {
                    AppendPageToMultiList(u_EmissiveToVoxelize, g_FilteredPageListParams[level], pageData, slot);
                }
            }
            else
            {
                AppendPageToMultiList(u_EmissiveToVoxelize, g_FilteredPageListParams[pageLevel], pageData, slot);
            }
        }
    }
}