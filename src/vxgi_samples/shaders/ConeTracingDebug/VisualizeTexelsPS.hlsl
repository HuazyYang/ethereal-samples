#include "../Common/ShaderCommon.hlsli"

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
void main(VisualizeTexels_GSOut IN, out float4 gl_FragColor : SV_Target)
{
float3 color;
if(IN.quadPos.x > 0.5)
color = IN.colorX;
else if(IN.quadPos.y > 0.5)
color = IN.colorY;
else
color = IN.color0;
int2 iPos = int2(IN.quadPos * 16);
if(!any(color != float3scalar(0)) && ((iPos.x ^ iPos.y) & 1) != 0)
color = float3(0.1, 0.1, 0.1);
gl_FragColor = float4(color, 0.7);
}
