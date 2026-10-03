#include "VoxelTextureCommon.hlsli"
Buffer<uint> t_PagesToProcess: register(t0);
RWTexture3D<uint> u_CoverageTexture: register(u0);
Texture3D<float4> t_BackupOpacityTexture: register(t1);
void DownsampleOpacityPagesBody(PageCoordinates pageCoords, uint3 offset)
{
int3 levelAddressSrc = GetLevelAddress(int3(pageCoords.coords), g_TranslationParamsPrevious, g_ToroidalOffsetPrevious.xyz, int(g_LevelSizePrevious));
levelAddressSrc += int3(offset) * 2;
levelAddressSrc.z += int(g_OpacityPackingOffsetPrevious);
int3 levelAddressDst, backupAddressDst;
bool isInBackup;
GetLevelAndBackupAddress(int3(pageCoords.coords), g_LevelToProcess, offset, levelAddressDst, backupAddressDst, isInBackup);
levelAddressDst.z += int(g_OpacityPackingOffsetCurrent);
float3 result = float3(0.0, 0.0, 0.0);
float4 values[8];
values[0] = VxgiUnpackOpacity(u_CoverageTexture[levelAddressSrc + int3(0, 0, 0)].x);
values[1] = VxgiUnpackOpacity(u_CoverageTexture[levelAddressSrc + int3(0, 0, 1)].x);
values[2] = VxgiUnpackOpacity(u_CoverageTexture[levelAddressSrc + int3(0, 1, 0)].x);
values[3] = VxgiUnpackOpacity(u_CoverageTexture[levelAddressSrc + int3(0, 1, 1)].x);
values[4] = VxgiUnpackOpacity(u_CoverageTexture[levelAddressSrc + int3(1, 0, 0)].x);
values[5] = VxgiUnpackOpacity(u_CoverageTexture[levelAddressSrc + int3(1, 0, 1)].x);
values[6] = VxgiUnpackOpacity(u_CoverageTexture[levelAddressSrc + int3(1, 1, 0)].x);
values[7] = VxgiUnpackOpacity(u_CoverageTexture[levelAddressSrc + int3(1, 1, 1)].x);
result.x = VxgiAverage4(
VxgiMultiplyComplements(values[0].x, values[4].x),
VxgiMultiplyComplements(values[1].x, values[5].x),
VxgiMultiplyComplements(values[2].x, values[6].x),
VxgiMultiplyComplements(values[3].x, values[7].x)
);
result.y = VxgiAverage4(
VxgiMultiplyComplements(values[0].y, values[2].y),
VxgiMultiplyComplements(values[1].y, values[3].y),
VxgiMultiplyComplements(values[4].y, values[6].y),
VxgiMultiplyComplements(values[5].y, values[7].y)
);
result.z = VxgiAverage4(
VxgiMultiplyComplements(values[0].z, values[1].z),
VxgiMultiplyComplements(values[2].z, values[3].z),
VxgiMultiplyComplements(values[4].z, values[5].z),
VxgiMultiplyComplements(values[6].z, values[7].z)
);
if(isInBackup)
{
int scale = int(1 << (int(g_StackSize) - int(g_LevelToProcess) - 1));
float rMapSize = rcp(float(g_LevelSizeCurrent));
float pageSize = (1 << g_AllocationLodBias) * rMapSize * scale;
float3 v = abs(float3(pageCoords.coords) * pageSize - 0.5 * scale + float3(offset) * rMapSize - g_ClipmapAnchorOffset.xyz) + pageSize;
float interpolationFactor = saturate(8 * max(max(v.x, v.y), v.z) - 1);
if(interpolationFactor > 0)
{
float3 voxelizedOpacity;
if(bool(g_PersistentVoxelData))
voxelizedOpacity = t_BackupOpacityTexture[backupAddressDst].xyz;
else
voxelizedOpacity = VxgiUnpackOpacity(u_CoverageTexture[levelAddressDst].x).xyz;
result = lerp(result, voxelizedOpacity, interpolationFactor);
}
}
uint emittanceBits = 0;
if(bool(g_PersistentVoxelData))
{
emittanceBits = u_CoverageTexture[levelAddressDst].x & MARK_OPACITY_MASK;
}
uint packedOpacity = VxgiPackOpacity(result) | emittanceBits;
u_CoverageTexture[levelAddressDst] = packedOpacity;
}
PAGE_PROCESSING_CS_GROUP(DownsampleOpacityPagesBody,t_PagesToProcess,g_ListParams)
