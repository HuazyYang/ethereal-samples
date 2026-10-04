// ShaderCommon.hlsli
//
// Shared declarations of the NvVolumetricLighting D3D11 shaders.
// Reconstructed from the reflection data (RDEF) of the blobs embedded in
// NvVolumetricLighting.d3d11.dll. Layouts match the C++ structures written by
// ContextImp::SetupCB_PerContext / PerFrame / PerVolume / PerApply.
//
// All shaders are compiled with: fxc /nologo /WX /T <profile> /E main [/D AXIS=n ...]

#ifndef NVVL_SHADER_COMMON_HLSLI
#define NVVL_SHADER_COMMON_HLSLI

////////////////////////////////////////////////////////////////////////////////
// Constant buffers (slots fixed by the C++ side: b0..b3).
// Members carry explicit packoffsets: besides documenting the layout, this matters for code
// generation (fxc emits "add r, l(1.0), -cb[i].x" for 1 - member, as in the original blobs, only for
// packoffset-declared members).

cbuffer cbContext : register(b0)
{
    float2 g_vOutputSize : packoffset(c0.x);
    float2 g_vOutputSize_Inv : packoffset(c0.z);
    float2 g_vBufferSize : packoffset(c1.x);
    float2 g_vBufferSize_Inv : packoffset(c1.z);
    float g_fResMultiplier : packoffset(c2.x);
    uint g_uBufferSamples : packoffset(c2.y);
};

cbuffer cbFrame : register(b1)
{
    float4x4 g_mProj : packoffset(c0);
    float4x4 g_mViewProj : packoffset(c4);
    float4x4 g_mViewProjInv : packoffset(c8);
    float2 g_vOutputViewportSize : packoffset(c12.x);
    float2 g_vOutputViewportSize_Inv : packoffset(c12.z);
    float2 g_vViewportSize : packoffset(c13.x);
    float2 g_vViewportSize_Inv : packoffset(c13.z);
    float3 g_vEyePosition : packoffset(c14.x);
    float2 g_vJitterOffset : packoffset(c15.x);
    float g_fZNear : packoffset(c15.z);
    float g_fZFar : packoffset(c15.w);
    float3 g_vScatterPower : packoffset(c16.x);
    uint g_uNumPhaseTerms : packoffset(c16.w);
    float3 g_vSigmaExtinction : packoffset(c17.x);
    uint g_uPhaseFunc[4] : packoffset(c18);
    float4 g_vPhaseParams[4] : packoffset(c22);
};

cbuffer cbVolume : register(b2)
{
    float4x4 g_mLightToWorld : packoffset(c0);
    float g_fLightFalloffAngle : packoffset(c4.x);
    float g_fLightFalloffPower : packoffset(c4.y);
    float g_fGridSectionSize : packoffset(c4.z);
    float g_fLightToEyeDepth : packoffset(c4.w);
    float g_fLightZNear : packoffset(c5.x);
    float g_fLightZFar : packoffset(c5.y);
    float4 g_vLightAttenuationFactors : packoffset(c6);
    float4x4 g_mLightProj[4] : packoffset(c7);
    float4x4 g_mLightProjInv[4] : packoffset(c23);
    float3 g_vLightDir : packoffset(c39.x);
    float g_fGodrayBias : packoffset(c39.w);
    float3 g_vLightPos : packoffset(c40.x);
    uint g_uMeshResolution : packoffset(c40.w);
    float3 g_vLightIntensity : packoffset(c41.x);
    float g_fTargetRaySize : packoffset(c41.w);
    float4 g_vElementOffsetAndScale[4] : packoffset(c42);
    float4 g_vShadowMapDim : packoffset(c46);
    uint g_uElementIndex[4] : packoffset(c47);
};

cbuffer cbApply : register(b3)
{
    float4x4 g_mHistoryXform : packoffset(c0);
    float g_fFilterThreshold : packoffset(c4.x);
    float g_fHistoryFactor : packoffset(c4.y);
    float3 g_vFogLight : packoffset(c5.x);
    float g_fMultiScattering : packoffset(c5.w);
};

////////////////////////////////////////////////////////////////////////////////
// Samplers (ContextImp_D3D11::ss_Point_ / ss_Linear_ bound to s0/s1)

SamplerState sPoint : register(s0);
SamplerState sBilinear : register(s1);

////////////////////////////////////////////////////////////////////////////////
// Shared enumerations (values match the C++ API enums / permutation indices)

#define SAMPLEMODE_SINGLE 0
#define SAMPLEMODE_MSAA 1

#define LIGHTMODE_DIRECTIONAL 0
#define LIGHTMODE_SPOTLIGHT 1
#define LIGHTMODE_OMNI 2

#define SHADOWMAPTYPE_ATLAS 0
#define SHADOWMAPTYPE_ARRAY 1

#define CASCADECOUNT_1 0
#define CASCADECOUNT_2 1
#define CASCADECOUNT_3 2
#define CASCADECOUNT_4 3

#define VOLUMETYPE_FRUSTUM 0
#define VOLUMETYPE_PARABOLOID 1

#define MAXTESSFACTOR_LOW 0
#define MAXTESSFACTOR_MEDIUM 1
#define MAXTESSFACTOR_HIGH 2

#define ATTENUATIONMODE_NONE 0
#define ATTENUATIONMODE_POLYNOMIAL 1
#define ATTENUATIONMODE_INV_POLYNOMIAL 2

#define FALLOFFMODE_NONE 0
#define FALLOFFMODE_FIXED 1
#define FALLOFFMODE_CUSTOM 2

#define PHASEFUNC_ISOTROPIC 0
#define PHASEFUNC_RAYLEIGH 1
#define PHASEFUNC_HG 2
#define PHASEFUNC_MIE_HAZY 3
#define PHASEFUNC_MIE_MURKY 4

#endif // NVVL_SHADER_COMMON_HLSLI
