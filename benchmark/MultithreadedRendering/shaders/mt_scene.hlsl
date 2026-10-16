//--------------------------------------------------------------------------------------
// File: mt_scene.hlsl
//
// Vertex and pixel shader of the MultithreadedRendering11 sample (MultithreadedRendering11_PS.hlsl),
// as used by D3D12-Trial/MultithreadedRendering. One source for both renderers: fxc (vs_5_0 /
// ps_5_0) for mt_rendering_native, ShaderTool / dxc (DXIL and SPIR-V) for mt_rendering_nvrhi.
// Matrices use the default (column-major) packing; the C++ side stores them transposed.
//
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License (MIT).
//--------------------------------------------------------------------------------------

#define SHADOW_DEPTH_BIAS 0.0005f

static const int g_iNumLights = 4;
static const int g_iNumShadows = 1; // by convention, the first n lights cast shadows

cbuffer cbPerObject : register(b0)
{
    float4x4 g_mWorld;
    float4   g_vObjectColor;
};

struct LightDataStruct
{
    float4x4 m_mLightViewProj;
    float4   m_vLightPos;
    float4   m_vLightDir;
    float4   m_vLightColor;
    float4   m_vFalloffs;    // x = dist end, y = dist range, z = cos angle end, w = cos range
};

cbuffer cbPerLight : register(b1)
{
    LightDataStruct g_LightData[g_iNumLights];
};

cbuffer cbPerScene : register(b2)
{
    float4x4 g_mViewProj;
    float4   g_vMirrorPlane;
    float4   g_vAmbientColor;
    float4   g_vTintColor;
};

// The two material textures are nvrhi binding layout 1. On Vulkan a binding layout is a descriptor set, and
// DXC maps register spaces to descriptor sets, so they live in space1 for the DXIL / SPIR-V builds. fxc
// (SM 5.0, the native renderer) has no register spaces; its root signature puts t0/t1 in space 0.
#if defined(TARGET_D3D12) || defined(TARGET_VULKAN) || defined(SPIRV)
#define MATERIAL_REGISTER(reg) register(reg, space1)
#else
#define MATERIAL_REGISTER(reg) register(reg)
#endif
Texture2D g_txDiffuse               : MATERIAL_REGISTER(t0);
Texture2D g_txNormal                : MATERIAL_REGISTER(t1);
Texture2D g_txShadow[g_iNumShadows] : register(t2);

SamplerState g_samPointClamp  : register(s0);
SamplerState g_samLinearWrap  : register(s1);

struct VS_INPUT
{
    float4 vPosition : POSITION;
    float3 vNormal   : NORMAL;
    float2 vTexcoord : TEXCOORD0;
    float3 vTangent  : TANGENT;
};

struct VS_OUTPUT
{
    float3 vNormal   : NORMAL;
    float3 vTangent  : TANGENT;
    float2 vTexcoord : TEXCOORD0;
    float4 vPosWorld : TEXCOORD1;
    float4 vPosition : SV_POSITION;
};

// The sdkmesh stores normals and tangents as DEC3N, which the input layout aliases as
// R10G10B10A2_UNORM: recover the signed values.
float3 R10G10B10A2_UNORM_TO_R32G32B32_FLOAT(in float3 vVec)
{
    vVec *= 2.0f;
    return vVec - 2.0f * step(1.0f, vVec); // vVec >= 1 ? vVec - 2 : vVec, without a vector ternary (dxc)
}

VS_OUTPUT VSMain(VS_INPUT Input)
{
    VS_OUTPUT Output;

    Input.vNormal = R10G10B10A2_UNORM_TO_R32G32B32_FLOAT(Input.vNormal);
    Input.vTangent = R10G10B10A2_UNORM_TO_R32G32B32_FLOAT(Input.vTangent);

    Output.vPosWorld = mul(Input.vPosition, g_mWorld);
    Output.vPosition = mul(Output.vPosWorld, g_mViewProj);
    Output.vNormal   = mul(Input.vNormal, (float3x3)g_mWorld);
    Output.vTangent  = mul(Input.vTangent, (float3x3)g_mWorld);
    Output.vTexcoord = Input.vTexcoord;

    return Output;
}

struct PS_INPUT
{
    float3 vNormal   : NORMAL;
    float3 vTangent  : TANGENT;
    float2 vTexcoord : TEXCOORD0;
    float4 vPosWorld : TEXCOORD1;
};

float3 CalcPerPixelNormal(float2 vTexcoord, float3 vVertNormal, float3 vVertTangent)
{
    vVertNormal = normalize(vVertNormal);
    vVertTangent = normalize(vVertTangent);
    float3 vVertBinormal = normalize(cross(vVertTangent, vVertNormal));
    float3x3 mTangentSpaceToWorldSpace = float3x3(vVertTangent, vVertBinormal, vVertNormal);

    float3 vBumpNormal = g_txNormal.Sample(g_samLinearWrap, vTexcoord).xyz;
    vBumpNormal = 2.0f * vBumpNormal - 1.0f;

    return mul(vBumpNormal, mTangentSpaceToWorldSpace);
}

// 2x2 percentage-closer filtering against shadow map iShadow
float4 CalcUnshadowedAmountPCF2x2(int iShadow, float4 vPosWorld)
{
    float4x4 mLightViewProj = g_LightData[iShadow].m_mLightViewProj;

    float4 vLightSpacePos = mul(vPosWorld, mLightViewProj);
    vLightSpacePos.xyz /= vLightSpacePos.w;

    float2 vShadowTexCoord = 0.5f * vLightSpacePos.xy + 0.5f;
    vShadowTexCoord.y = 1.0f - vShadowTexCoord.y;

    float vLightSpaceDepth = vLightSpacePos.z - SHADOW_DEPTH_BIAS;

    float2 vShadowMapDims = float2(2048.0f, 2048.0f); // keep in sync with MtScene::kShadowMapSize
    float4 vSubPixelCoords;
    vSubPixelCoords.xy = frac(vShadowMapDims * vShadowTexCoord);
    vSubPixelCoords.zw = 1.0f - vSubPixelCoords.xy;
    float4 vBilinearWeights = vSubPixelCoords.zxzx * vSubPixelCoords.wwyy;

    float2 vTexelUnits = 1.0f / vShadowMapDims;
    float4 vShadowDepths;
    vShadowDepths.x = g_txShadow[iShadow].Sample(g_samPointClamp, vShadowTexCoord).x;
    vShadowDepths.y = g_txShadow[iShadow].Sample(g_samPointClamp, vShadowTexCoord + float2(vTexelUnits.x, 0.0f)).x;
    vShadowDepths.z = g_txShadow[iShadow].Sample(g_samPointClamp, vShadowTexCoord + float2(0.0f, vTexelUnits.y)).x;
    vShadowDepths.w = g_txShadow[iShadow].Sample(g_samPointClamp, vShadowTexCoord + vTexelUnits).x;

    float4 vShadowTests = step(vLightSpaceDepth, vShadowDepths); // depth >= pixel depth ? 1 : 0
    return dot(vBilinearWeights, vShadowTests);
}

float4 CalcLightingColor(int iLight, float3 vPosWorld, float3 vPerPixelNormal)
{
    float3 vLightPos   = g_LightData[iLight].m_vLightPos.xyz;
    float3 vLightDir   = g_LightData[iLight].m_vLightDir.xyz;
    float4 vLightColor = g_LightData[iLight].m_vLightColor;
    float4 vFalloffs   = g_LightData[iLight].m_vFalloffs;

    float3 vLightToPixelUnNormalized = vPosWorld - vLightPos;

    float fDist = length(vLightToPixelUnNormalized);
    float fDistFalloff = saturate((vFalloffs.x - fDist) / vFalloffs.y);

    float3 vLightToPixelNormalized = vLightToPixelUnNormalized / fDist;

    float fCosAngle = dot(vLightToPixelNormalized, vLightDir);
    float fAngleFalloff = saturate((fCosAngle - vFalloffs.z) / vFalloffs.w);

    float fNDotL = saturate(-dot(vLightToPixelNormalized, vPerPixelNormal));

    return vLightColor * fNDotL * fDistFalloff * fAngleFalloff;
}

float4 PSMain(PS_INPUT Input) : SV_TARGET
{
    // Manual clip test, so that objects which are behind the mirror don't show up in the mirror.
    clip(dot(g_vMirrorPlane.xyz, Input.vPosWorld.xyz) + g_vMirrorPlane.w);

    float4 vDiffuse = g_txDiffuse.Sample(g_samLinearWrap, Input.vTexcoord);

    float3 vPerPixelNormal = CalcPerPixelNormal(Input.vTexcoord, Input.vNormal, Input.vTangent);

    float4 vTotalLightingColor = g_vAmbientColor;

    for (int iLight = 0; iLight < g_iNumLights; ++iLight)
    {
        float4 vLightingColor = CalcLightingColor(iLight, Input.vPosWorld.xyz, vPerPixelNormal);
        if (iLight < g_iNumShadows && any(vLightingColor.xyz)) // Don't bother checking shadow map if the pixel is unlit
        {
            vLightingColor *= CalcUnshadowedAmountPCF2x2(iLight, Input.vPosWorld);
        }
        vTotalLightingColor += vLightingColor;
    }

    return vDiffuse * g_vTintColor * g_vObjectColor * vTotalLightingColor;
}
