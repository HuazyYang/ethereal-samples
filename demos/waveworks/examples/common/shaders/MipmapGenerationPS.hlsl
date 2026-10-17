#ifndef WW_REGISTER
#ifdef WAVEWORKS_SPACE
#define WW_REGISTER(reg) register(reg, WAVEWORKS_SPACE)
#else
#define WW_REGISTER(reg) register(reg)
#endif
#endif
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

Texture2D g_texture1							: WW_REGISTER(t0);
SamplerState g_bilinearSampler					: WW_REGISTER(s0);

// ---------------------------------------------------------------------------
// Global structs
// ---------------------------------------------------------------------------
struct VS_OUTPUT
{
	float4 vPos									: SV_Position;
	float2 vTexCoord							: TEXCOORD0;
};

struct PS_OUTPUT
{
	float4 filtered								: SV_Target0;
};

// ---------------------------------------------------------------------------
// Pixel shader
// ---------------------------------------------------------------------------
PS_OUTPUT main(VS_OUTPUT In)                    
{
	PS_OUTPUT Output;
	Output.filtered = g_texture1.SampleLevel(g_bilinearSampler, In.vTexCoord.xy, 0);
	return Output;
}
