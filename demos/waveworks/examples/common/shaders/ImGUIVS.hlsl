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

cbuffer vertexBuffer : WW_REGISTER(b0)
{
	float4x4 ProjectionMatrix;
};

struct VS_INPUT
{
	float2 pos : POSITION0;
	float2 uv  : TEXCOORD0;
	float4 col : COLOR0;
	
};

struct PS_INPUT
{
	float4 pos : SV_Position;
	float4 col : TEXCOORD0;
	float2 uv  : TEXCOORD1;
};

// ---------------------------------------------------------------------------
// Vertex shader
// ---------------------------------------------------------------------------
PS_INPUT main(VS_INPUT input)
{
	PS_INPUT output;
	output.pos = mul(ProjectionMatrix, float4(input.pos.xy, 0.f, 1.f));
	output.col = input.col;
	output.uv = input.uv;
	return output;
}
