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
struct HS_CONSTANTOUTPUT
{
	float edgeTessFactors[3]                                 : SV_TessFactor;
	float insideTessFactor : SV_InsideTessFactor;
};

struct HS_OUTPUT
{
	float4 worldspacePosition                                : HSO;
};

struct DS_OUTPUT
{
	float4 clipSpacePosition                                 : SV_Position;
	centroid float4 UVForCascade01                           : TEXCOORD0;
	centroid float4 UVForCascade23                           : TEXCOORD1;
	centroid float2 UVForLocalWaves                          : TEXCOORD2;
	centroid float4 cascadeBlendingFactors                   : TEXCOORD3;
    float3 worldspacePositionDisplaced                       : TEXCOORD4;
    float3 worldspacePositionUndisplaced                     : TEXCOORD5;
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

Texture2DArray g_displacementTextureArrayWindWaves  : WW_REGISTER(t0);
Texture2D      g_displacementTextureLocalWaves      : WW_REGISTER(t1);
SamplerState   g_bilinearSampler					: WW_REGISTER(s0);
SamplerState   g_bilinearSamplerClamp				: WW_REGISTER(s1);


// ---------------------------------------------------------------------------
// Helper functions
// ---------------------------------------------------------------------------
DS_OUTPUT GFSDK_WaveWorks_GetDisplacedVertexAfterTessellation(float4 In0, float4 In1, float4 In2, float3 BarycentricCoords)
{
	// Get starting position
	precise float3 tessellatedWorldspacePosition = In0.xyz * BarycentricCoords.x + In1.xyz * BarycentricCoords.y + In2.xyz * BarycentricCoords.z;
	float3 worldspacePositionUndisplaced = tessellatedWorldspacePosition;
	
	// Blending factors for cascades	
	float distance = length(g_eyePos - worldspacePositionUndisplaced);
	float4 cascadeWordldspaceSizes = float4(1.0 / g_cascade0UVScale, 1.0 / g_cascade1UVScale, 1.0 / g_cascade2UVScale, 1.0 / g_cascade3UVScale);
	float4 cascadeBlendingFactors = float4 (1.0, cascadeBlendingFactors.yzw = saturate(0.033 * (cascadeWordldspaceSizes.yzw*30.0 - distance) / cascadeWordldspaceSizes.yzw));

	// UVs
	float2 UVForCascade0 = worldspacePositionUndisplaced.xy * g_cascade0UVScale + g_cascade0UVOffset.xx;
	float2 UVForCascade1 = worldspacePositionUndisplaced.xy * g_cascade1UVScale + g_cascade1UVOffset.xx;
	float2 UVForCascade2 = worldspacePositionUndisplaced.xy * g_cascade2UVScale + g_cascade2UVOffset.xx;
	float2 UVForCascade3 = worldspacePositionUndisplaced.xy * g_cascade3UVScale + g_cascade3UVOffset.xx;
	float2 UVForLocalWaves = float2(0.5, 0.5) + (worldspacePositionUndisplaced.xy - g_localWavesSimulationDomainWorldspaceCenter) / g_localWavesSimulationDomainWorldspaceSize;

	UVForCascade0 += float2(g_UVWarpingAmplitude*cos(UVForCascade0.y*g_UVWarpingFrequency), g_UVWarpingAmplitude*sin(UVForCascade0.x*g_UVWarpingFrequency));
	UVForCascade1 += float2(g_UVWarpingAmplitude*cos(UVForCascade1.y*g_UVWarpingFrequency), g_UVWarpingAmplitude*sin(UVForCascade1.x*g_UVWarpingFrequency));
	UVForCascade2 += float2(g_UVWarpingAmplitude*cos(UVForCascade2.y*g_UVWarpingFrequency), g_UVWarpingAmplitude*sin(UVForCascade2.x*g_UVWarpingFrequency));
	UVForCascade3 += float2(g_UVWarpingAmplitude*cos(UVForCascade3.y*g_UVWarpingFrequency), g_UVWarpingAmplitude*sin(UVForCascade3.x*g_UVWarpingFrequency));

	// displacements
	float3 displacement;
	displacement =  cascadeBlendingFactors.x                                               * g_displacementTextureArrayWindWaves.SampleLevel(g_bilinearSampler, float3(UVForCascade0, 0.0), 0.0).xyz;
	displacement += cascadeBlendingFactors.y==0 ? float3(0,0,0) : cascadeBlendingFactors.y * g_displacementTextureArrayWindWaves.SampleLevel(g_bilinearSampler, float3(UVForCascade1, 1.0), 0.0).xyz;
	displacement += cascadeBlendingFactors.z==0 ? float3(0,0,0) : cascadeBlendingFactors.z * g_displacementTextureArrayWindWaves.SampleLevel(g_bilinearSampler, float3(UVForCascade2, 2.0), 0.0).xyz;
	displacement += cascadeBlendingFactors.w==0 ? float3(0,0,0) : cascadeBlendingFactors.w * g_displacementTextureArrayWindWaves.SampleLevel(g_bilinearSampler, float3(UVForCascade3, 3.0), 0.0).xyz;
	displacement += g_displacementTextureLocalWaves.SampleLevel(g_bilinearSamplerClamp, UVForLocalWaves, 0.0).xyz;
	float3 worldspacePositionDisplaced = worldspacePositionUndisplaced + displacement;
	
	// Output
	DS_OUTPUT Output;
	Output.UVForCascade01.xy = UVForCascade0;
	Output.UVForCascade01.zw = UVForCascade1;
	Output.UVForCascade23.xy = UVForCascade2;
	Output.UVForCascade23.zw = UVForCascade3;
	Output.UVForLocalWaves = UVForLocalWaves;
	Output.cascadeBlendingFactors = cascadeBlendingFactors;
	Output.worldspacePositionDisplaced = worldspacePositionDisplaced;
	Output.worldspacePositionUndisplaced = worldspacePositionUndisplaced;
	
	// Output.clipSpacePosition is not initialized here
	return Output; 
}


// ---------------------------------------------------------------------------
// Domain shader
// ---------------------------------------------------------------------------
[domain("tri")]
DS_OUTPUT main( HS_CONSTANTOUTPUT HSConstantData, const OutputPatch<HS_OUTPUT, 3> I, float3 f3BarycentricCoords : SV_DomainLocation )
{
	DS_OUTPUT Output = GFSDK_WaveWorks_GetDisplacedVertexAfterTessellation(I[0].worldspacePosition, I[1].worldspacePosition, I[2].worldspacePosition, f3BarycentricCoords);
	Output.clipSpacePosition = mul(g_matViewProj, float4(Output.worldspacePositionDisplaced, 1.0));
	return Output;
}
