// ComputePhaseLookup_PS.hlsl
//
// Builds the 1x512 phase function lookup texture ("NvVl::Phase LUT", R16G16B16A16_FLOAT)
// (ContextImp_D3D11::BeginAccumulation_UpdateMediumLUT, fullscreen pass, viewport 1x512).
// Row v of the LUT stores the density-weighted sum of the medium's phase terms for the
// scattering angle theta = PI * v, normalized by the total density.
// Blob: table ps_ComputePhaseLookup @ 0x1801FF2E0 (1 entry)

#include "ShaderCommon.hlsli"
#include "PostProcess.hlsli"

static const float PI = 3.1415926535f;
static const float INV_4PI = 0.0795774715f;

// Phase functions (normalized over the sphere). The constants/forms are taken verbatim from the blob:
// Rayleigh 3/(16pi)(1+cos^2) with an extra (1 - cos^4/8) factor, Mie hazy/murky with x = ((1-cos)/2)^8 / ^32.
float PhaseRayleigh(float cosTheta)
{
    float cos2 = cosTheta * cosTheta;
    return (3.0f / (16.0f * PI)) * (1.0f + cos2) * (1.0f - 0.125f * cos2 * cos2);
}

float PhaseMie(float x, float scale)
{
    return (0.5f + scale * x) * INV_4PI * (1.0f - 0.5f * x);
}

float PhaseHG(float cosTheta, float g)
{
    // cosTheta is negated: the LUT angle is measured from the light direction
    float c = -cosTheta;
    float g2 = g * g;
    float k = (1.0f - abs(g)) / sqrt(max(g2 - 2.0f * g * c + 1.0f, 0.0f));
    return (g2 + (1.0f - g2) * INV_4PI) * (k * k * k);
}

float4 main(PS_QUAD_INPUT input) : SV_TARGET
{
    float cosTheta = cos(PI * input.vTex.y);

    float phaseRayleigh = PhaseRayleigh(cosTheta);
    float x = 0.5f * (1.0f - cosTheta);
    float x8 = x * x;
    x8 = x8 * x8;
    x8 = x8 * x8;
    float phaseHazy = PhaseMie(x8, 4.5f);
    float x32 = x8 * x8;
    x32 = x32 * x32;
    float phaseMurky = PhaseMie(x32, 16.5f);

    float3 totalPhase = float3(0, 0, 0);
    float3 totalDensity = float3(0, 0, 0);
    for (uint i = 0; i < g_uNumPhaseTerms; ++i)
    {
        totalDensity += g_vPhaseParams[i].xyz;
        uint func = g_uPhaseFunc[i];
        if (func == PHASEFUNC_ISOTROPIC)
            totalPhase += g_vPhaseParams[i].xyz * INV_4PI;
        else if (func == PHASEFUNC_RAYLEIGH)
            totalPhase += g_vPhaseParams[i].xyz * phaseRayleigh;
        else if (func == PHASEFUNC_HG)
            totalPhase += g_vPhaseParams[i].xyz * PhaseHG(cosTheta, g_vPhaseParams[i].w);
        else if (func == PHASEFUNC_MIE_HAZY)
            totalPhase += g_vPhaseParams[i].xyz * phaseHazy;
        else [flatten] if (func == PHASEFUNC_MIE_MURKY) // the blob selects this last term with movc
            totalPhase += g_vPhaseParams[i].xyz * phaseMurky;
    }
    return float4(totalPhase / totalDensity, 1);
}
