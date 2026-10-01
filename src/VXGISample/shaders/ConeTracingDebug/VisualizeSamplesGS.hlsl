#include "ConeTracingDebugGSCommon.hlsli"

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

#define SV_POSITION_ATTR_OUT OUT.position
struct VisualizeSamples_GSIn
{
};
[maxvertexcount(14)]
void main(point VisualizeSamples_GSIn dummyInput[1], uint gl_PrimitiveIDIn: SV_PrimitiveID, inout TriangleStream<VisualizeSamples_GSOut> outStream)
{
SampleData IN = t_SamplePositions[gl_PrimitiveIDIn];
if(g_ConeIndexFilter != 0 && g_ConeIndexFilter != IN.coneIndex)
return;
if(g_SampleIndexFilter > 0 && g_SampleIndexFilter != IN.sampleIndex)
return;
if(bool(g_OnlyContributingSamples) && dot(IN.sampledEmittance, float3scalar(1)) == 0)
return;
float size = pow(2, IN.mipLevel) * 0.5;
float4 positions[8];
for(int i = 0; i < 8; i++)
{
int3 direction = int3(i & 1, (i >> 1) & 1, (i >> 2) & 1) * 2 - 1;
float4 worldPos = float4(IN.worldSamplePos + size * direction * g_SmallestVoxelSize.xyz, 1);
positions[i] = mul(worldPos, g_ViewProjMatrixDebug);
}
float3 color = float3scalar(0);
switch(g_ColorSelection)
{
case 0:
{
float3 colorMipZero = float3(0, 1, 0);
float3 colorMipHighest = float3(1, 0, 1);
color = lerp(colorMipZero, colorMipHighest, float(IN.mipLevel) / (g_TotalMipLevels - 1));
}
break;
case 1:
color = IN.sampledEmittance * 8;
break;
case 2:
color = float3(IN.accumulatedOcclusion, IN.accumulatedOcclusion, IN.accumulatedOcclusion);
break;
}

VisualizeSamples_GSOut OUT;

const int vertexMapping[] = {3, 2, 7, 6, 4, 2, 0, 3, 1, 7, 5, 4, 1, 0};
OUT.color = color;
[unroll]
for(uint n = 0; n < 14; n++)
{
SV_POSITION_ATTR_OUT = positions[vertexMapping[n]];
outStream.Append(OUT);
}
outStream.RestartStrip();
}
