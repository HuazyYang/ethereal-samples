#include "VoxelTextureCommon.hlsli"
RWTexture3D<uint> u_CoverageTexture: register(u0);
[numthreads(8,8,1)]
void main(
in uint3 gfsdk_GlobalIdx : SV_DispatchThreadID
)
{
int level = int(gfsdk_GlobalIdx.z);
int3 voxelPosition;
voxelPosition.xy = int2(gfsdk_GlobalIdx.xy);
voxelPosition.z = (g_LevelSizeCurrent + 2) * level;
u_CoverageTexture[voxelPosition] = u_CoverageTexture[voxelPosition + int3(0, 0, g_LevelSizeCurrent)];
u_CoverageTexture[voxelPosition + int3(0, 0, g_LevelSizeCurrent + 1)] = u_CoverageTexture[voxelPosition + int3(0, 0, 1)];
}
