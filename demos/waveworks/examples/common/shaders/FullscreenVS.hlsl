/*
* Copyright (c) 2008-2017, NVIDIA CORPORATION. All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto. Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

// ---------------------------------------------------------------------------
// Global variables
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Global structs
// ---------------------------------------------------------------------------

struct VS_INPUT
{
	float4 vInPos                           : POSITION;
	float2 vInTexCoord                      : TEXCOORD0;
};


struct VS_OUTPUT
{
	float4 vPos                             : SV_Position;
	float2 vTexCoord                        : TEXCOORD0;
};

// ---------------------------------------------------------------------------
// Vertex shader
// ---------------------------------------------------------------------------

VS_OUTPUT main(VS_INPUT In)
{
	VS_OUTPUT vso;
	vso.vPos = In.vInPos;
	vso.vTexCoord = In.vInTexCoord;
	return vso;
}
