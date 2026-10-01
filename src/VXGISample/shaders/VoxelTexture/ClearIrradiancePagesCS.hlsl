#include "IrradianceMapCB.hlsli"

Buffer<uint> t_PagesToProcess: register(t0);
RWTexture3D<float4> u_IrradianceMap: register(u3);
void ClearIrradiancePagesBody(PageCoordinates pageCoords, uint3 offset)
{
int3 voxelPosition = int3(pageCoords.coords.xyz << g_LogPageSize) + int3(offset);
[unroll]
for (uint direction = 0; direction < 6; ++direction)
{
    u_IrradianceMap[voxelPosition + int3(0, 0, direction * g_IrradianceMapSize)] = float4scalar(0);
}
}
PAGE_PROCESSING_CS_GROUP(ClearIrradiancePagesBody,t_PagesToProcess,g_ListParams)
