// Recovered from _EvolveInFrequencySpaceForLocalWavesCS_bytecode_dx.msasm.

cbuffer CS_CBUFFER : register(b0)
{
    uint g_simulationGridSize : packoffset(c0.x);
    uint g_32minusLog2Resolution : packoffset(c0.y);
    float g_simulationWorldspaceSize : packoffset(c0.z);
    float g_dT : packoffset(c0.w);
    float g_dampening : packoffset(c1.x);
    uint g_numDisturbanceSamples : packoffset(c1.y);
    float g_verticalScaler : packoffset(c1.z);
    float g_lateralScaler : packoffset(c1.w);
};

// (vertical displacement re, im, velocity potential re, im) per frequency.
RWStructuredBuffer<float4> g_verticalDisplacementAndVelocityPotentialFS : register(u0);
// (x displacement re, im, y displacement re, im) per frequency.
RWStructuredBuffer<float4> g_lateralDisplacementsFS : register(u1);

static const float kGravity = 9.81000041961669921875f;
static const float kSurfaceTension = 7.4000003223773092031478881835938e-05f;
static const float kTwoPi = 6.283185482025146484375f;

[numthreads(16, 16, 1)]
void EvolveInFrequencySpaceForLocalWavesCS(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint spectrumIndex = dispatchThreadID.y * g_simulationGridSize + dispatchThreadID.x;
    float4 spectrum = g_verticalDisplacementAndVelocityPotentialFS[spectrumIndex];

    float fundamentalWavenumber = kTwoPi / g_simulationWorldspaceSize;

    // Recentre the texel: the upper half of each axis carries the negative
    // frequencies. Spelled per component because a vector condition on a
    // ternary is legal under FXC but rejected by DXC 2021, and this source is
    // also compiled at cs_6_0 for the SPIR-V backend.
    uint halfGridSize = g_simulationGridSize >> 1u;
    bool2 liesInLowerFrequencyHalf = bool2(dispatchThreadID.x < halfGridSize,
                                           dispatchThreadID.y < halfGridSize);
    int2 wrappedFrequency = int2(dispatchThreadID.xy - g_simulationGridSize);
    int2 centeredFrequency;
    centeredFrequency.x = liesInLowerFrequencyHalf.x ? int(dispatchThreadID.x) : wrappedFrequency.x;
    centeredFrequency.y = liesInLowerFrequencyHalf.y ? int(dispatchThreadID.y) : wrappedFrequency.y;

    // The radius is accumulated in integers (imul/iadd/itof), matching the CPU
    // path's sqrt(kx * kx + ky * ky). iadd r2.x, r2.y, r2.x: the source addends
    // must stay in the order x + y.
    int2 frequencySquared = centeredFrequency * centeredFrequency;
    float2 frequency = float2(centeredFrequency);
    float radius = sqrt(float(frequencySquared.x + frequencySquared.y));

    // Deep-water dispersion with a surface-tension term:
    // omega = sqrt(g * k + sigma * k^3).
    float waveNumber = fundamentalWavenumber * radius;
    float waveNumberSquared = waveNumber * waveNumber;
    float waveNumberCubed = waveNumber * waveNumberSquared;
    float omegaSquared = waveNumberCubed * kSurfaceTension;
    // mad r2.y, r0.y, l(9.810000), r2.y: written out with the fused product as
    // the first addend, which is what the shipped blob encodes.
    omegaSquared = waveNumber * kGravity + omegaSquared;
    float omega = sqrt(omegaSquared);

    bool omegaIsZero = (omega == 0.0f);
    float reciprocalOmega = 1.0f / omega;
    float phase = omega * g_dT;
    float sinPhase = sin(phase);
    float cosPhase = cos(phase);
    float inverseOmega = omegaIsZero ? 0.0f : reciprocalOmega;

    float sinOverOmega = sinPhase * inverseOmega;
    float waveNumberOverOmega = waveNumber * inverseOmega;
    float sinWaveNumberOverOmega = sinPhase * waveNumberOverOmega;

    // dampening^dT spread over the inverse-FFT normalisation, as the CPU path's
    // amplitudeScale = pow(0.95, t) / (N * N).
    float amplitudeScale = pow(abs(g_dampening), g_dT);
    float sampleCount = float(g_simulationGridSize * g_simulationGridSize);
    amplitudeScale /= sampleCount;

    // Rotation of the (displacement, potential) pair by the phase advance:
    //   displacement' = cos * displacement + (k / omega) * sin * potential
    //   potential'    = cos * potential    - (C / omega) * sin * displacement
    //
    // The shipped blob uses C = g (mul r2.z, r2.z, l(9.810000)), not the
    // g + sigma * k^2 the exact linearised solution calls for: surface tension
    // enters omega but not the restoring term. For the map to be a rotation the
    // two off-diagonal coefficients must multiply to one, and
    //   (k / omega) * (g / omega) = k * g / (k * (g + sigma * k^2)) < 1,
    // so as shipped it is a slight inward spiral and amplitude bleeds away at
    // capillary wavenumbers, on top of the intended dampening. With
    // C = g + sigma * k^2 the product is exactly one.
    //
    // Defining WWS_FIX_CAPILLARY_RESTORING corrects it. That necessarily changes
    // the emitted bytecode, so the default is the shipped behaviour and
    // tools/dxbccmp.sh only matches with the define absent.
    float restoringCoefficient = kGravity;
#ifdef WWS_FIX_CAPILLARY_RESTORING
    restoringCoefficient = kGravity + kSurfaceTension * waveNumberSquared;
#endif
    float potentialFromDisplacement = amplitudeScale * sinOverOmega;
    potentialFromDisplacement *= restoringCoefficient;
    float cosTerm = cosPhase * amplitudeScale;
    float displacementFromPotential = sinWaveNumberOverOmega * amplitudeScale;

    float4 dampedSpectrum = spectrum * cosTerm;
    float2 evolvedVelocityPotential =
        mad(-potentialFromDisplacement, spectrum.xy, dampedSpectrum.zw);
    float2 evolvedVerticalDisplacement =
        mad(displacementFromPotential, spectrum.zw, dampedSpectrum.xy);
    float4 evolved = float4(evolvedVerticalDisplacement, evolvedVelocityPotential);
    g_verticalDisplacementAndVelocityPotentialFS[spectrumIndex] = evolved;

    // Lateral displacement is the height rotated a quarter turn and steered by
    // the unit wave vector: (-k * h.im, k * h.re) / radius, per the CPU path.
    float2 negatedImaginaryHeight = frequency * -evolved.y;
    float2 scaledRealHeight = frequency * evolved.x;
    // Source order is load-bearing: the real quotient must be written before
    // the imaginary one for FXC to emit div r3.yw ahead of div r3.xz.
    float2 lateralReal = negatedImaginaryHeight / radius;
    float2 lateralImaginary = scaledRealHeight / radius;
    float4 lateralDisplacement;
    lateralDisplacement.xz = lateralReal;
    lateralDisplacement.yw = lateralImaginary;

    // and r1.xyzw, r3.xyzw, r0.yyyy: the zero-frequency guard is a bitwise mask
    // against the comparison result, not a movc select; source order is
    // mask & value.
    uint nonZeroRadiusMask = (0.0f < radius) ? 0xffffffffu : 0u;
    g_lateralDisplacementsFS[spectrumIndex] =
        asfloat(nonZeroRadiusMask & asuint(lateralDisplacement));
}
