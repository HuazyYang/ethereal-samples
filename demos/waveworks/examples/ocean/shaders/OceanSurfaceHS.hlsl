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
struct VS_OUTPUT 
{
	float4 worldspacePosition                       : VSO;
};

struct HS_CONSTANTOUTPUT
{
	float edgeTessFactors[3]                        : SV_TessFactor; 
	float insideTessFactor                          : SV_InsideTessFactor;
};

struct HS_OUTPUT
{
	float4 worldspacePosition                       : HSO;
};

// ---------------------------------------------------------------------------
// Global variables
// ---------------------------------------------------------------------------
cbuffer OCEAN_VS_HS_DS_CBUFFER                      : WW_REGISTER(b0)
{
	// Data used in vertex shader
	float4x4       g_matViewProj;
	float3         g_eyePos;
	float          g_meanOceanLevel;
	// x16 bytes boundary
	float          g_useDiamondPattern;

	// Data used in hull shader
	float          g_dynamicTesselationAmount;
	float          g_staticTesselationOffset;

	// Data used in domain shader
	// Wind waves related data
	float          g_cascade0UVScale;
	// x16 bytes boundary
	float          g_cascade1UVScale;
	float          g_cascade2UVScale;
	float          g_cascade3UVScale;
	float          g_cascade0UVOffset;
	// x16 bytes boundary
	float          g_cascade1UVOffset;
	float          g_cascade2UVOffset;
	float          g_cascade3UVOffset;
	float          g_UVWarpingAmplitude;
	// x16 bytes boundary
	float          g_UVWarpingFrequency;

	// Local waves related data
	float          g_localWavesSimulationDomainWorldspaceSize;
	float2         g_localWavesSimulationDomainWorldspaceCenter;
	float          g_beachScene;
	float3         g_beachPadding;
};

// ---------------------------------------------------------------------------
// Helper functions
// ---------------------------------------------------------------------------
float GFSDK_WaveWorks_GetEdgeTessellationFactor(float4 vertex1, float4 vertex2)
{
	float3 edgeCenter = 0.5 * (vertex1.xyz + vertex2.xyz);
	float  edgeLength = length(vertex1.xyz - vertex2.xyz);
	float  distanceToEdge = length(g_eyePos - edgeCenter);
	return g_staticTesselationOffset + g_dynamicTesselationAmount * edgeLength / distanceToEdge;
}

// ---------------------------------------------------------------------------
// Hull constant shader
// ---------------------------------------------------------------------------

HS_CONSTANTOUTPUT HS_Constant( InputPatch<VS_OUTPUT, 3> I )
{
	HS_CONSTANTOUTPUT Output;
	Output.edgeTessFactors[0] = GFSDK_WaveWorks_GetEdgeTessellationFactor(I[1].worldspacePosition, I[2].worldspacePosition);
	Output.edgeTessFactors[1] = GFSDK_WaveWorks_GetEdgeTessellationFactor(I[2].worldspacePosition, I[0].worldspacePosition);
	Output.edgeTessFactors[2] = GFSDK_WaveWorks_GetEdgeTessellationFactor(I[0].worldspacePosition, I[1].worldspacePosition);
	Output.insideTessFactor = (Output.edgeTessFactors[0] + Output.edgeTessFactors[1] + Output.edgeTessFactors[2]) / 3.0;
	return Output;
}

// ---------------------------------------------------------------------------
// Hull shader
// ---------------------------------------------------------------------------
[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[patchconstantfunc("HS_Constant")]
[outputcontrolpoints(3)]
HS_OUTPUT main( InputPatch<VS_OUTPUT, 3> I, uint uCPID : SV_OutputControlPointID )
{
	HS_OUTPUT Output;
	Output.worldspacePosition = float4(I[uCPID].worldspacePosition);
	return Output;
}
