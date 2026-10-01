#include "VoxelTextureCommon.hlsli"
RWTexture3D<uint> u_CoverageTexture: register(u0);
[numthreads(8, 8, 1)]
void main(
in uint3 gfsdk_GlobalIdx : SV_DispatchThreadID
)
{
if (all(gfsdk_GlobalIdx.xy < uint2scalar(uint(g_LevelSizeCurrent) + 2)))
{
uint3 dstPosition;
dstPosition.xy = (gfsdk_GlobalIdx.xy - 1) & (g_TextureSize.xy - 1);
dstPosition.z = gfsdk_GlobalIdx.z == 0
? g_OpacityPackingOffsetCurrent - 1
: g_OpacityPackingOffsetCurrent + uint(g_LevelSizeCurrent);
uint3 srcPosition;
srcPosition.xy = (gfsdk_GlobalIdx.xy - 1) & uint(g_LevelSizeCurrent - 1);
srcPosition.z = gfsdk_GlobalIdx.z == 0
                    ? g_OpacityPackingOffsetCurrent + uint(g_LevelSizeCurrent) - 1
                    : g_OpacityPackingOffsetCurrent;
u_CoverageTexture[int3(dstPosition)] = u_CoverageTexture[int3(srcPosition)];
}
if ((gfsdk_GlobalIdx.x < uint(g_LevelSizeCurrent) + 2) && (gfsdk_GlobalIdx.y < uint(g_LevelSizeCurrent)))
{
uint3 dstPosition;
dstPosition.x = (gfsdk_GlobalIdx.x - 1) & (g_TextureSize.x - 1);
dstPosition.z = gfsdk_GlobalIdx.y + g_OpacityPackingOffsetCurrent;
dstPosition.y = gfsdk_GlobalIdx.z == 0
? uint(g_LevelSizeCurrent)
: g_TextureSize.y - 1;
uint3 srcPosition;
srcPosition.x = (gfsdk_GlobalIdx.x - 1) & uint(g_LevelSizeCurrent - 1);
srcPosition.z = gfsdk_GlobalIdx.y + g_OpacityPackingOffsetCurrent;
srcPosition.y = gfsdk_GlobalIdx.z == 0
                    ? 0
                    : uint(g_LevelSizeCurrent) - 1;
u_CoverageTexture[int3(dstPosition)] = u_CoverageTexture[int3(srcPosition)];
}
if (all(gfsdk_GlobalIdx.xy < uint2scalar(uint(g_LevelSizeCurrent))))
{
uint3 dstPosition;
dstPosition.y = gfsdk_GlobalIdx.x;
dstPosition.z = gfsdk_GlobalIdx.y + g_OpacityPackingOffsetCurrent;
dstPosition.x = gfsdk_GlobalIdx.z == 0
? uint(g_LevelSizeCurrent)
: g_TextureSize.y - 1;
uint3 srcPosition;
srcPosition.y = gfsdk_GlobalIdx.x;
srcPosition.z = gfsdk_GlobalIdx.y + g_OpacityPackingOffsetCurrent;
srcPosition.x = gfsdk_GlobalIdx.z == 0
                    ? 0
                    : uint(g_LevelSizeCurrent) - 1;
u_CoverageTexture[int3(dstPosition)] = u_CoverageTexture[int3(srcPosition)];
}
}
