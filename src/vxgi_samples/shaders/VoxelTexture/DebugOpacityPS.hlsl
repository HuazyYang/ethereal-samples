#include "VoxelTextureDebugCommon.hlsli"

Texture3D<float4> t_OpacityXYZ_Pos: register(t0);
Texture3D<float4> t_OpacityXYZ_Neg: register(t1);
bool OutputFunction(int3 iPos, float3 fracPos, int direction, int3 iStep, out float3 color)
{
int3 texelPos = int3(iPos >> g_MipOffset) + int3(0, 0, g_OpacityPackingOffset);
float opacity = 0;
if(bool(g_Use6DOpacity))
{
float4 opacitiesPos, opacitiesNeg;
opacitiesPos.xyzw = t_OpacityXYZ_Pos[texelPos].xyzw;
opacitiesNeg.xyzw = t_OpacityXYZ_Neg[texelPos].xyzw;
if (opacitiesPos.x + opacitiesNeg.x + opacitiesPos.y + opacitiesNeg.y + opacitiesPos.z + opacitiesNeg.z == 0)
return false;
switch(direction)
{
case 0: opacity = (iStep.x > 0) ? opacitiesNeg.x : opacitiesPos.x; break;
case 1: opacity = (iStep.y > 0) ? opacitiesNeg.y : opacitiesPos.y; break;
case 2: opacity = (iStep.z > 0) ? opacitiesNeg.z : opacitiesPos.z; break;
}
}
else
{
float3 opacities = t_OpacityXYZ_Pos[texelPos].xyz;
if(all(opacities == 0.0.xxx))
return false;
switch(direction)
{
case 0: opacity = opacities.x; break;
case 1: opacity = opacities.y; break;
case 2: opacity = opacities.z; break;
}
}
float3 colorLow = float3(0, 0, 1);
float3 colorHigh = float3(1, 0, 0);
color.rgb = lerp(colorLow, colorHigh, opacity);
if(direction >= 0) color.rgb *= float(direction) * 0.15 + 0.5;
return true;
}
