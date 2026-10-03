#include "VoxelTextureDebugCommon.hlsli"

Texture3D<float4> t_OpacityXYZ_Pos: register(t0);
Texture3D<float4> t_OpacityXYZ_Neg: register(t1);
#if EMITTANCE_FORMAT == FLOAT32
Texture3D<float4> t_EmittanceR: register(t2);
Texture3D<float4> t_EmittanceG: register(t3);
Texture3D<float4> t_EmittanceB: register(t4);
#else
Texture3D<float4> t_Emittance: register(t2);
#endif
bool OutputFunction(int3 iPos, float3 fracPos, int direction, int3 iStep, out float3 color)
{
color = float3(0.0, 0.0, 0.0);
int3 texelPos = int3(iPos >> g_MipOffset);
int levelSize = g_TextureSize >> g_MipOffset;
int packingStride = g_TextureSize + 2;
int emittanceDirection = 0;
switch(direction)
{
case 0: emittanceDirection = (iStep.x > 0) ? EMITTANCE_NEGATIVE_X : EMITTANCE_POSITIVE_X; break;
case 1: emittanceDirection = (iStep.y > 0) ? EMITTANCE_NEGATIVE_Y : EMITTANCE_POSITIVE_Y; break;
case 2: emittanceDirection = (iStep.z > 0) ? EMITTANCE_NEGATIVE_Z : EMITTANCE_POSITIVE_Z; break;
}
int3 emittanceAddr = texelPos;
emittanceAddr.x += 1 + emittanceDirection * packingStride;
emittanceAddr.z += int(g_EmittancePackingOffset);
float3 emittance = float3scalar(0);
#if EMITTANCE_FORMAT == FLOAT32
emittance.r = t_EmittanceR[emittanceAddr].r;
emittance.g = t_EmittanceG[emittanceAddr].r;
emittance.b = t_EmittanceB[emittanceAddr].r;
#else
emittance = t_Emittance[emittanceAddr].rgb;
#endif
color.rgb = emittance * g_SourceDataScale;
#if MARK_EMITTANCE_IN_OPACITY
texelPos.z += int(g_OpacityPackingOffset);
bool presenceFlag = t_OpacityXYZ_Pos[texelPos].a != 0;
bool reallyPresent = any(emittance != float3scalar(0));
if(!presenceFlag && !reallyPresent)
return false;
#if 0
float2 fracXY = 0;
switch(direction)
{
case 0: fracXY = fracPos.yz; break;
case 1: fracXY = fracPos.xz; break;
case 2: fracXY = fracPos.xy; break;
}
if(fracXY.x < 0.05 || fracXY.x > 0.95 || fracXY.y < 0.05 || fracXY.y > 0.95)
{
color.rgb = float3(0.0, 0.0, 0.0);
if(presenceFlag && reallyPresent)
color.g = 1.0;
else if(presenceFlag && !reallyPresent)
color.b = 1.0;
else if(!presenceFlag && reallyPresent)
color.r = 1.0;
}
#endif
#else
if(all(emittance == 0.0.xxx))
return false;
#endif
return true;
}
