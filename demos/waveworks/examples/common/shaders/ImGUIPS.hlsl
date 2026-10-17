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

struct PS_INPUT
{
	float4 pos : SV_POSITION;
	float4 col : TEXCOORD0;
	float2 uv  : TEXCOORD1;
};

// ---------------------------------------------------------------------------
// Global variables
// ---------------------------------------------------------------------------

sampler sampler0		: WW_REGISTER(s0);
Texture2D texture0		: WW_REGISTER(t0);

// ---------------------------------------------------------------------------
// Pixel shader
// ---------------------------------------------------------------------------

float4 main(PS_INPUT input) : SV_Target
{
	float4 out_col = input.col * (texture0.Sample(sampler0, input.uv));
	out_col.rgb *= out_col.a;
	return out_col;
}
