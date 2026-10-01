#include "VoxelTextureCommon.hlsli"
RWTexture3D<uint> u_CoverageTexture: register(u0);
[numthreads(4, 4, 4)]
void main(
in uint3 gfsdk_GlobalIdx : SV_DispatchThreadID
)
{
if (any(gfsdk_GlobalIdx.xyz >= int3scalar(g_LevelSizeCurrent)))
return;
int3 readAddrBase = int3(gfsdk_GlobalIdx.xyz * 2) + int3(0, 0, g_OpacityPackingOffsetPrevious);
float3 result = float3(0.0, 0.0, 0.0);
float4 values[8];
values[0] = VxgiUnpackOpacity(u_CoverageTexture[readAddrBase + int3(0, 0, 0)].x);
values[1] = VxgiUnpackOpacity(u_CoverageTexture[readAddrBase + int3(0, 0, 1)].x);
values[2] = VxgiUnpackOpacity(u_CoverageTexture[readAddrBase + int3(0, 1, 0)].x);
values[3] = VxgiUnpackOpacity(u_CoverageTexture[readAddrBase + int3(0, 1, 1)].x);
values[4] = VxgiUnpackOpacity(u_CoverageTexture[readAddrBase + int3(1, 0, 0)].x);
values[5] = VxgiUnpackOpacity(u_CoverageTexture[readAddrBase + int3(1, 0, 1)].x);
values[6] = VxgiUnpackOpacity(u_CoverageTexture[readAddrBase + int3(1, 1, 0)].x);
values[7] = VxgiUnpackOpacity(u_CoverageTexture[readAddrBase + int3(1, 1, 1)].x);
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
uint value = VxgiPackOpacity(result);
if(bool(g_PersistentVoxelData))
{
uint emittanceBits = u_CoverageTexture[int3(gfsdk_GlobalIdx)+int3(0, 0, g_OpacityPackingOffsetCurrent)].x & MARK_OPACITY_MASK;
value |= emittanceBits;
}
u_CoverageTexture[int3(gfsdk_GlobalIdx) + int3(0, 0, g_OpacityPackingOffsetCurrent)] = value;
}
