#include "ConeTracingDebugGSCommon.hlsli"
#include "../ConeTracing/ConeTracingCommon.hlsli"

/*
* Copyright (c) 2012-2016, NVIDIA CORPORATION. All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto. Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/
cbuffer VisualizeConstants: register(b0)
{
float4x4 g_ViewProjMatrixDebug;
float4 g_SmallestVoxelSize;
float4 g_ClipmapCenterDebug;
float4 g_ClipmapCenterAtSave;
float4 g_ToroidalOffsetDebug;
uint g_TotalMipLevels;
uint g_ColorSelection;
uint g_OnlyContributingSamples;
int g_ConeIndexFilter;
int g_SampleIndexFilter;
int g_StackSizeDebug;
int g_StackTextureSizeDebug;
int g_PackingStrideDebug;
};
struct VisualizeSamples_GSOut
{
float4 position : SV_Position;
float3 color    : COLOR;
};
struct VisualizeTexels_GSOut
{
float4 position : SV_Position;
float3 color0   : COLOR0;
float3 colorX   : COLORX;
float3 colorY   : COLORY;
float2 quadPos  : QUADPOS;
};
StructuredBuffer<SampleData> t_SamplePositions: register(t0);
Texture3D<float4> t_EmittanceDebug: register(t2);

#define SV_POSITION_ATTR_OUT OUT.position
struct VisualizeSamples_GSIn
{
};
void AppendTexelQuad(float4 positions[8], inout TriangleStream<VisualizeTexels_GSOut> outStream, float3 colors[8], int a, int b, int c, int d)
{
VisualizeTexels_GSOut OUT;
OUT.color0 = colors[b];
OUT.colorX = colors[a];
OUT.colorY = colors[c];
OUT.quadPos = float2(0, 1); SV_POSITION_ATTR_OUT = positions[c]; outStream.Append(OUT);
OUT.quadPos = float2(0, 0); SV_POSITION_ATTR_OUT = positions[b]; outStream.Append(OUT);
OUT.quadPos = float2(1, 0); SV_POSITION_ATTR_OUT = positions[a]; outStream.Append(OUT);
outStream.RestartStrip();
OUT.color0 = colors[d];
OUT.colorX = colors[c];
OUT.colorY = colors[a];
OUT.quadPos = float2(0, 0); SV_POSITION_ATTR_OUT = positions[d]; outStream.Append(OUT);
OUT.quadPos = float2(1, 0); SV_POSITION_ATTR_OUT = positions[c]; outStream.Append(OUT);
OUT.quadPos = float2(0, 1); SV_POSITION_ATTR_OUT = positions[a]; outStream.Append(OUT);
outStream.RestartStrip();
}
float3 GetLevelCoordinatesDebug(float3 position, float level)
{
float4 translationParams = g_VxgiTranslationParameters[int(level)];
float3 positionInClipmap = (position - g_ClipmapCenterDebug.xyz) * g_ClipmapCenterDebug.w * translationParams.x + 0.5;
float3 toroidalOffset = g_ToroidalOffsetDebug.xyz * translationParams.y;
float3 fVoxelCoord = frac(positionInClipmap + toroidalOffset);
float3 iVoxelCoord = fVoxelCoord * g_StackTextureSizeDebug * translationParams.z;
iVoxelCoord.z += translationParams.w;
return floor(iVoxelCoord);
}
float3 LoadEmittance(float3 coords, float3 direction)
{
int3 pos = int3(coords);
float3 emittanceX, emittanceY, emittanceZ;
emittanceX = t_EmittanceDebug[pos + int3(((direction.x > 0) ? EMITTANCE_NEGATIVE_X : EMITTANCE_POSITIVE_X) * g_PackingStrideDebug + 1, 0, 0)].rgb;
emittanceY = t_EmittanceDebug[pos + int3(((direction.y > 0) ? EMITTANCE_NEGATIVE_Y : EMITTANCE_POSITIVE_Y) * g_PackingStrideDebug + 1, 0, 0)].rgb;
emittanceZ = t_EmittanceDebug[pos + int3(((direction.z > 0) ? EMITTANCE_NEGATIVE_Z : EMITTANCE_POSITIVE_Z) * g_PackingStrideDebug + 1, 0, 0)].rgb;
return abs(direction.x) * emittanceX + abs(direction.y) * emittanceY + abs(direction.z) * emittanceZ;
}

[maxvertexcount(48)]
void main(point VisualizeSamples_GSIn dummyInput[1], uint gl_PrimitiveIDIn: SV_PrimitiveID, inout TriangleStream<VisualizeTexels_GSOut> outStream)
{
SampleData IN = t_SamplePositions[gl_PrimitiveIDIn];
if(g_ConeIndexFilter != 0 && g_ConeIndexFilter != IN.coneIndex)
return;
if(g_SampleIndexFilter > 0 && g_SampleIndexFilter != IN.sampleIndex)
return;
if(bool(g_OnlyContributingSamples) && dot(IN.sampledEmittance, float3scalar(1)) == 0)
return;
float level = floor(IN.mipLevel);
if(g_ColorSelection == 4)
level += 1;
float size = pow(2, level);
float3 voxelSize = g_SmallestVoxelSize.xyz * size;
float3 center = round((IN.worldSamplePos - g_ClipmapCenterAtSave.xyz) / voxelSize) * voxelSize + g_ClipmapCenterAtSave.xyz;
float4 positions[8];
float3 colors[8];
for(int i = 0; i < 8; i++)
{
float3 direction = float3(i & 1, (i >> 1) & 1, (i >> 2) & 1) * 2 - 1;
float4 worldPos = float4(center + direction * voxelSize, 1);
colors[i] = 8 * LoadEmittance(GetLevelCoordinatesDebug(center + direction * 0.5, level), IN.direction);
positions[i] = mul(worldPos, g_ViewProjMatrixDebug);
}
AppendTexelQuad(positions, outStream, colors, 4, 6, 7, 5);
AppendTexelQuad(positions, outStream, colors, 0, 1, 3, 2);
AppendTexelQuad(positions, outStream, colors, 2, 3, 7, 6);
AppendTexelQuad(positions, outStream, colors, 0, 4, 5, 1);
AppendTexelQuad(positions, outStream, colors, 1, 5, 7, 3);
AppendTexelQuad(positions, outStream, colors, 0, 2, 6, 4);
}
