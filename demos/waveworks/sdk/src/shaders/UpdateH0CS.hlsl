// Recovered from _UpdateH0CS_bytecode_dx.msasm.
//
// The kernel seeds the initial spectrum H0: a JONSWAP peak-enhanced Phillips
// spectrum with a sech^2 directional spread, evaluated twice (a wind-driven
// "base" band and a "swell" band) and multiplied by a Gaussian sample. The same
// algorithm is implemented readably on the CPU in Jonswap(),
// src/Wind_Waves_FFT_Simulation_CPU_Impl.cpp; the names below follow it.
//
// Byte-identical to the shipped blob under FXC 10.1: raw=0 rn=0, 258/258.
//
// The long-standing residual here was one lane choice. The shipped blob squares
// both coordinate pairs as `r0.yyyx * r0.yyyx` (instructions 5 and 18), and FXC
// only emits that for `v.yx * v.yx`; spelled `v * v` the two lanes exchange and
// the whole downstream register allocation shifts with them. Both sites carry a
// comment.
//
// FXC also emits a commutative `mul` with its operands in the reverse of the
// source order, so the several `x = x * y` lines below are deliberate: written
// `x = y * x` they still compute the same value but swap the operands in the
// bytecode.

cbuffer WWCB : register(b0)
{
    uint m_resolution;
    uint m_resolution_plus_one;
    uint m_half_resolution;
    uint m_half_resolution_plus_one;
    uint m_resolution_plus_one_squared_minus_one;
    uint m_32_minus_log2_resolution;
    float m_window_in;
    float m_window_out;
    float m_frequency_scale;
    float m_choppy_scale;
    double m_time;
    float2 m_base_wind_dir;
    float m_base_wind_speed;
    float m_base_wind_distance;
    float m_base_wind_dependency;
    float m_base_small_waves_cutoff_length;
    float m_base_small_waves_cutoff_power;
    float m_base_spectrum_peaking;
    float2 m_swell_wind_dir;
    float m_base_linear_scale;
    float m_swell_wind_speed;
    float m_swell_wind_distance;
    float m_swell_wind_dependency;
    float m_swell_small_waves_cutoff_length;
    float m_swell_small_waves_cutoff_power;
    float m_swell_spectrum_peaking;
    float m_swell_linear_scale;
    float pad0;
    float pad1;
};

StructuredBuffer<float2> g_gauss_input : register(t0);
RWStructuredBuffer<float2> g_h0_output : register(u0);

static const float Gravity = 9.81000041961669921875f;
// Surface tension over density: the capillary term of the dispersion relation.
static const float SurfaceTension = 7.4000003223773092031478881835938e-05f;
static const float Pi = 3.1415927410125732421875f;
static const float TwoPi = 6.283185482025146484375f;
static const float InverseTwoPi = 0.15915493667125701904296875f;
static const float Log2E = 1.44269502162933349609375f;
static const float MinimumWindSpeed = 9.9999999747524270787835121154785e-07f;
// The JONSWAP peak is 22 * pow(96.2361 / (windSpeed * windDistance), 0.33).
static const float PeakFrequencyNumerator = 96.2360992431640625f;
static const float PeakFrequencyExponent = 0.3300000131130218505859375f;
static const float PeakFrequencyScale = 22.0f;
static const float AlphaExponent = 0.2199999988079071044921875f;
// The CPU path spells this as alpha = pow(..., 0.22) * 0.076 and then multiplies
// by gravity twice; 0.076 * 9.81 * 9.81 is folded into the one constant here.
static const float AlphaTimesGravitySquared = 7.3139438629150390625f;
// sigma^2 of the peak-enhancement lobe: 0.07^2 below the peak, 0.09^2 above it.
static const float SigmaSquaredBelowPeak = 0.004900000058114528656005859375f;
static const float SigmaSquaredAbovePeak = 0.008100000210106372833251953125f;
// -1.25 * log2(e): the Phillips tail exp(-1.25 * (peak / omega)^4) as an exp2.
static const float PhillipsExponentScale = -1.8033688068389892578125f;
// 2 * pi^2, converting the omega spectrum to a wave-vector spectrum.
static const float TwoPiSquared = 19.739208221435546875f;
// 2 * pi * log2(e): exp(2 * pi * spread) as an exp2.
static const float TwoPiLog2E = 9.06472015380859375f;

// FXC lowers pow() to `log R, R`; the shipped blob has `log R, |R|`, so the
// magnitude has to be spelled out.
float PowAbs(float base, float exponent)
{
    return exp2(log2(abs(base)) * exponent);
}

[numthreads(512, 1, 1)]
void UpdateH0CS(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    if (dispatchThreadID.x < m_resolution)
    {
        // iadd r0.xy, vThreadID.yxyy, -cb0[0].zzzz: the shipped blob centres the
        // lattice index with the row in .x and the column in .y, so from here on .x
        // carries the y axis and .y carries the x axis.
        int2 centeredIndex = int2(dispatchThreadID.yx) - (int)m_half_resolution;
        // imul null, r0.zw, r0.yyyx, r0.yyyx: the .yx is load-bearing. See the header.
        int2 centeredIndexSquared = centeredIndex.yx * centeredIndex.yx;
        float gridRadius = sqrt(float(centeredIndexSquared.x + centeredIndexSquared.y));
        // The DC term is dropped, and each cascade only owns an annulus of the lattice.
        bool insideBand = (centeredIndex.y | centeredIndex.x) != 0 &&
            gridRadius >= m_window_in && gridRadius < m_window_out;

        float amplitude;
        if (insideBand)
        {
            float2 waveVector = float2(centeredIndex) * m_frequency_scale;
            // mul r0.zw, r0.yyyx, r0.yyyx: the .yx is load-bearing, as above.
            float2 waveVectorSquared = waveVector.yx * waveVector.yx;
            float waveNumberSquared = waveVectorSquared.x + waveVectorSquared.y;
            float waveNumber = sqrt(waveNumberSquared);

            float baseWindSpeed = max(MinimumWindSpeed, m_base_wind_speed);
            // Dispersion with the capillary term: omega^2 = g * k + (sigma / rho) * k^3.
            float waveNumberCubed = waveNumberSquared * waveNumber;
            float omegaSquared =
                waveNumber * Gravity + waveNumberCubed * SurfaceTension;
            float omega = sqrt(omegaSquared);
            // d(omega)/dk; 0.000222 is 3 * SurfaceTension, from the k^3 term.
            float groupVelocity = (waveNumberSquared *
                0.00022200001694727689027786254882812f + Gravity) * 0.5f / omega;

            float basePeakFactor = PowAbs(PeakFrequencyNumerator /
                (baseWindSpeed * m_base_wind_distance), PeakFrequencyExponent);
            float basePeakAngularFrequency = basePeakFactor * PeakFrequencyScale;

            float baseWindSpeedSquared = baseWindSpeed * baseWindSpeed;
            float omegaFourth = omegaSquared * omegaSquared;

            float baseAlphaFactor = PowAbs(baseWindSpeedSquared /
                (m_base_wind_distance * Gravity), AlphaExponent);

            bool baseBelowPeak = omega < basePeakAngularFrequency;
            // mad r1.w, -r1.w, l(22.000000), r1.z: the peak is re-formed from its
            // factor here rather than reusing basePeakAngularFrequency.
            float baseOmegaFromPeak = omega - basePeakFactor * PeakFrequencyScale;
            float baseNegativePeakDistanceSquared = (-baseOmegaFromPeak) * baseOmegaFromPeak;
            float baseSigmaSquared =
                baseBelowPeak ? SigmaSquaredBelowPeak : SigmaSquaredAbovePeak;
            float basePeakSquared = basePeakAngularFrequency * basePeakAngularFrequency;
            // dp2 r2.y, r2.zzzz, r2.yyyy: 2 * (sigma * peak)^2 is spelled as a dot.
            float baseTwoSigmaPeakSquared = dot(basePeakSquared.xx, baseSigmaSquared.xx);
            float baseNegativePeakExponent =
                baseNegativePeakDistanceSquared / baseTwoSigmaPeakSquared;

            float baseAlphaTimesGravitySquared = baseAlphaFactor * AlphaTimesGravitySquared;
            float basePeakShape = exp(baseNegativePeakExponent);

            float basePeakRatio = basePeakAngularFrequency / omega;
            float basePeakRatioSquared = basePeakRatio * basePeakRatio;
            float basePeakRatioFourth = basePeakRatioSquared * basePeakRatioSquared;
            float basePhillips = exp2(basePeakRatioFourth * PhillipsExponentScale);
            float baseAlphaPhillips = basePhillips * baseAlphaTimesGravitySquared;

            float basePeaking = PowAbs(m_base_spectrum_peaking, basePeakShape);
            float omegaToFifth = omega * omegaFourth;
            float baseSpectrum = baseAlphaPhillips * (basePeaking / omegaToFifth);

            // Shared by both bands: the omega-to-wave-vector Jacobian.
            float waveVectorJacobian =
                (groupVelocity * TwoPiSquared) / (waveNumber + waveNumber);
            baseSpectrum = baseSpectrum * waveVectorJacobian;

            float omegaToBasePeak = omega / basePeakAngularFrequency;
            bool baseBelowUpperSpread = omegaToBasePeak < 1.60000002384185791015625f;
            bool baseBelowLowerSpread = omegaToBasePeak < 0.949999988079071044921875f;
            // log r1.w, r1.w carries no abs modifier, so the two spread powers
            // cannot be spelled pow() -- they share this one log2.
            float baseLogOmegaToPeak = log2(omegaToBasePeak);
            float baseUpperSpread =
                exp2(baseLogOmegaToPeak * -1.2999999523162841796875f) *
                2.2799999713897705078125f;
            float baseLowerSpread =
                exp2(baseLogOmegaToPeak * 1.2999999523162841796875f) *
                2.6099998950958251953125f;
            float baseSpread =
                baseBelowUpperSpread ? baseUpperSpread : 1.2400000095367431640625f;
            baseSpread = baseBelowLowerSpread ? baseLowerSpread : baseSpread;

            float2 waveDirection = waveVector / waveNumber;
            // waveDirection.x is the y component and .y the x component, so this is
            // atan2(k_y, k_x) -- theta in the CPU path.
            float theta = atan2(waveDirection.x, waveDirection.y);

            float baseWindAngle = atan2(m_base_wind_dir.y, m_base_wind_dir.x);
            float baseAngleDelta = theta - baseWindAngle;
            baseAngleDelta =
                (Pi < baseAngleDelta) ? (baseAngleDelta - TwoPi) : baseAngleDelta;
            baseAngleDelta =
                (baseAngleDelta < -Pi) ? (baseAngleDelta + TwoPi) : baseAngleDelta;

            float baseIsotropicWeight = 1.0f - m_base_wind_dependency;
            float baseCutoffAttenuation = 1.0f - m_base_small_waves_cutoff_power;

            float baseSpreadTimesDependency = baseSpread * m_base_wind_dependency;
            float baseScaledAngle = baseSpread * baseAngleDelta;
            // 2 / (e^u + e^-u) is sech(u). The CPU path evaluates an eighth-order
            // Taylor cosh instead of the exponentials.
            float baseTwoCosh = exp(baseScaledAngle) + exp(-baseScaledAngle);
            float baseSech = 2.0f / baseTwoCosh;
            float wavelength = TwoPi / waveNumber;
            float baseDirectional = baseSpreadTimesDependency * (baseSech * baseSech);

            float baseExpTwoSpreadPi = exp2(baseSpread * TwoPiLog2E);
            // tanh(pi * spread) normalises the sech^2 lobe over [-pi, pi]. The CPU
            // path uses the [7/6] Pade approximant of tanh instead.
            float baseSpreadIntegral =
                (baseExpTwoSpreadPi - 1.0f) / (baseExpTwoSpreadPi + 1.0f);
            float baseTwoSpreadIntegral = baseSpreadIntegral + baseSpreadIntegral;
            float baseDirectionalTotal = baseIsotropicWeight * InverseTwoPi +
                baseDirectional / baseTwoSpreadIntegral;
            baseSpectrum = baseSpectrum * baseDirectionalTotal;

            bool baseBelowCutoff = wavelength < m_base_small_waves_cutoff_length;
            baseSpectrum =
                baseBelowCutoff ? (baseSpectrum * baseCutoffAttenuation) : baseSpectrum;

            float swellWindSpeed = max(MinimumWindSpeed, m_swell_wind_speed);
            float swellPeakFactor = PowAbs(PeakFrequencyNumerator /
                (swellWindSpeed * m_swell_wind_distance), PeakFrequencyExponent);
            float swellPeakAngularFrequency = swellPeakFactor * PeakFrequencyScale;

            float swellWindSpeedSquared = swellWindSpeed * swellWindSpeed;
            float swellAlphaFactor = PowAbs(swellWindSpeedSquared /
                (m_swell_wind_distance * Gravity), AlphaExponent);
            // This comparison has to sit here, between the two halves of the alpha
            // chain: sunk below `exp r1.x` FXC folds `mul r2.x, r1.w, l(22.000000)`
            // into the exponent multiply and the program loses an instruction.
            bool swellBelowPeak = omega < swellPeakAngularFrequency;

            float swellOmegaFromPeak = omega - swellPeakFactor * PeakFrequencyScale;
            float swellNegativePeakDistanceSquared =
                (-swellOmegaFromPeak) * swellOmegaFromPeak;
            float swellSigmaSquared =
                swellBelowPeak ? SigmaSquaredBelowPeak : SigmaSquaredAbovePeak;
            float swellPeakSquared =
                swellPeakAngularFrequency * swellPeakAngularFrequency;
            float swellTwoSigmaPeakSquared =
                dot(swellPeakSquared.xx, swellSigmaSquared.xx);
            float swellNegativePeakExponent =
                swellNegativePeakDistanceSquared / swellTwoSigmaPeakSquared;

            float swellAlphaTimesGravitySquared = swellAlphaFactor * AlphaTimesGravitySquared;
            float swellPeakShape = exp(swellNegativePeakExponent);

            // The swell band settles its spread before its Phillips tail; the base
            // band does it the other way round.
            float omegaToSwellPeak = omega / swellPeakAngularFrequency;
            bool swellBelowUpperSpread = omegaToSwellPeak < 1.60000002384185791015625f;
            bool swellBelowLowerSpread = omegaToSwellPeak < 0.949999988079071044921875f;
            float swellLogOmegaToPeak = log2(omegaToSwellPeak);
            float swellUpperSpread =
                exp2(swellLogOmegaToPeak * -1.2999999523162841796875f) *
                2.2799999713897705078125f;
            float swellLowerSpread =
                exp2(swellLogOmegaToPeak * 1.2999999523162841796875f) *
                2.6099998950958251953125f;
            float swellSpread =
                swellBelowUpperSpread ? swellUpperSpread : 1.2400000095367431640625f;
            swellSpread = swellBelowLowerSpread ? swellLowerSpread : swellSpread;

            float swellPeakRatio = swellPeakAngularFrequency / omega;
            float swellPeakRatioSquared = swellPeakRatio * swellPeakRatio;
            float swellPeakRatioFourth = swellPeakRatioSquared * swellPeakRatioSquared;
            float swellPhillips = exp2(swellPeakRatioFourth * PhillipsExponentScale);
            float swellAlphaPhillips = swellAlphaTimesGravitySquared * swellPhillips;

            // mul r1.z, r1.z, r1.w: the swell band takes the logarithm times the
            // exponent, the opposite order from PowAbs(), so it is spelled out.
            float swellPeaking = exp2(swellPeakShape * log2(abs(m_swell_spectrum_peaking)));
            float swellPeakingOverOmegaToFifth = swellPeaking / omegaToFifth;
            float swellSpectrum = swellAlphaPhillips * swellPeakingOverOmegaToFifth;
            swellSpectrum = waveVectorJacobian * swellSpectrum;

            float swellWindAngle = atan2(m_swell_wind_dir.y, m_swell_wind_dir.x);
            float swellAngleDelta = -swellWindAngle + theta;
            swellAngleDelta =
                (Pi < swellAngleDelta) ? (swellAngleDelta - TwoPi) : swellAngleDelta;
            swellAngleDelta =
                (swellAngleDelta < -Pi) ? (swellAngleDelta + TwoPi) : swellAngleDelta;

            float swellIsotropicWeight = 1.0f - m_swell_wind_dependency;
            float swellCutoffAttenuation = 1.0f - m_swell_small_waves_cutoff_power;

            float swellSpreadTimesDependency = swellSpread * m_swell_wind_dependency;
            float swellScaledAngle = swellSpread * swellAngleDelta;
            float swellTwoCosh = exp(swellScaledAngle) + exp(-swellScaledAngle);
            float swellSech = 2.0f / swellTwoCosh;
            float swellDirectional = swellSpreadTimesDependency * (swellSech * swellSech);

            float swellExpTwoSpreadPi = exp2(swellSpread * TwoPiLog2E);
            float swellSpreadIntegral =
                (swellExpTwoSpreadPi - 1.0f) / (swellExpTwoSpreadPi + 1.0f);
            float swellTwoSpreadIntegral = swellSpreadIntegral + swellSpreadIntegral;
            float swellDirectionalTotal = swellIsotropicWeight * InverseTwoPi +
                swellDirectional / swellTwoSpreadIntegral;
            swellSpectrum = swellSpectrum * swellDirectionalTotal;

            bool swellBelowCutoff = wavelength < m_swell_small_waves_cutoff_length;
            swellSpectrum = swellBelowCutoff ?
                (swellSpectrum * swellCutoffAttenuation) : swellSpectrum;

            // sqrt r0.xy, r0.xyxx: both band amplitudes in one instruction.
            amplitude = m_base_linear_scale * sqrt(baseSpectrum) +
                sqrt(swellSpectrum) * m_swell_linear_scale;
        }
        else
        {
            amplitude = 0.0f;
        }

        // The H0 grid is (N + 1) wide, the Gaussian source only N.
        uint h0Index = dispatchThreadID.y * m_resolution_plus_one + dispatchThreadID.x;
        uint gaussIndex = h0Index - dispatchThreadID.y;
        float2 gauss = g_gauss_input[gaussIndex];
        g_h0_output[h0Index] = amplitude * gauss;

        // ieq r0.xz, vThreadID.yyxy, l(0, 0, 0, 0): the first row and the first
        // column clear their mirror image so the conjugate wrap stays symmetric.
        bool2 onFirstRowOrColumn = int2(dispatchThreadID.yx) == 0;
        if (onFirstRowOrColumn.x || onFirstRowOrColumn.y)
        {
            g_h0_output[m_resolution_plus_one_squared_minus_one - h0Index] =
                float2(0.0f, 0.0f);
        }
    }
}
