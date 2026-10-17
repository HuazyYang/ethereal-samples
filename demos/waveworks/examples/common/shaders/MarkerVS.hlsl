#ifndef WW_REGISTER
#ifdef WAVEWORKS_SPACE
#define WW_REGISTER(reg) register(reg, WAVEWORKS_SPACE)
#else
#define WW_REGISTER(reg) register(reg)
#endif
#endif
// ---------------------------------------------------------------------------
// Global structs
// ---------------------------------------------------------------------------

struct MARKER_VERTEX_INPUT
{
	float4 vPos										: POSITION;
	float4 vPerInstanceData							: PERINSTANCEDATA; 
};

struct VS_OUTPUT
{
	float4 vClipPos                                 : SV_Position;
	float3 vLocalPos								: TEXCOORD0;
};

// ---------------------------------------------------------------------------
// Global variables
// ---------------------------------------------------------------------------

cbuffer VS_CBUFFER_APP								: WW_REGISTER(b0)
{
	float4x4 g_matViewProj;
};


// ---------------------------------------------------------------------------
// Vertex shader
// ---------------------------------------------------------------------------

VS_OUTPUT main(MARKER_VERTEX_INPUT In)
{
	VS_OUTPUT Output ;
	Output.vLocalPos = In.vPos.xyz;
	Output.vClipPos = mul(g_matViewProj, float4(In.vPos.xyz + In.vPerInstanceData.xyz, 1.0));
#if API_VK
	Output.vClipPos.y = -Output.vClipPos.y;
#endif
	return Output;
}
