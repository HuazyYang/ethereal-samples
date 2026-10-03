#ifndef PAGE_HLSLI
#define PAGE_HLSLI
#include "../Common/ShaderCommon.hlsli"

struct PageCoordinates {
    uint level;
    uint3 coords;
};
struct PageData {
    uint data;
};
struct MultiListParams {
    uint segmentOffset;
    uint maxElements;
    uint logPageSize;
    uint dummy;
};
PageData PackPageCoordinates(uint level, uint3 coords)
{
    PageData pageData;
    pageData.data = PAGE_DATA_VALID_BIT;
    pageData.data |= (level << 27);
    pageData.data |= (coords.x << 18);
    pageData.data |= (coords.y << 9);
    pageData.data |= (coords.z << 0);
    return pageData;
}
bool IsPageDataValid(PageData pageData)
{
    return (pageData.data & PAGE_DATA_VALID_BIT) != 0;
}
PageCoordinates UnpackPageData(PageData pageData)
{
    PageCoordinates pageCoords;
    pageCoords.level = (pageData.data >> 27) & 0x00f;
    pageCoords.coords.x = (pageData.data >> 18) & 0x1ff;
    pageCoords.coords.y = (pageData.data >> 9) & 0x1ff;
    pageCoords.coords.z = (pageData.data >> 0) & 0x1ff;
    return pageCoords;
}
uint GetMostDetailedLevelWithPage(uint3 page, uint textureSize, uint maxClipLevel)
{
    float halfTextureSize = float(textureSize) * 0.5;
    float3 distancesFromCenter = abs(float3(page) - float3scalar(halfTextureSize - 0.5));
    distancesFromCenter /= halfTextureSize;
    float maxDistance = max(distancesFromCenter.x, max(distancesFromCenter.y, distancesFromCenter.z));
    int level = max(0, int(ceil(log2(maxDistance)) + maxClipLevel));
    return uint(level);
}
#define PAGE_PROCESSING_CS(BODY, PAGES, PARAMS)                                                                                                    \
    [numthreads(GROUP_SIZE, GROUP_SIZE, GROUP_SIZE)]                                                                                               \
    void main(in uint3 gfsdk_GroupIdx: SV_GroupID, in uint3 gfsdk_GroupThreadIdx: SV_GroupThreadID, in uint3 gfsdk_GlobalIdx: SV_DispatchThreadID) \
    {                                                                                                                                              \
        PageData pageData;                                                                                                                         \
        pageData.data = uint(PAGES[int((gfsdk_GlobalIdx.x >> PARAMS.logPageSize) + PARAMS.segmentOffset + MULTILIST_DATA_OFFSET)].x);              \
        if (!IsPageDataValid(pageData))                                                                                                            \
            return;                                                                                                                                \
        PageCoordinates pageCoords = UnpackPageData(pageData);                                                                                     \
        uint3 offset = gfsdk_GlobalIdx;                                                                                                            \
        offset.x &= ((1 << PARAMS.logPageSize) - 1);                                                                                               \
        BODY(pageCoords, offset);                                                                                                                  \
    }
#define PAGE_PROCESSING_CS_1(BODY, PAGES, PARAMS)                                                                                                  \
    [numthreads(64, 1, 1)]                                                                                                                         \
    void main(in uint3 gfsdk_GroupIdx: SV_GroupID, in uint3 gfsdk_GroupThreadIdx: SV_GroupThreadID, in uint3 gfsdk_GlobalIdx: SV_DispatchThreadID) \
    {                                                                                                                                              \
        PageData pageData;                                                                                                                         \
        if (gfsdk_GlobalIdx.x >= PAGES[int(PARAMS.segmentOffset + MULTILIST_COUNTER_OFFSET)].x)                                                    \
            return;                                                                                                                                \
        pageData.data = PAGES[int(gfsdk_GlobalIdx.x + PARAMS.segmentOffset + MULTILIST_DATA_OFFSET)].x;                                            \
        if (!IsPageDataValid(pageData))                                                                                                            \
            return;                                                                                                                                \
        PageCoordinates pageCoords = UnpackPageData(pageData);                                                                                     \
        uint3 offset = uint3(0, 0, 0);                                                                                                             \
        BODY(pageCoords, offset);                                                                                                                  \
    }
#define PAGE_PROCESSING_CS_2(BODY, PAGES, PARAMS)                                                                                                  \
    [numthreads(16, 2, 2)]                                                                                                                         \
    void main(in uint3 gfsdk_GroupIdx: SV_GroupID, in uint3 gfsdk_GroupThreadIdx: SV_GroupThreadID, in uint3 gfsdk_GlobalIdx: SV_DispatchThreadID) \
    {                                                                                                                                              \
        PageData pageData;                                                                                                                         \
        if ((gfsdk_GlobalIdx.x >> 1) >= PAGES[int(PARAMS.segmentOffset + MULTILIST_COUNTER_OFFSET)].x)                                             \
            return;                                                                                                                                \
        pageData.data = PAGES[int((gfsdk_GlobalIdx.x >> 1) + PARAMS.segmentOffset + MULTILIST_DATA_OFFSET)].x;                                     \
        if (!IsPageDataValid(pageData))                                                                                                            \
            return;                                                                                                                                \
        PageCoordinates pageCoords = UnpackPageData(pageData);                                                                                     \
        uint3 offset = gfsdk_GlobalIdx;                                                                                                            \
        offset.x &= 1;                                                                                                                             \
        BODY(pageCoords, offset);                                                                                                                  \
    }
#if GROUP_SIZE == 1
#define PAGE_PROCESSING_CS_GROUP(BODY, PAGES, PARAMS) PAGE_PROCESSING_CS_1(BODY, PAGES, PARAMS)
#elif GROUP_SIZE == 2
#define PAGE_PROCESSING_CS_GROUP(BODY, PAGES, PARAMS) PAGE_PROCESSING_CS_2(BODY, PAGES, PARAMS)
#else
#define PAGE_PROCESSING_CS_GROUP(BODY, PAGES, PARAMS) PAGE_PROCESSING_CS(BODY, PAGES, PARAMS)
#endif
#define AppendPageToMultiList(list, params, _data, slot)                            \
    InterlockedAdd(list[params.segmentOffset + MULTILIST_COUNTER_OFFSET], 1, slot); \
    if (slot < params.maxElements)                                                  \
        list[slot + params.segmentOffset + MULTILIST_DATA_OFFSET] = _data.data;
#endif /* PAGE_HLSLI */
