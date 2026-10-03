#include "VoxelTextureDebugCommon.hlsli"

Texture3D<float4> t_Irradiance: register(t0);
bool OutputFunction(int3 iPos, float3 fracPos, int direction, int3 iStep, out float3 color)
{
int zOffset = 0;
switch(direction)
{
case 0: zOffset = (iStep.x > 0) ? EMITTANCE_NEGATIVE_X : EMITTANCE_POSITIVE_X; break;
case 1: zOffset = (iStep.y > 0) ? EMITTANCE_NEGATIVE_Y : EMITTANCE_POSITIVE_Y; break;
case 2: zOffset = (iStep.z > 0) ? EMITTANCE_NEGATIVE_Z : EMITTANCE_POSITIVE_Z; break;
}
float3 irradiance = t_Irradiance[iPos + int3(0, 0, g_TextureSize * zOffset)].rgb;
if(!any(irradiance != float3scalar(0)))
return false;
color.rgb = irradiance * g_SourceDataScale;
return true;
}
