#include "VoxelTextureDebugCommon.hlsli"

Texture3D<float4> t_CoverageXYZ_Pos: register(t0);
Texture3D<float4> t_CoverageXYZ_Neg: register(t1);
bool OutputFunction(int3 iPos, float3 fracPos, int direction, int3 iStep, out float3 color)
{
color = float3(0.0, 0.0, 0.0);
int3 texelPos = int3(iPos >> g_MipOffset) + int3(0, 0, g_OpacityPackingOffset);
uint dirCoverage = 0;
if(bool(g_Use6DOpacity))
{
float3 coveragePosf = t_CoverageXYZ_Pos[texelPos].xyz * ((4.0 * 256.0) - 1.0);
float3 coverageNegf = t_CoverageXYZ_Neg[texelPos].xyz * ((4.0 * 256.0) - 1.0);
uint3 coveragePos = uint3(uint(ceil(coveragePosf.x)), uint(ceil(coveragePosf.y)), uint(ceil(coveragePosf.z)));
uint3 coverageNeg = uint3(uint(ceil(coverageNegf.x)), uint(ceil(coverageNegf.y)), uint(ceil(coverageNegf.z)));
if (all(coveragePos == 0u.xxx) && all(coverageNeg == 0u.xxx))
return false;
switch(direction)
{
case 0: dirCoverage = (iStep.x > 0) ? coverageNeg.x : coveragePos.x; break;
case 1: dirCoverage = (iStep.y > 0) ? coverageNeg.y : coveragePos.y; break;
case 2: dirCoverage = (iStep.z > 0) ? coverageNeg.z : coveragePos.z; break;
}
}
else
{
float3 coveragef = t_CoverageXYZ_Pos[texelPos].xyz * ((4.0 * 256.0) - 1.0);
uint3 coverage = uint3(uint(ceil(coveragef.x)), uint(ceil(coveragef.y)), uint(ceil(coveragef.z)));
if (all(coverage == 0u.xxx))
return false;
switch(direction)
{
case 0: dirCoverage = coverage.x; break;
case 1: dirCoverage = coverage.y; break;
case 2: dirCoverage = coverage.z; break;
}
}
float2 fracXY = float2(0.0, 0.0);
switch(direction)
{
case 0: fracXY = fracPos.yz; break;
case 1: fracXY = fracPos.xz; break;
case 2: fracXY = fracPos.xy; break;
}
int2 samplePos = int2(fracXY * 3);
int index = samplePos.y * 3 + samplePos.x;
bool covered = bool((dirCoverage >> index) & 1);
float3 colorLow = float3(0, 0, 0);
float3 colorHigh = float3(0.5, 0.5, 0.5);
color.rgb = covered ? colorHigh : colorLow;
if(fracXY.x < 0.05 || fracXY.x > 0.95 || fracXY.y < 0.05 || fracXY.y > 0.95)
color.r = 1.0;
return true;
}
