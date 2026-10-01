#include "Page.hlsli"
#include "AllocationMapCB.hlsli"
RWBuffer<uint> u_DestPages : register(u10);
[numthreads(1, 1, 1)]
void main(
    in uint3 gfsdk_GroupIdx : SV_DispatchThreadID
)
{
    uint level = min(gfsdk_GroupIdx.x, MAX_PAGED_LEVELS - 1);
    MultiListParams listParams = g_FilteredPageListParams[level];
    uint4 gridParams = g_GridSizeParams[level];
    uint pageCount = u_DestPages[listParams.segmentOffset + MULTILIST_COUNTER_OFFSET];
    uint gridSize = ((min(pageCount, listParams.maxElements) + gridParams.x) >> gridParams.y) << gridParams.z;
    u_DestPages[listParams.segmentOffset + MULTILIST_DISPATCHARGS_OFFSET + 0] = gridSize;
    u_DestPages[listParams.segmentOffset + MULTILIST_DISPATCHARGS_OFFSET + 1] = 1u << gridParams.z;
    u_DestPages[listParams.segmentOffset + MULTILIST_DISPATCHARGS_OFFSET + 2] = 1u << gridParams.z;
}