#include "VoxelTextureCommon.hlsli"
Buffer<uint> t_PagesToProcess: register(t0);
RWTexture3D<uint> u_CoverageTexture: register(u0);
RWTexture3D<uint> u_BackupOpacityTexture: register(u5);
#if USE_6D_OPACITY
RWTexture3D<uint> u_CoverageTextureNeg: register(u1);
RWTexture3D<uint> u_BackupOpacityTextureNeg: register(u6);
#endif
uint3 GetDirectionalCoverage(uint _coverage)
{
uint3 coverage;
coverage.x = _coverage & 0x3ff;
coverage.y = _coverage & (0x3ff << 10);
coverage.z = _coverage & (0x3ff << 20);
return coverage;
}
uint ConvertCoverageToOpacity(uint coverageBits)
{
uint3 coverage = GetDirectionalCoverage(coverageBits);
uint3 bitcounts = countbits(coverage);
uint packedOpacity = 0;
packedOpacity |= uint(saturate(float(bitcounts.x) / 8.0) * 1023.0);
packedOpacity |= uint(saturate(float(bitcounts.y) / 8.0) * 1023.0) << 10;
packedOpacity |= uint(saturate(float(bitcounts.z) / 8.0) * 1023.0) << 20;
packedOpacity |= coverageBits & MARK_OPACITY_MASK;
return packedOpacity;
}
void ConvertCoverageToOpacityBody(PageCoordinates pageCoords, uint3 offset)
{
int3 levelAddress, backupAddress;
bool isInBackup;
if(bool(g_PersistentVoxelData))
{
GetLevelAndBackupAddress(int3(pageCoords.coords), g_LevelToProcess, offset, levelAddress, backupAddress, isInBackup);
}
else
{
levelAddress = GetLevelAddress(int3(pageCoords.coords), g_TranslationParamsCurrent, g_ToroidalOffsetCurrent.xyz, g_LevelSizeCurrent);
levelAddress += int3(offset);
backupAddress = int3scalar(0);
isInBackup = false;
}
levelAddress.z += int(g_OpacityPackingOffsetCurrent);
uint coverageBits = u_CoverageTexture[levelAddress].x;
#if USE_6D_OPACITY
uint coverageBitsNeg = u_CoverageTextureNeg[levelAddress].x;
#endif
if (isInBackup || bool(coverageBits))
{
uint packedOpacity = ConvertCoverageToOpacity(coverageBits);
if (isInBackup)
    u_BackupOpacityTexture[backupAddress] = packedOpacity;
else
u_CoverageTexture[levelAddress] = packedOpacity;
}
#if USE_6D_OPACITY
if (isInBackup || bool(coverageBitsNeg))
{
uint packedOpacity = ConvertCoverageToOpacity(coverageBitsNeg);
if (isInBackup)
    u_BackupOpacityTextureNeg[backupAddress] = packedOpacity;
else
u_CoverageTextureNeg[levelAddress] = packedOpacity;
}
#endif
}
PAGE_PROCESSING_CS_GROUP(ConvertCoverageToOpacityBody,t_PagesToProcess,g_ListParams)
