#include "VoxelTextureCommon.hlsli"
Buffer<uint> t_PagesToProcess: register(t0);
Texture3D<float4> t_BackupOpacityTexture: register(t1);
RWTexture3D<uint> u_CoverageTexture: register(u0);
void RestoreOpacityPagesBody(PageCoordinates pageCoords, uint3 offset)
{
int3 levelAddressDst, backupAddressDst;
bool isInBackup;
GetLevelAndBackupAddress(int3(pageCoords.coords), g_LevelToProcess, offset, levelAddressDst, backupAddressDst, isInBackup);
levelAddressDst.z += int(g_OpacityPackingOffsetCurrent);
if(isInBackup)
{
float3 voxelizedOpacity = t_BackupOpacityTexture[backupAddressDst].xyz;
uint emittanceBits = u_CoverageTexture[levelAddressDst].x & MARK_OPACITY_MASK;
uint packedOpacity = VxgiPackOpacity(voxelizedOpacity) | emittanceBits;
u_CoverageTexture[levelAddressDst] = packedOpacity.xxxx;
}
}
PAGE_PROCESSING_CS_GROUP(RestoreOpacityPagesBody, t_PagesToProcess, g_ListParams)
