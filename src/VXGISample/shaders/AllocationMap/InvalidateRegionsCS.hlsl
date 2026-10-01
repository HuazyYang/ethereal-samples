#include "Page.hlsli"
#include "AllocationMapCB.hlsli"
struct Frustum
{
    float4 planes[6];
};
#define USE_SHARED_MEMORY 0
RWTexture3D<uint> u_AllocationMap : register(u0);
RWBuffer<uint> u_OpacityToClear : register(u7);
RWBuffer<uint> u_OpacityToRestore : register(u8);
RWBuffer<uint> u_EmittanceToClear : register(u9);
#if PERSISTENT_VOXEL_DATA == 1
StructuredBuffer<VxgiBox4i> t_InvalidateRegions: register(t2);
#elif PERSISTENT_VOXEL_DATA == 2
Texture3D<uint> t_InvalidateBitmap: register(t1);
#endif
#if PERSISTENT_VOXEL_DATA != 0
StructuredBuffer<Frustum> t_InvalidateLightFrusta: register(t3);
#endif
#define INVALIDATE_GROUP_SIZE_X 4
#define INVALIDATE_GROUP_SIZE_Y 4
#define INVALIDATE_GROUP_SIZE_Z 4
#define INVALIDATE_GROUP_SIZE (INVALIDATE_GROUP_SIZE_X * INVALIDATE_GROUP_SIZE_Y * INVALIDATE_GROUP_SIZE_Z)
#if USE_SHARED_MEMORY
groupshared int s_IntersectedBoxCount;
groupshared int4 s_BoxesLower[INVALIDATE_GROUP_SIZE];
groupshared int4 s_BoxesUpper[INVALIDATE_GROUP_SIZE];
#endif
[numthreads(INVALIDATE_GROUP_SIZE_X, INVALIDATE_GROUP_SIZE_Y, INVALIDATE_GROUP_SIZE_Z)]
void main(in uint3 gfsdk_GroupIdx: SV_GroupID, in uint3 gfsdk_GroupThreadIdx: SV_GroupThreadID, in uint3 gfsdk_GlobalIdx: SV_DispatchThreadID)
{
    int3 groupSize = int3(INVALIDATE_GROUP_SIZE_X, INVALIDATE_GROUP_SIZE_Y, INVALIDATE_GROUP_SIZE_Z);
    uint linearIdx = gfsdk_GroupThreadIdx.x + (gfsdk_GroupThreadIdx.y + (gfsdk_GroupThreadIdx.z * groupSize.y)) * groupSize.x;
    int3 groupLower = int3(gfsdk_GroupIdx) * groupSize;
    int3 groupUpper = groupLower + groupSize - 1;
    bool geometryDirty = false;
#if PERSISTENT_VOXEL_DATA == 1
#if USE_SHARED_MEMORY
    for (uint offset = 0; offset < g_NumberOfRegions; offset += INVALIDATE_GROUP_SIZE)
    {
        if (linearIdx == 0)
            s_IntersectedBoxCount = 0;
        GroupMemoryBarrierWithGroupSync();
        VxgiBox4i box = t_InvalidateRegions[offset + linearIdx];
        if (all(gfsdk_LessThanEqual(box.lower.xyz, groupUpper)) && all(gfsdk_LessThanEqual(groupLower, box.upper.xyz)))
        {
            int index;
            gfsdk_AtomicAdd(s_IntersectedBoxCount, 1, index);
            s_BoxesLower[index] = box.lower;
            s_BoxesUpper[index] = box.upper;
        }
        GroupMemoryBarrierWithGroupSync();
        for (int i = 0; i < s_IntersectedBoxCount; i++)
        {
            VxgiBox4i region;
            region.lower = s_BoxesLower[i];
            region.upper = s_BoxesUpper[i];
            if (VxgiPartOfRegion(int3(gfsdk_GlobalIdx), region))
            {
                geometryDirty = true;
                break;
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }
#else
    for (uint i = 0; i < g_NumberOfRegions; i++)
    {
        VxgiBox4i region = t_InvalidateRegions[i];
        if (VxgiPartOfRegion(int3(gfsdk_GlobalIdx), region))
        {
            geometryDirty = true;
            break;
        }
    }
#endif
#elif PERSISTENT_VOXEL_DATA == 2
    uint bitmap = t_InvalidateBitmap[int3(gfsdk_GlobalIdx.xy, gfsdk_GlobalIdx.z >> 5)].x;
    geometryDirty = (bitmap & (1u << (gfsdk_GlobalIdx.z & 31))) != 0;
#else
    geometryDirty = true;
#endif
    bool lightingDirty = false;
#if PERSISTENT_VOXEL_DATA != 0
    if (!geometryDirty)
    {
        float3 samplingPoint = (float3(gfsdk_GlobalIdx.xyz) + float3(0.5, 0.5, 0.5)) * g_ClipmapOrigin.w + g_ClipmapOrigin.xyz;
        for (uint frustum = 0; frustum < g_NumberOfFrusta; frustum++)
        {
            bool insideFrustum = true;
            [unroll]
                for (uint plane = 0; plane < 6; plane++)
            {
                float4 planeEquation = t_InvalidateLightFrusta[frustum].planes[plane];
                float distance = dot(planeEquation.xyz, samplingPoint.xyz);
                if (distance > planeEquation.w)
                {
                    insideFrustum = false;
                    break;
                }
            }
            if (insideFrustum)
            {
                lightingDirty = true;
                break;
            }
        }
    }
#endif
    int3 pageAddr = TOROIDAL_ADDRESS(int3(gfsdk_GlobalIdx), g_ToroidalOffset.xyz, int3scalar(int(g_TextureSize)));
    uint newFinestLevel = GetMostDetailedLevelWithPage(gfsdk_GlobalIdx.xyz, g_TextureSize, g_MaxClipLevel);
    PageData pageData = PackPageCoordinates(newFinestLevel, gfsdk_GlobalIdx);
    uint value = u_AllocationMap[pageAddr].x;
    int3 oldGlobalIdx = int3(gfsdk_GlobalIdx.xyz) + g_ToroidalOffset.xyz - g_ToroidalOffsetPrevious.xyz;
    uint oldFinestLevel = GetMostDetailedLevelWithPage(oldGlobalIdx.xyz, g_TextureSize, g_MaxClipLevel);
    PageData oldPageData = PackPageCoordinates(oldFinestLevel, oldGlobalIdx);
    bool oldDataInvalid = newFinestLevel != oldFinestLevel;
    bool oldGlobalIdxWithinClipmap = oldFinestLevel <= g_MaxClipLevel;
    bool wasPresent = (value & AMAP_PRESENT_BIT) != 0;
    bool wasIlluminated = (value & (AMAP_EMISSIVE_BIT | AMAP_DILATED_EMITTANCE_BIT)) != 0;
    if (geometryDirty)
    {
        if (wasPresent || oldDataInvalid)
        {
            uint slot;
            for (uint level = newFinestLevel; level < g_NumPagedLevels; level++)
            {
                AppendPageToMultiList(u_OpacityToClear, g_FilteredPageListParams[level], pageData, slot);
            }
        }
        value = AMAP_GEOMETRY_DIRTY_BIT;
    }
    else
    {
        value &= (AMAP_PRESENT_BIT | AMAP_EMISSIVE_BIT);
        if (wasPresent && oldDataInvalid && oldGlobalIdxWithinClipmap)
        {
            uint slot;
            for (uint level = oldFinestLevel + 1; level <= g_MaxClipLevel; level++)
            {
                AppendPageToMultiList(u_OpacityToRestore, g_FilteredPageListParams[level], oldPageData, slot);
            }
        }
    }
    if (lightingDirty || geometryDirty)
    {
        if (wasIlluminated || oldDataInvalid)
        {
            uint slot;
            for (uint level = newFinestLevel; level < g_NumPagedLevels; level++)
            {
                AppendPageToMultiList(u_EmittanceToClear, g_FilteredPageListParams[level], pageData, slot);
            }
        }
        value |= AMAP_LIGHTING_DIRTY_BIT;
    }
    u_AllocationMap[pageAddr] = value;
}
