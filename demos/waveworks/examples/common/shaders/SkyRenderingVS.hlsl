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

struct VS_INPUT
{
	float4 vPos										: POSITION0;
	float2 vTexCoord								: TEXCOORD0;
};

struct VS_OUTPUT
{
	float4 vClipPos									: SV_Position;
	float2 vTexCoord								: TEXCOORD0;
	float3 vEyeToPixelVector						: TEXCOORD1;
};

// ---------------------------------------------------------------------------
// Global variables
// ---------------------------------------------------------------------------
cbuffer VS_CBUFFER									: WW_REGISTER(b0)
{
	float4x4 g_matViewProj;
};

// ---------------------------------------------------------------------------
// Vertex shader
// ---------------------------------------------------------------------------
VS_OUTPUT main(VS_INPUT In)
{
	VS_OUTPUT Output;
	Output.vClipPos = mul(g_matViewProj, In.vPos);
	Output.vTexCoord = In.vTexCoord;
	Output.vEyeToPixelVector = In.vPos.xyz;
	return Output;
}
