// RenderVolume_PS.hlsl
//
// Light volume pixel shader: analytic in-scattering integral along the view ray, accumulated
// additively (bs_Additive_) into the "NvVl::Accumulation" target.
// Callers: ContextImp_D3D11::RenderVolume_DoVolume_Directional / _Spotlight / _Omni
// Table:   ps_RenderVolume @ 0x1801FE2E0 (512 entries, 162 used, 28 unique blobs)
//          index = SAMPLEMODE | LIGHTMODE << 1 | PASSMODE << 3 | ATTENUATIONMODE << 5 | FALLOFFMODE << 7
//
// Passes (PASSMODE):
//   GEOMETRY: rasterized light volume (shadow-mapped mesh from VS/HS/DS). Each face adds the integral
//             from the eye to the face, signed: front faces subtract, back faces add, so the sum
//             over the volume yields the lit segments of the ray (depth tested against the scene).
//   SKY:      fullscreen pass for pixels whose volume extends past the far plane (stencil marked);
//             the ray length is g_fZFar.
//   FINAL:    fullscreen pass closing the volume at the scene depth (ray length = distance to the
//             reconstructed scene position), stencil selects pixels still inside the volume.
// SAMPLEMODE only matters for FINAL (Texture2DMS scene depth, per-sample execution).
// ATTENUATIONMODE / FALLOFFMODE only matter for the spotlight: for omni lights and spotlights with
// NONE/FIXED falloff the attenuation is baked into the light LUTs (cs_ComputeLightLUT).

#include "ShaderCommon.hlsli"
#include "Quad.hlsli"

#define PASSMODE_GEOMETRY 0
#define PASSMODE_SKY 1
#define PASSMODE_FINAL 2

#if (SAMPLEMODE == SAMPLEMODE_MSAA)
Texture2DMS<float> tSceneDepth : register(t2);
#else
Texture2D<float> tSceneDepth : register(t2);
#endif
Texture2D<float4> tPhaseLUT : register(t4);
Texture2D<float4> tLightLUT_P : register(t5);
Texture2D<float4> tLightLUT_S1 : register(t6);
Texture2D<float4> tLightLUT_S2 : register(t7);

static const float PI = 3.1415926535897932384626433832795f;

struct PS_POLYGONAL_INPUT
{
    float4 vPos : SV_POSITION;
    float4 vWorldPos : TEXCOORD0;
};

// Phase function lookup: the phase LUT (ps_ComputePhaseLookup) is indexed by theta / PI.
float3 GetPhaseFactor(Texture2D<float4> tex, float cos_theta)
{
    float2 tc;
    tc.x = 0;
    tc.y = acos(clamp(cos_theta, -1, 1)) / PI;
    return g_vScatterPower * tex.SampleLevel(sBilinear, tc, 0).rgb;
}

// Directional light (unshadowed segment): integral of phase * exp(-sigma_t * s) ds over [0, eye_dist]
float3 Integrate_SimpleDirectional(float eye_dist, float3 vV, float3 vL)
{
    float3 vScatter = 1 - exp(-g_vSigmaExtinction * eye_dist);
    float3 vPhase = GetPhaseFactor(tPhaseLUT, dot(vV, vL));
    return vPhase * vScatter / g_vSigmaExtinction;
}

// Light LUT lookup (omni / spotlight): x = normalized distance along the ray relative to the light's
// [fLightDist - zfar, fLightDist + zfar] range, y = angle between the ray and the light-to-eye vector.
// The LUT stores rgb * a (a = scale) of the pre-integrated in-scattering.
float3 SampleLightLUT(Texture2D<float4> tex, float fDist, float fLightDist, float fCosTheta)
{
    float fDistOffset = max(fLightDist - g_fLightZFar, 0);
    float2 tc;
    tc.y = acos(-fCosTheta) / PI;
    tc.x = (fDist - fDistOffset) / (fLightDist + g_fLightZFar - fDistOffset);
    float4 vLUT = tex.SampleLevel(sBilinear, tc, 0);
    return vLUT.rgb * vLUT.a;
}

float3 Integrate_Omni(float eye_dist, float3 vV)
{
    float3 vLightToEye = g_vEyePosition - g_vLightPos;
    float fLightDist = length(vLightToEye);
    vLightToEye = vLightToEye / fLightDist;
    return g_vScatterPower * SampleLightLUT(tLightLUT_P, eye_dist, fLightDist, dot(vLightToEye, vV));
}

// Intersect the view ray with the (double) cone of the spotlight.
// Returns true if the ray segment [fStart, fEnd] lies inside the lit cone.
bool IntersectCone(out float fStart, out float fEnd, float3 vRayDir, float fRayLength, float3 vLightToEye, float fEyeAxis, float fRayAxis)
{
    float fCos2 = g_fLightFalloffAngle * g_fLightFalloffAngle;
    float fSin2 = 1 - g_fLightFalloffAngle * g_fLightFalloffAngle;
    float3 vRayPerp = vRayDir - fRayAxis * g_vLightDir;
    float3 vEyePerp = vLightToEye - fEyeAxis * g_vLightDir;
    float a = fCos2 * dot(vRayPerp, vRayPerp) - (fRayAxis * fRayAxis) * fSin2;
    float b = 2 * fCos2 * dot(vRayPerp, vEyePerp) - 2 * fRayAxis * fSin2 * fEyeAxis;
    float c = fCos2 * dot(vEyePerp, vEyePerp) - (fEyeAxis * fEyeAxis) * fSin2;
    float fDiscriminant = b * b - 4 * a * c;

    fStart = 0;
    fEnd = 0;
    bool bHit = false;
    if (fDiscriminant < 0 || a == 0)
    {
        bHit = false;
    }
    else
    {
        float f2A = 2 * a;
        float fSqrtDisc = sqrt(fDiscriminant);
        float t0 = (-b - fSqrtDisc) / f2A;
        float t1 = (-b + fSqrtDisc) / f2A;
        float fLightDist = length(vLightToEye);
        float fEyeCos = (fLightDist > 0) ? fEyeAxis / fLightDist : 1;
        if (fEyeCos >= g_fLightFalloffAngle)
        {
            // Eye inside the cone
            fStart = 0;
            fEnd = (fRayAxis >= g_fLightFalloffAngle) ? fRayLength : t1;
            bHit = true;
        }
        else if (fEyeCos <= -g_fLightFalloffAngle)
        {
            // Eye inside the reflected (back) cone
            fStart = t0;
            fEnd = fRayLength;
            bHit = !(t0 < 0 && t1 > 0);
        }
        else if (t0 < 0 && t1 < 0)
        {
            fStart = t0;
            fEnd = t1;
            bHit = false;
        }
        else
        {
            fStart = t0;
            float3 vP0 = t0 * vRayDir + vLightToEye;
            if (dot(g_vLightDir, vP0) < 0)
            {
                // First intersection is with the back cone
                fEnd = t1;
                bHit = false;
            }
            else
            {
                fEnd = (t1 < 0) ? fRayLength : t1;
                bHit = true;
            }
        }
        if (fRayLength < fStart)
        {
            fStart = 0;
            fEnd = 0;
            bHit = false;
        }
    }
    return bHit;
}

float3 SampleLUT(Texture2D<float4> tex, float2 tc)
{
    float4 vLUT = tex.SampleLevel(sBilinear, tc, 0);
    return vLUT.rgb * vLUT.a;
}

// Fixed (linear) angular falloff: f(p) = (dot(p - L, D) / |p - L| - cos_a) / (1 - cos_a)
// with dot(p - L, D) = fEyeAxis + t * fRayAxis; the LUTs hold the matching integrals.
float3 EvaluateSpotLUT(float2 tc, float fEyeAxis, float fRayAxis)
{
    return fEyeAxis * SampleLUT(tLightLUT_S1, tc) + fRayAxis * SampleLUT(tLightLUT_S2, tc) - g_fLightFalloffAngle * SampleLUT(tLightLUT_P, tc);
}

// In-scattered light at distance t along the view ray (custom falloff: evaluated analytically per sample)
float3 Integrand_CustomSpotlight(float t, float fLightDist2, float fEyeRayDot, float fEyeAxis, float fRayAxis)
{
    float fDist2 = max(fLightDist2 + 2 * fEyeRayDot * t + t * t, 0);
    float fDist = sqrt(fDist2);
    // Scattering angle (law of cosines in the eye / light / sample triangle)
    float fCosTheta = (t > 0 && fDist > 0) ? (t * t + fDist2 - fLightDist2) / (2 * t * fDist) : 0;
    float3 vPhase = GetPhaseFactor(tPhaseLUT, fCosTheta);
#if (ATTENUATIONMODE == ATTENUATIONMODE_POLYNOMIAL)
    float fAttenuation = saturate(1 - (g_vLightAttenuationFactors.x + g_vLightAttenuationFactors.y * fDist + fDist2 * g_vLightAttenuationFactors.z));
#elif (ATTENUATIONMODE == ATTENUATIONMODE_INV_POLYNOMIAL)
    float fAttenuation = saturate(1 / (g_vLightAttenuationFactors.x + g_vLightAttenuationFactors.y * fDist + fDist2 * g_vLightAttenuationFactors.z) + g_vLightAttenuationFactors.w);
#else
    float fAttenuation = 1;
#endif
    // Angular falloff
    float fSpotCos = (fDist > 0) ? (t * fRayAxis + fEyeAxis) / fDist : 1;
    float fFalloff = saturate(fSpotCos - g_fLightFalloffAngle) / (1 - g_fLightFalloffAngle);
    fFalloff = (fFalloff > 0.000001f) ? pow(abs(fFalloff), g_fLightFalloffPower) : 0;
    float3 vExtinction = exp(-g_vSigmaExtinction * (t + fDist));
    return (vPhase * fAttenuation * fFalloff) * vExtinction;
}

float3 Integrate_Spotlight(float eye_dist, float3 vV)
{
    float3 vLightToEye = g_vEyePosition - g_vLightPos;
    float fEyeAxis = dot(vLightToEye, g_vLightDir);
    float fRayAxis = dot(vV, g_vLightDir);
    float fStart, fEnd;
    if (IntersectCone(fStart, fEnd, vV, eye_dist, vLightToEye, fEyeAxis, fRayAxis))
    {
        fEnd = min(eye_dist, fEnd);
#if (FALLOFFMODE == FALLOFFMODE_CUSTOM)
        // Numerical integration (Simpson's rule, 8 intervals) of the in-scattering along [fStart, fEnd]
        float fLightDist2 = dot(vLightToEye, vLightToEye);
        float fEyeRayDot = dot(vLightToEye, vV);
        float fSegment = fEnd - fStart;
        float fStepSize = fSegment / 8;
        float t = fStart;
        float3 vIntegral = Integrand_CustomSpotlight(t, fLightDist2, fEyeRayDot, fEyeAxis, fRayAxis);
        [unroll]
        for (uint i = 1; i < 8; ++i)
        {
            t += fStepSize;
            vIntegral += ((i & 1) ? 4 : 2) * Integrand_CustomSpotlight(t, fLightDist2, fEyeRayDot, fEyeAxis, fRayAxis);
        }
        vIntegral += Integrand_CustomSpotlight(fEnd, fLightDist2, fEyeRayDot, fEyeAxis, fRayAxis);
        return (fStepSize / 3) * vIntegral * 6;
#else
        float fLightDist = length(vLightToEye);
        float3 vL = vLightToEye / fLightDist;
        float fDistOffset = max(fLightDist - g_fLightZFar, 0);
        float fDistRange = fLightDist + g_fLightZFar - fDistOffset;
        float2 tc;
        tc.x = (fEnd - fDistOffset) / fDistRange;
        tc.y = acos(-dot(vL, vV)) / PI;
#if (FALLOFFMODE == FALLOFFMODE_FIXED)
        float3 vIntegral = EvaluateSpotLUT(tc, fEyeAxis, fRayAxis);
        if (fStart > 0)
        {
            tc.x = (fStart - fDistOffset) / fDistRange;
            vIntegral -= EvaluateSpotLUT(tc, fEyeAxis, fRayAxis);
        }
        return vIntegral * (g_vScatterPower / (1 - g_fLightFalloffAngle));
#else
        float4 vLUT = tLightLUT_P.SampleLevel(sBilinear, tc, 0);
        float3 vIntegral = vLUT.rgb * vLUT.a;
        if (fStart > 0)
        {
            tc.x = (fStart - fDistOffset) / fDistRange;
            vLUT = tLightLUT_P.SampleLevel(sBilinear, tc, 0);
            vIntegral -= vLUT.rgb * vLUT.a;
        }
        return vIntegral * g_vScatterPower;
#endif
#endif
    }
    else
    {
        return 0;
    }
}

#if (PASSMODE == PASSMODE_FINAL)
float4 main(VS_QUAD_OUTPUT input, uint uSampleID : SV_SAMPLEINDEX, bool bIsFrontFace : SV_ISFRONTFACE) : SV_TARGET
#else
float4 main(PS_POLYGONAL_INPUT input, bool bIsFrontFace : SV_ISFRONTFACE) : SV_TARGET
#endif
{
#if (PASSMODE == PASSMODE_FINAL)
#if (SAMPLEMODE == SAMPLEMODE_MSAA)
    float fSceneDepth = tSceneDepth.Load(uint2(input.vPos.xy), uSampleID);
#else
    float fSceneDepth = tSceneDepth.Load(uint3(input.vPos.xy, 0));
#endif
    float2 vClip = input.vPos.xy * g_vViewportSize_Inv * float2(2, -2) + float2(-1, 1);
    float4 vWorldPos = mul(g_mViewProjInv, float4(vClip, fSceneDepth, 1));
    float3 vWorld = vWorldPos.xyz * (1.0f / vWorldPos.w);
#else
    float3 vWorld = input.vWorldPos.xyz;
#endif

#if (LIGHTMODE == LIGHTMODE_DIRECTIONAL)
    float3 vIntegral;
#if (PASSMODE == PASSMODE_SKY)
    float3 vRayDir = normalize(vWorld - g_vEyePosition);
    vIntegral = Integrate_SimpleDirectional(g_fZFar, vRayDir, g_vLightDir);
#else
    float3 vRay = vWorld - g_vEyePosition;
    float fRayLength = length(vRay);
    vIntegral = Integrate_SimpleDirectional(fRayLength, vRay / fRayLength, g_vLightDir);
#endif
#if (PASSMODE == PASSMODE_GEOMETRY)
    vIntegral = ((bIsFrontFace) ? -1 : 1) * vIntegral;
#endif
    return float4(vIntegral * g_vLightIntensity, 0);
#elif (LIGHTMODE == LIGHTMODE_OMNI)
    float3 vIntegral;
#if (PASSMODE == PASSMODE_SKY)
    float3 vRayDir = normalize(vWorld - g_vEyePosition);
    vIntegral = Integrate_Omni(g_fZFar, vRayDir);
#else
    float3 vRay = vWorld - g_vEyePosition;
    float fRayLength = length(vRay);
    vIntegral = Integrate_Omni(fRayLength, vRay / fRayLength);
#endif
#if (PASSMODE == PASSMODE_GEOMETRY)
    vIntegral = ((bIsFrontFace) ? -1 : 1) * vIntegral;
#endif
    return float4(vIntegral * g_vLightIntensity, 0);
#elif (LIGHTMODE == LIGHTMODE_SPOTLIGHT)
#if (PASSMODE == PASSMODE_GEOMETRY)
    float fSign = (bIsFrontFace) ? -1 : 1;
#endif
    float3 vIntegral;
#if (PASSMODE == PASSMODE_SKY)
    float3 vRayDir = normalize(vWorld - g_vEyePosition);
    vIntegral = Integrate_Spotlight(g_fZFar, vRayDir);
#else
    float3 vRay = vWorld - g_vEyePosition;
    float fRayLength = length(vRay);
    vIntegral = Integrate_Spotlight(fRayLength, vRay / fRayLength);
#endif
#if (PASSMODE == PASSMODE_GEOMETRY)
    vIntegral = fSign * vIntegral;
#endif
    return float4(vIntegral * g_vLightIntensity, 0);
#endif
}
