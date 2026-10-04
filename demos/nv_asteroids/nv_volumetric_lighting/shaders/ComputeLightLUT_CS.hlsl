// ComputeLightLUT_CS.hlsl
//
// Light scattering look-up tables for local lights (spotlight / omni).
// Called from ContextImp_D3D11::RenderVolume_DoVolume_Spotlight (0x18000A8D0) and
// ContextImp_D3D11::RenderVolume_DoVolume_Omni (0x18000B700).
// Table: cs_ComputeLightLUT[16] @ 0x1801FE250 (sizes @ 0x1800AD750).
//
// Permutation axes (index = LIGHTMODE field | ATTENUATIONMODE << 1 | COMPUTEPASS << 3):
//   LIGHTMODE       LIGHTMODE_OMNI (field 0): only the point LUT (omni lights, spotlights without falloff)
//                   LIGHTMODE_SPOTLIGHT (field 1): point LUT + the two spotlight moment LUTs (FIXED falloff)
//   ATTENUATIONMODE light distance attenuation (NONE / POLYNOMIAL / INV_POLYNOMIAL)
//   COMPUTEPASS     0: per-sample in-scattering + 32-wide prefix sum, Dispatch(8, 64, 1)
//                   1: carries the 32-wide partial sums along the row, Dispatch(1, 128, 1 or 3)
//
// LUT parameterisation (256 x 512, R16G16B16A16_FLOAT):
//   x: distance along the view ray, t = near + x / 127 * (far - near) with near/far being the
//      eye-to-light distance -/+ g_fLightZFar (near clamped to 0)
//   y: angle between the view ray and the eye->light direction, theta = y * PI / 511
// Each texel stores the in-scattered light (phase * transmittance * step) integrated from the
// near distance up to t; rgb is stored divided by a scale that is kept in alpha.

#include "ShaderCommon.hlsli"

#ifndef LIGHTMODE
#define LIGHTMODE LIGHTMODE_OMNI
#endif
#ifndef ATTENUATIONMODE
#define ATTENUATIONMODE ATTENUATIONMODE_NONE
#endif
#ifndef COMPUTEPASS
#define COMPUTEPASS 0
#endif

static const float PI = 3.1415926535897932384626433832795f;

// Scale applied to the stored rgb values (kept in alpha so pass 1 can undo it)
static const float LUT_SCALE = 1.0f / 1024.0f;

Texture2D<float4> tPhaseLUT : register(t4);
Texture2D<float4> tLightLUT_P : register(t5);
Texture2D<float4> tLightLUT_S1 : register(t6);
Texture2D<float4> tLightLUT_S2 : register(t7);

RWTexture2D<float4> rwLightLUT_P : register(u0);
RWTexture2D<float4> rwLightLUT_S1 : register(u1);
RWTexture2D<float4> rwLightLUT_S2 : register(u2);

#if (COMPUTEPASS == 0)

////////////////////////////////////////////////////////////////////////////////
// Pass 0: evaluate the in-scattering of one ray sample and prefix-sum it over
// the 32 threads of a row.

#define GROUP_SIZE_X 32
#define GROUP_SIZE_Y 8

groupshared float3 sLUT_P[GROUP_SIZE_X * GROUP_SIZE_Y];
#if (LIGHTMODE == LIGHTMODE_SPOTLIGHT)
groupshared float3 sLUT_S1[GROUP_SIZE_X * GROUP_SIZE_Y];
groupshared float3 sLUT_S2[GROUP_SIZE_X * GROUP_SIZE_Y];
#endif

float AttenuationFunc(float d)
{
#if (ATTENUATIONMODE == ATTENUATIONMODE_POLYNOMIAL)
    // 1 - (k0 + k1*d + k2*d^2)
    return saturate(1 - (g_vLightAttenuationFactors.x + g_vLightAttenuationFactors.y * d + g_vLightAttenuationFactors.z * d * d));
#elif (ATTENUATIONMODE == ATTENUATIONMODE_INV_POLYNOMIAL)
    // 1 / (k0 + k1*d + k2*d^2) + k3
    return saturate(1 / (g_vLightAttenuationFactors.x + g_vLightAttenuationFactors.y * d + g_vLightAttenuationFactors.z * d * d) + g_vLightAttenuationFactors.w);
#else
    return 1;
#endif
}

[numthreads(GROUP_SIZE_X, GROUP_SIZE_Y, 1)]
void main(uint3 gthreadID : SV_GroupThreadID, uint3 dthreadID : SV_DispatchThreadID)
{
    uint idx = gthreadID.y * GROUP_SIZE_X + gthreadID.x;

    float fDistanceCoord = float(dthreadID.x) / 127.0f;
    float fCosTheta = cos(float(dthreadID.y) * (PI / 511.0f));

    // Ray distance range covered by the light volume
    float3 vLightToEye = g_vEyePosition - g_vLightPos;
    float fLightToEyeDist2 = dot(vLightToEye, vLightToEye);
    float fLightToEyeDist = sqrt(fLightToEyeDist2);
    float fNear = max(fLightToEyeDist - g_fLightZFar, 0);
    float fFar = fLightToEyeDist + g_fLightZFar;
    float fRange = fFar - fNear;
    float fRayDist = fDistanceCoord * fRange + fNear;

    // Sample-to-light distance (law of cosines)
    float fLightDist2 = max(fLightToEyeDist2 + 2 * (-fCosTheta * fLightToEyeDist) * fRayDist + fRayDist * fRayDist, 0);
    float fLightDist = sqrt(fLightDist2);

    // Scattering angle at the sample
    float fCosPhi = ((fRayDist > 0) && (fLightDist > 0))
        ? ((fRayDist * fRayDist + fLightDist2 - fLightToEyeDist2) / (2 * fRayDist * fLightDist))
        : -fCosTheta;

    float3 vTransmittance = exp((fRayDist + fLightDist) * -g_vSigmaExtinction);
    float fPhaseCoord = acos(clamp(fCosPhi, -1, 1)) / PI;
    float3 vPhase = tPhaseLUT.SampleLevel(sBilinear, float2(0, fPhaseCoord), 0).rgb * g_vScatterPower;
    vPhase *= AttenuationFunc(fLightDist);
    float3 vInscatter = vPhase * vTransmittance * (fRange / 128.0f) / g_vScatterPower;

    sLUT_P[idx] = vInscatter;
#if (LIGHTMODE == LIGHTMODE_SPOTLIGHT)
    float3 vInscatterS1 = (fLightDist == 0) ? 0 : vInscatter / fLightDist;
    sLUT_S1[idx] = vInscatterS1;
    sLUT_S2[idx] = fRayDist * vInscatterS1;
#endif

    // Hillis-Steele prefix sum along the 32 threads of the row
    [unroll]
    for (uint i = 1; i < GROUP_SIZE_X; i *= 2)
    {
        if (gthreadID.x >= i)
        {
            sLUT_P[idx] = sLUT_P[idx - i] + sLUT_P[idx];
#if (LIGHTMODE == LIGHTMODE_SPOTLIGHT)
            sLUT_S1[idx] = sLUT_S1[idx - i] + sLUT_S1[idx];
            sLUT_S2[idx] = sLUT_S2[idx - i] + sLUT_S2[idx];
#endif
        }
    }

    rwLightLUT_P[dthreadID.xy] = float4(sLUT_P[idx] / LUT_SCALE, LUT_SCALE);
#if (LIGHTMODE == LIGHTMODE_SPOTLIGHT)
    rwLightLUT_S1[dthreadID.xy] = float4(sLUT_S1[idx] / LUT_SCALE, LUT_SCALE);
    float3 vSumS2 = sLUT_S2[idx];
    float fScaleS2 = fFar / 512.0f;
    rwLightLUT_S2[dthreadID.xy] = float4(vSumS2 / fScaleS2, fScaleS2);
#endif
}

#else // COMPUTEPASS == 1

////////////////////////////////////////////////////////////////////////////////
// Pass 1: propagate the running sum of each row across the 32-texel blocks
// written by pass 0. Spotlights process the three LUTs as SV_DispatchThreadID.z.

#define GROUP_SIZE_X 32
#define GROUP_SIZE_Y 4
#define BLOCK_COUNT 4

groupshared float3 sRowSum[GROUP_SIZE_Y];

float4 LoadLUT(uint2 coord, uint lut)
{
#if (LIGHTMODE == LIGHTMODE_SPOTLIGHT)
    if (lut == 2)
        return tLightLUT_S2[coord];
    else if (lut == 1)
        return tLightLUT_S1[coord];
    else
#endif
        return tLightLUT_P[coord];
}

void StoreLUT(uint2 coord, uint lut, float3 vSum, float fScale)
{
#if (LIGHTMODE == LIGHTMODE_SPOTLIGHT)
    if (lut == 2)
        rwLightLUT_S2[coord] = float4(vSum / fScale, fScale);
    else if (lut == 1)
        rwLightLUT_S1[coord] = float4(vSum / fScale, fScale);
    else
#endif
        rwLightLUT_P[coord] = float4(vSum / fScale, fScale);
}

[numthreads(GROUP_SIZE_X, GROUP_SIZE_Y, 1)]
void main(uint3 gthreadID : SV_GroupThreadID, uint3 dthreadID : SV_DispatchThreadID)
{
    if (gthreadID.x == 0)
        sRowSum[gthreadID.y] = 0;

    [unroll]
    for (uint i = 0; i < BLOCK_COUNT; ++i)
    {
        uint2 coord = dthreadID.xy + uint2(i * GROUP_SIZE_X, 0);
        float4 vValue = LoadLUT(coord, dthreadID.z);
        float3 vSum = vValue.rgb * vValue.a + sRowSum[gthreadID.y];
        if (gthreadID.x == GROUP_SIZE_X - 1)
            sRowSum[gthreadID.y] = vSum;
        float fScale = vValue.a * 4;
        StoreLUT(coord, dthreadID.z, vSum, fScale);
    }
}

#endif // COMPUTEPASS
