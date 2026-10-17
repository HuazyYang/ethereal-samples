#include "Wind_Waves_FFT_Simulation_CPU_Impl.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <atomic>
#include <waveworks/GFSDK_WaveWorks.h>
#include "CustomMemory.h"

namespace
{
    template <typename T>
    void freeAlignedArray(T*& ptr)
    {
        if (ptr)
        {
            NVSDK_aligned_free(ptr);
            ptr = nullptr;
        }
    }

    struct PrecalculatedJonswapIntermediatesType
    {
        uint8_t padding0[8]; // offset 0
        float basePeakAngularFrequency; // offset 8
        float baseAlpha; // offset 12
        float swellPeakAngularFrequency; // offset 16
        float swellAlpha; // offset 20
    };

    static_assert(sizeof(PrecalculatedJonswapIntermediatesType) == 24,
                  "Bad JONSWAP intermediate layout.");

    float Jonswap(
        int omegaIndex,
        float2 waveVector,
        const PrecalculatedJonswapIntermediatesType& precalculated,
        const float2* omegaData,
        float baseWindAngle,
        float baseSpectrumPeaking,
        float baseWindDependency,
        float baseSmallWavesCutoffLength,
        float baseSmallWavesCutoffPower,
        float baseAmplitudeScale,
        float swellWindAngle,
        float swellSpectrumPeaking,
        float swellWindDependency,
        float swellSmallWavesCutoffLength,
        float swellSmallWavesCutoffPower,
        float swellAmplitudeScale)
    {
        const float waveNumber = sqrtf(
            waveVector.y * waveVector.y + waveVector.x * waveVector.x);
        const float omega = omegaData[omegaIndex].x;
        const float theta = omegaData[omegaIndex].y;
        const float groupVelocity =
            (((waveNumber * 3.0f) * waveNumber * 0.074000001f) / 1000.0f +
             9.8100004f) *
            0.5f / omega;

        const float baseSigma =
            precalculated.basePeakAngularFrequency <= omega ? 0.090000004f : 0.07f;
        const float basePeakDistance =
            (omega - precalculated.basePeakAngularFrequency) /
            (baseSigma * precalculated.basePeakAngularFrequency);
        const float basePeakExponent =
            (basePeakDistance * basePeakDistance) * 0.5f;
        float basePeakShape;
        if (basePeakExponent < 16.0f)
        {
            const float negativeExponent = -basePeakExponent;
            if (basePeakExponent >= 0.25f)
            {
                basePeakShape = expf(negativeExponent);
            }
            else
            {
                basePeakShape =
                    (((3.0f - basePeakExponent) * (3.0f - basePeakExponent)) + 3.0f) /
                    (((negativeExponent - 3.0f) * (negativeExponent - 3.0f)) + 3.0f);
            }
        }
        else
        {
            basePeakShape = 0.0f;
        }

        const float omegaSquared = omega * omega;
        const float basePeaking =
            basePeakShape <= 0.0f ? 1.0f : powf(baseSpectrumPeaking, basePeakShape);
        const float basePeakRatio =
            precalculated.basePeakAngularFrequency / omega;
        const float basePhillipsExponent =
            ((basePeakRatio * basePeakRatio) * (basePeakRatio * basePeakRatio)) * 1.25f;
        float basePhillips;
        if (basePhillipsExponent < 16.0f)
        {
            const float negativeExponent = -basePhillipsExponent;
            if (basePhillipsExponent >= 0.25f)
            {
                basePhillips = expf(negativeExponent);
            }
            else
            {
                basePhillips =
                    (((3.0f - basePhillipsExponent) *
                      (3.0f - basePhillipsExponent)) +
                     3.0f) /
                    (((negativeExponent - 3.0f) * (negativeExponent - 3.0f)) +
                     3.0f);
            }
        }
        else
        {
            basePhillips = 0.0f;
        }

        const float omegaToBasePeak =
            omega / precalculated.basePeakAngularFrequency;
        const float omegaToFifth = (omegaSquared * omega) * omegaSquared;
        float baseSpectrum =
            (((precalculated.baseAlpha * 9.8100004f) * 9.8100004f) *
             basePhillips) *
            (basePeaking / omegaToFifth);
        baseSpectrum *=
            (groupVelocity * 19.73921f) / (waveNumber + waveNumber);

        float baseSpread = 1.24f;
        if (omegaToBasePeak < 1.6f)
            baseSpread = powf(omegaToBasePeak, -1.3f) * 2.28f;
        if (omegaToBasePeak < 0.94999999f)
            baseSpread = powf(omegaToBasePeak, 1.3f) * 2.6099999f;

        float baseAngleDelta = theta - baseWindAngle;
        if (baseAngleDelta > Pif)
            baseAngleDelta += -TwoPif;
        if (baseAngleDelta < -Pif)
            baseAngleDelta += TwoPif;

        const float baseAngleSquared =
            (baseAngleDelta * baseSpread) * (baseAngleDelta * baseSpread);
        const float baseSpreadPi = baseSpread * Pif;
        const float baseCoshApproximation =
            ((((baseAngleSquared * 0.5f) + 1.0f) +
              ((baseAngleSquared * baseAngleSquared) * 0.041666668f)) +
             (((baseAngleSquared * baseAngleSquared) * baseAngleSquared) *
              0.0013888889f)) +
            (((baseAngleSquared * baseAngleSquared) *
              (baseAngleSquared * baseAngleSquared)) *
             0.000024801588f);
        const float baseSpreadPiSquared = baseSpreadPi * baseSpreadPi;
        const float baseSpreadPiSixth =
            (baseSpreadPiSquared * baseSpreadPiSquared) *
            (baseSpreadPi * baseSpreadPi);
        const float baseIntegralNumerator =
            ((((baseSpreadPi * baseSpreadPi) * 17325.0f + 135135.0f) +
              ((baseSpreadPiSquared * baseSpreadPiSquared) * 378.0f)) +
             baseSpreadPiSixth) *
            baseSpreadPi;
        const float baseIntegralDenominator =
            (((baseSpreadPi * baseSpreadPi) * 62370.0f + 135135.0f) +
             ((baseSpreadPiSquared * baseSpreadPiSquared) * 3150.0f)) +
            baseSpreadPiSixth * 28.0f;
        const float baseIntegral =
            baseIntegralNumerator / baseIntegralDenominator;
        baseSpectrum *=
            ((((baseSpread * baseWindDependency) *
               (1.0f / baseCoshApproximation)) *
              (1.0f / baseCoshApproximation)) /
             (baseIntegral + baseIntegral)) +
            ((1.0f - baseWindDependency) * 0.15915494f);
        if (baseSmallWavesCutoffLength > TwoPif / waveNumber)
            baseSpectrum *= 1.0f - baseSmallWavesCutoffPower;

        const float swellSigma =
            precalculated.swellPeakAngularFrequency <= omega ? 0.090000004f : 0.07f;
        const float swellPeakDistance =
            (omega - precalculated.swellPeakAngularFrequency) /
            (swellSigma * precalculated.swellPeakAngularFrequency);
        const float swellPeakExponent =
            (swellPeakDistance * swellPeakDistance) * 0.5f;
        float swellPeakShape;
        if (swellPeakExponent < 16.0f)
        {
            const float negativeExponent = -swellPeakExponent;
            if (swellPeakExponent >= 0.25f)
            {
                swellPeakShape = expf(negativeExponent);
            }
            else
            {
                swellPeakShape =
                    (((3.0f - swellPeakExponent) *
                      (3.0f - swellPeakExponent)) +
                     3.0f) /
                    (((negativeExponent - 3.0f) * (negativeExponent - 3.0f)) +
                     3.0f);
            }
        }
        else
        {
            swellPeakShape = 0.0f;
        }

        const float swellPeakRatio =
            precalculated.swellPeakAngularFrequency / omega;
        const float swellPeaking =
            swellPeakShape <= 0.0f ? 1.0f :
                                           powf(swellSpectrumPeaking, swellPeakShape);
        const float swellPhillipsExponent =
            ((swellPeakRatio * swellPeakRatio) *
             (swellPeakRatio * swellPeakRatio)) *
            1.25f;
        float swellPhillips = 0.0f;
        if (swellPhillipsExponent < 16.0f)
        {
            const float negativeExponent = -swellPhillipsExponent;
            if (swellPhillipsExponent >= 0.25f)
            {
                swellPhillips = expf(negativeExponent);
            }
            else
            {
                swellPhillips =
                    (((3.0f - swellPhillipsExponent) *
                      (3.0f - swellPhillipsExponent)) +
                     3.0f) /
                    (((negativeExponent - 3.0f) * (negativeExponent - 3.0f)) +
                     3.0f);
            }
        }

        const float omegaToSwellPeak =
            omega / precalculated.swellPeakAngularFrequency;
        float swellSpectrum =
            ((((precalculated.swellAlpha * 9.8100004f) * 9.8100004f) *
              swellPhillips) *
             (swellPeaking / omegaToFifth)) *
            ((groupVelocity * 19.73921f) / (waveNumber + waveNumber));

        float swellSpread = 1.24f;
        if (omegaToSwellPeak < 1.6f)
            swellSpread = powf(omegaToSwellPeak, -1.3f) * 2.28f;
        if (omegaToSwellPeak < 0.94999999f)
            swellSpread = powf(omegaToSwellPeak, 1.3f) * 2.6099999f;

        float swellAngleDelta = theta - swellWindAngle;
        if (swellAngleDelta > Pif)
            swellAngleDelta += -TwoPif;
        if (swellAngleDelta < -Pif)
            swellAngleDelta += TwoPif;

        const float swellAngleSquared =
            (swellAngleDelta * swellSpread) * (swellAngleDelta * swellSpread);
        const float swellSpreadPi = swellSpread * Pif;
        const float swellSpreadPiSquared = swellSpreadPi * swellSpreadPi;
        const float swellCoshReciprocal =
            1.0f /
            (((((swellAngleSquared * 0.5f) + 1.0f) +
               ((swellAngleSquared * swellAngleSquared) * 0.041666668f)) +
              (((swellAngleSquared * swellAngleSquared) * swellAngleSquared) *
               0.0013888889f)) +
             (((swellAngleSquared * swellAngleSquared) *
               (swellAngleSquared * swellAngleSquared)) *
              0.000024801588f));
        const float swellSpreadPiSixth =
            (swellSpreadPiSquared * swellSpreadPiSquared) *
            (swellSpreadPi * swellSpreadPi);
        const float swellIntegralNumeratorWithoutPi =
            (((swellSpreadPi * swellSpreadPi) * 17325.0f + 135135.0f) +
             ((swellSpreadPiSquared * swellSpreadPiSquared) * 378.0f)) +
            swellSpreadPiSixth;
        const float swellIntegralDenominator =
            (((swellSpreadPi * swellSpreadPi) * 62370.0f + 135135.0f) +
             ((swellSpreadPiSquared * swellSpreadPiSquared) * 3150.0f)) +
            swellSpreadPiSixth * 28.0f;
        const float swellIntegral =
            (swellIntegralNumeratorWithoutPi * swellSpreadPi) /
            swellIntegralDenominator;
        swellSpectrum *=
            ((((swellSpread * swellWindDependency) *
               swellCoshReciprocal) *
              swellCoshReciprocal) /
             (swellIntegral + swellIntegral)) +
            ((1.0f - swellWindDependency) * 0.15915494f);
        if (swellSmallWavesCutoffLength > TwoPif / waveNumber)
            swellSpectrum *= 1.0f - swellSmallWavesCutoffPower;

        return sqrtf(baseSpectrum) * baseAmplitudeScale +
               sqrtf(swellSpectrum) * swellAmplitudeScale;
    }

    void calculateSineAndCosine(float angle, float& sine, float& cosine)
    {
        constexpr float FourOverPi = 1.27323954473516f;
        constexpr float Dp1 = -0.78515625f;
        constexpr float Dp2 = -2.4187564849853515625e-4f;
        constexpr float Dp3 = -3.77489497744594108e-8f;
        constexpr float CosineCoefficient0 = 2.443315711809948e-5f;
        constexpr float CosineCoefficient1 = -1.388731625493765e-3f;
        constexpr float CosineCoefficient2 = 4.166664568298827e-2f;
        constexpr float SineCoefficient0 = -1.9515295891e-4f;
        constexpr float SineCoefficient1 = 8.3321608736e-3f;
        constexpr float SineCoefficient2 = -1.6666654611e-1f;

        const bool angleIsNegative = std::signbit(angle);
        const float absoluteAngle = std::fabs(angle);
        int quadrant = static_cast<int>(absoluteAngle * FourOverPi);
        quadrant = (quadrant + 1) & ~1;

        const float quadrantAsFloat = static_cast<float>(quadrant);
        const float reducedAngle =
            ((quadrantAsFloat * Dp1 + absoluteAngle) + quadrantAsFloat * Dp2) +
            quadrantAsFloat * Dp3;
        const float squaredAngle = reducedAngle * reducedAngle;

        const float cosinePolynomial =
            (((CosineCoefficient0 * squaredAngle + CosineCoefficient1) *
                  squaredAngle +
              CosineCoefficient2) *
                 squaredAngle * squaredAngle -
             0.5f * squaredAngle) +
            1.0f;
        const float sinePolynomial =
            ((SineCoefficient0 * squaredAngle + SineCoefficient1) *
                     squaredAngle +
                 SineCoefficient2) *
                squaredAngle * reducedAngle +
            reducedAngle;

        if ((quadrant & 2) == 0)
        {
            sine = sinePolynomial;
            cosine = cosinePolynomial;
        }
        else
        {
            sine = cosinePolynomial;
            cosine = sinePolynomial;
        }

        if (angleIsNegative != ((quadrant & 4) != 0))
            sine = -sine;
        if (((~(quadrant - 2)) & 4) != 0)
            cosine = -cosine;
    }

    uint16_t FloatToHalfBitsTruncate(float value)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));

        const uint32_t exponent = (bits >> 23) & 0xffu;
        const uint32_t halfExponent =
            (((0x70u - exponent) >> 16) & 0x1fu & (exponent - 0x70u)) |
            ((bits >> 31) << 5);

        return static_cast<uint16_t>((halfExponent << 10) | ((bits >> 13) & 0x3ffu));
    }

    int32_t interlockedDecrement(std::atomic<int32_t>& value)
    {
        return value.fetch_sub(1, std::memory_order_seq_cst) - 1;
    }

    void interlockedExchange(std::atomic<int32_t>& value, int32_t desired)
    {
        value.store(desired, std::memory_order_seq_cst);
    }
}

Wind_Waves_FFT_Simulation_CPU_Impl::Wind_Waves_FFT_Simulation_CPU_Impl(
    const Wind_Waves_Detailed_Simulation_Parameters::Cascade& cascade)
    : m_pendingCascade(cascade)
    , m_activeCascade(cascade)
    , m_hasPendingCascade(false)
    , m_numUpdateH0Tasks(0)
    , m_numUpdateHtTasks(0)
    , m_numFFT_XTasks(0)
    , m_numFFT_YTasks(0)
    , m_numUpdateTextureTasks(0)
    , m_device(nullptr)
    , m_cmdList(nullptr)
    , m_useCpu(false)
    , m_displacementTexture(nullptr)
    , m_fftxBuffersFp16{}
    , m_fftxRowPitch(0)
    , m_padding2(0)
    , m_gaussData(nullptr)
    , m_omegaData0(nullptr)
    , m_omegaData1(nullptr)
    , m_inverseRadius(nullptr)
    , m_fftBuffers(nullptr)
    , m_displacementBuffers{}
    , m_currentDisplacementBuffer(nullptr)
    , m_archiveRing(nullptr)
    , m_currentDisplacementBufferIndex(0)
    , m_padding3(0)
    , m_simulationTime(0.0)
    , m_hasDisplacementMap(true)
    , m_displacementMapVersion(UINT64_MAX)
    , m_useDelayedCascade(false)
{
}

Wind_Waves_FFT_Simulation_CPU_Impl::~Wind_Waves_FFT_Simulation_CPU_Impl()
{
    releaseAllResources();
}

int64_t Wind_Waves_FFT_Simulation_CPU_Impl::ComputeFFT_X(uint taskX, uint taskY, int sign)
{
    const uint N = m_activeCascade.textureSize;

    // N, N
    // gridDim = (1, N / 32)
    // blockDim = (1, 32)
    // blockIdx.x = taskX, blockIdx.y = taskY
    // globalIdx = (taskX, taskY * 32)
    // linearGlobalIdx = (taskX * N + taskY * 32) * N
    // loop 8 times, each step process 4 * N elements
    // linearGlobalIdx = (taskX * N + taskY * 32 + 4 * i) * N

    const uint blockBase = 8 * taskY;
    for (uint i = 0; i < 8u; ++i)
    {
        uint rowIndex = taskX * N + 4u * (blockBase + i);
        float2* row = m_fftBuffers + static_cast<size_t>(rowIndex) * N;
        Simulation_Util::FFT1DSIMD_X_4wide(row, N, sign);
    }

    return static_cast<int64_t>(interlockedDecrement(m_numFFT_XTasks));
}

int64_t Wind_Waves_FFT_Simulation_CPU_Impl::ComputeFFT_Y(uint taskX, uint taskY, int sign)
{
    const uint N = m_activeCascade.textureSize;

    const uint blockBase = 8u * static_cast<uint>(taskY);
    for (uint i = 0; i < 8u; ++i)
    {
        uint rowIndex = taskX * N * N + 4u * (blockBase + i);
        float2* row = m_fftBuffers + static_cast<size_t>(rowIndex);
        Simulation_Util::FFT1DSIMD_Y_4wide(row, N, sign);
    }

    return static_cast<int64_t>(interlockedDecrement(m_numFFT_YTasks));
}

int64_t Wind_Waves_FFT_Simulation_CPU_Impl::GetNumFFT_XTasks() const
{
    return static_cast<int64_t>(m_activeCascade.textureSize >> 5);
}

int64_t Wind_Waves_FFT_Simulation_CPU_Impl::GetNumFFT_YTasks() const
{
    return static_cast<int64_t>(m_activeCascade.textureSize >> 5);
}

int64_t Wind_Waves_FFT_Simulation_CPU_Impl::GetNumUpdateH0Tasks() const
{
    return static_cast<int64_t>((m_activeCascade.textureSize >> 3) + 1);
}

int64_t Wind_Waves_FFT_Simulation_CPU_Impl::GetNumUpdateHtTasks() const
{
    return static_cast<int64_t>(m_activeCascade.textureSize >> 4);
}

int64_t Wind_Waves_FFT_Simulation_CPU_Impl::GetNumUpdateTextureTasks() const
{
    return static_cast<int64_t>(m_activeCascade.textureSize >> 4);
}

int64_t Wind_Waves_FFT_Simulation_CPU_Impl::GetTotalNumberOfTasks() const
{
    int base = 0;
    if (m_hasDisplacementMap)
        base = static_cast<int>((m_activeCascade.textureSize >> 3) + 1);

    return base + 2 * ((m_activeCascade.textureSize >> 4) +
                       ((3 * m_activeCascade.textureSize) >> 5));
}

void Wind_Waves_FFT_Simulation_CPU_Impl::OnCompleteSimulationStep(uint64_t frameIndex)
{
    m_displacementMapVersion = frameIndex;

    if (m_useCpu)
    {
        uint textureSize = m_activeCascade.textureSize;
        nvrhi::TextureSlice textureSlice = {};
        textureSlice.width = textureSize;
        textureSlice.height = textureSize;
        size_t rowPitch;
        uint8_t* buffer =
            (uint8_t *)m_device->mapStagingTexture(m_staging[m_currentDisplacementBufferIndex],
                                        textureSlice, nvrhi::CpuAccessMode::Write, rowPitch);
        if(textureSize) {
            for (uint j = 0; j < textureSize; ++j) {
                memcpy(buffer + rowPitch * j,
                       reinterpret_cast<uint8_t*>(m_fftxBuffersFp16[m_currentDisplacementBufferIndex]) +
                           static_cast<size_t>(j) * m_fftxRowPitch,
                       m_fftxRowPitch);
            }
        }
        m_device->unmapStagingTexture(m_staging[m_currentDisplacementBufferIndex]);

        m_cmdList->beginTrackingTextureState(m_displacementTexture, nvrhi::AllSubresources,
                                             nvrhi::ResourceStates::ShaderResource);
        m_cmdList->copyTexture3(m_displacementTexture, textureSlice, m_staging[m_currentDisplacementBufferIndex], textureSlice);
        m_cmdList->setTextureState(m_displacementTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
        m_cmdList->commitBarriers();
    }

    m_currentDisplacementBuffer =
        reinterpret_cast<uint8_t*>(m_displacementBuffers[m_currentDisplacementBufferIndex]);
    m_displacementMapVersion = frameIndex;
    m_currentDisplacementBufferIndex = (m_currentDisplacementBufferIndex + 1) & 1;
}

WWRESULT Wind_Waves_FFT_Simulation_CPU_Impl::OnInitiateSimulationStep(double simulationTime)
{
    if (m_hasPendingCascade)
    {
        m_activeCascade = m_pendingCascade;
        m_hasPendingCascade = false;
    }

    m_simulationTime = simulationTime;

    const uint N = m_activeCascade.textureSize;
    interlockedExchange(m_numUpdateH0Tasks, static_cast<int32_t>((N >> 3) + 1));
    interlockedExchange(m_numUpdateHtTasks, static_cast<int32_t>(N >> 4));
    const int32_t fftTasks = static_cast<int32_t>((3 * N) >> 5);
    interlockedExchange(m_numFFT_XTasks, fftTasks);
    interlockedExchange(m_numFFT_YTasks, fftTasks);
    interlockedExchange(m_numUpdateTextureTasks, static_cast<int32_t>(N >> 4));

    return WWE_OK;
}

int64_t Wind_Waves_FFT_Simulation_CPU_Impl::UpdateDisplacements(uint taskIndex)
{
    // TODO: improve me using SIMD, see _Wind_Waves_FFT_Simulation_CPU_Impl.cpp:389-502.
    const uint N = m_activeCascade.textureSize;
    const uint bufferIndex = m_currentDisplacementBufferIndex;

    const float displacementScale = m_activeCascade.displacementScale;
    const uint startRow = taskIndex * 16u;
    const uint endRow = startRow + 16u;
    const bool writeDisplacementMap = m_activeCascade.writeDisplacementMap;

    float4* displacementDest = m_displacementBuffers[bufferIndex];
    uint8_t* fftxBase = reinterpret_cast<uint8_t*>(m_fftxBuffersFp16[bufferIndex]);
    const size_t planeStride = static_cast<size_t>(N) * N;
    const float2* fftPlane0 = m_fftBuffers;
    const float2* fftPlane1 = m_fftBuffers + planeStride;
    const float2* fftPlane2 = m_fftBuffers + 2u * planeStride;

    for (uint row = startRow; row < endRow; ++row)
    {
        const float2* rowPlane0 = fftPlane0 + static_cast<size_t>(row) * N;
        const float2* rowPlane1 = fftPlane1 + static_cast<size_t>(row) * N;
        const float2* rowPlane2 = fftPlane2 + static_cast<size_t>(row) * N;
        uint16_t* fftxRow = reinterpret_cast<uint16_t*>(
            fftxBase + static_cast<size_t>(row) * m_fftxRowPitch);

        for (uint col = 0; col < N; ++col)
        {
            // multiplier is (+-s, +-s, +-1, 1): only x/y carry displacementScale, z (height) does not
            const float checkerboardSign = ((row + col) & 1u) ? -1.0f : 1.0f;
            const float lateralScale = checkerboardSign * displacementScale;
            const float4 displacement = make_float4(
                rowPlane1[col].x * lateralScale,
                rowPlane2[col].x * lateralScale,
                rowPlane0[col].x * checkerboardSign,
                1.0f);

            uint16_t* tempPixel = fftxRow + 4u * col;
            tempPixel[0] = FloatToHalfBitsTruncate(displacement.x);
            tempPixel[1] = FloatToHalfBitsTruncate(displacement.y);
            tempPixel[2] = FloatToHalfBitsTruncate(displacement.z);
            tempPixel[3] = FloatToHalfBitsTruncate(displacement.w);

            if (writeDisplacementMap)
            {
                displacementDest[row * N + col] = displacement;
            }
        }
    }

    return static_cast<int64_t>(
        static_cast<uint32_t>(interlockedDecrement(m_numUpdateTextureTasks)));
}

int64_t Wind_Waves_FFT_Simulation_CPU_Impl::UpdateH0(uint taskIndex)
{
    float baseWindAngle = 0.0f;
    if (m_activeCascade.baseWindDirection.x != 0.0f ||
        m_activeCascade.baseWindDirection.y != 0.0f)
    {
        baseWindAngle = atan2f(
            m_activeCascade.baseWindDirection.y,
            m_activeCascade.baseWindDirection.x);
    }

    float swellWindAngle = 0.0f;
    if (m_activeCascade.swellWindDirection.x != 0.0f ||
        m_activeCascade.swellWindDirection.y != 0.0f)
    {
        swellWindAngle = atan2f(
            m_activeCascade.swellWindDirection.y,
            m_activeCascade.swellWindDirection.x);
    }

    const float physicalSize = m_activeCascade.physicalSize;
    const int textureSize = static_cast<int>(m_activeCascade.textureSize);
    const int rowWidth = textureSize + 4;
    const uint gaussOffset =
        static_cast<uint>(516 - rowWidth) >> 1u;
    const float baseAmplitudeScale =
        m_activeCascade.baseAmplitudeMultiplier / physicalSize;
    const float swellAmplitudeScale =
        m_activeCascade.swellAmplitudeMultiplier / physicalSize;

    float baseWindDistance = m_activeCascade.baseWindDistance;
    if (baseWindDistance <= 0.0f)
        baseWindDistance = 0.000001f;
    float baseWindSpeed = m_activeCascade.baseWindSpeed;
    if (baseWindSpeed <= 0.0f)
        baseWindSpeed = 0.000001f;

    PrecalculatedJonswapIntermediatesType precalculated;
    precalculated.basePeakAngularFrequency =
        powf(96.236107f / (baseWindSpeed * baseWindDistance), 0.33000001f) *
        22.0f;
    precalculated.baseAlpha =
        powf(
            (baseWindSpeed * baseWindSpeed) /
                (baseWindDistance * 9.8100004f),
            0.22f) *
        0.075999998f;

    float swellWindDistance = m_activeCascade.swellWindDistance;
    if (swellWindDistance <= 0.0f)
        swellWindDistance = 0.000001f;
    float swellWindSpeed = m_activeCascade.swellWindSpeed;
    if (swellWindSpeed <= 0.0f)
        swellWindSpeed = 0.000001f;

    precalculated.swellPeakAngularFrequency =
        powf(96.236107f / (swellWindSpeed * swellWindDistance), 0.33000001f) *
        22.0f;
    precalculated.swellAlpha =
        powf(
            (swellWindSpeed * swellWindSpeed) /
                (swellWindDistance * 9.8100004f),
            0.22f) *
        0.075999998f;

    const uint firstRow = taskIndex * 8u;
    for (uint rowOffset = 0; rowOffset < 8u; ++rowOffset)
    {
        const uint row = firstRow + rowOffset;
        if (row < static_cast<uint>(textureSize + 1))
        {
            const float2* gaussRow =
                m_gaussData +
                gaussOffset + 516u * (row + gaussOffset);
            float2* h0Row =
                m_omegaData0 + static_cast<size_t>(row) * rowWidth;
            const int halfTextureSize = textureSize / 2;
            const int yIndex = static_cast<int>(row) - halfTextureSize;
            const float yWaveNumber =
                (TwoPif / physicalSize) * static_cast<float>(yIndex);

            int xIndex = textureSize / -2;
            for (int column = 0; column <= textureSize; ++column, ++xIndex)
            {
                const float xWaveNumber =
                    (TwoPif / physicalSize) * static_cast<float>(xIndex);
                const float gridRadius = sqrtf(
                    static_cast<float>(
                        xIndex * xIndex + yIndex * yIndex));

                float bandMultiplier;
                if (m_activeCascade.lowerGridRadius > gridRadius ||
                    m_activeCascade.upperGridRadius <= gridRadius)
                {
                    bandMultiplier = 0.0f;
                }
                else
                {
                    bandMultiplier = 1.0f;
                }

                float amplitude;
                if (xWaveNumber == 0.0f && yWaveNumber == 0.0f)
                {
                    amplitude = 0.0f;
                }
                else
                {
                    amplitude = Jonswap(
                        static_cast<int>(
                            static_cast<size_t>(row) * rowWidth +
                            static_cast<size_t>(column)),
                        make_float2(xWaveNumber, yWaveNumber),
                        precalculated,
                        m_omegaData1,
                        baseWindAngle,
                        m_activeCascade.baseSpectrumPeaking,
                        m_activeCascade.baseWindDependency,
                        m_activeCascade.baseSmallWavesCutoffLength,
                        m_activeCascade.baseSmallWavesCutoffPower,
                        baseAmplitudeScale,
                        swellWindAngle,
                        m_activeCascade.swellSpectrumPeaking,
                        m_activeCascade.swellWindDependency,
                        m_activeCascade.swellSmallWavesCutoffLength,
                        m_activeCascade.swellSmallWavesCutoffPower,
                        swellAmplitudeScale);
                }

                amplitude *= bandMultiplier;
                h0Row[column].x = amplitude * gaussRow[column].x;
                h0Row[column].y = amplitude * gaussRow[column].y;
            }
        }
    }

    return static_cast<uint32_t>(interlockedDecrement(m_numUpdateH0Tasks));
}

int64_t Wind_Waves_FFT_Simulation_CPU_Impl::UpdateHt(uint taskIndex)
{
    // TODO: improve me using SIMD, see _Wind_Waves_FFT_Simulation_CPU_Impl.cpp:797-939.
    const uint N = m_activeCascade.textureSize;
    const uint rowWidth = N + 4u;
    const uint startRow = taskIndex * 16u;
    const uint endRow = startRow + 16u;
    const size_t planeStride = static_cast<size_t>(N) * N;
    const float halfN = static_cast<float>(N) * 0.5f;
    const double phaseScale = m_simulationTime / 6.28318530718;

    for (uint row = startRow; row < endRow; ++row)
    {
        float2* heightRow = m_fftBuffers + static_cast<size_t>(row) * N;
        float2* xDisplacementRow =
            m_fftBuffers + planeStride + static_cast<size_t>(row) * N;
        float2* yDisplacementRow =
            m_fftBuffers + 2u * planeStride + static_cast<size_t>(row) * N;
        const float* inverseRadiusRow = m_inverseRadius + static_cast<size_t>(row) * N;
        const float2* h0Row = m_omegaData0 + static_cast<size_t>(row) * rowWidth;
        const float2* omegaRow = m_omegaData1 + static_cast<size_t>(row) * rowWidth;
        const float2* mirroredH0Row =
            m_omegaData0 + static_cast<size_t>(N - row) * rowWidth;
        const float ky = static_cast<float>(row) - halfN;

        for (uint column = 0; column < N; column += 4u)
        {
            for (uint lane = 0; lane < 4u; ++lane)
            {
                const uint index = column + lane;
                const double phaseTurns =
                    static_cast<double>(omegaRow[index].x) * phaseScale;
                const float phase = static_cast<float>(
                    phaseTurns - static_cast<int>(phaseTurns)) *
                    TwoPif;

                float sine;
                float cosine;
                calculateSineAndCosine(phase, sine, cosine);

                const float2 h0 = h0Row[index];
                const float2 mirroredH0 = mirroredH0Row[N - index];
                const float sumReal = h0.x + mirroredH0.x;
                const float sumImaginary = h0.y + mirroredH0.y;
                const float differenceReal = h0.x - mirroredH0.x;
                const float differenceImaginary = h0.y - mirroredH0.y;
                const float heightReal =
                    sumReal * cosine - sumImaginary * sine;
                const float heightImaginary =
                    differenceImaginary * cosine + differenceReal * sine;

                heightRow[index] = make_float2(heightReal, heightImaginary);

                const float inverseRadius = inverseRadiusRow[index];
                const float imaginaryOverRadius =
                    heightImaginary * inverseRadius;
                const float realOverRadius = heightReal * inverseRadius;
                const float kx = static_cast<float>(index) - halfN;
                xDisplacementRow[index] =
                    make_float2(-kx * imaginaryOverRadius, kx * realOverRadius);
                yDisplacementRow[index] =
                    make_float2(-ky * imaginaryOverRadius, ky * realOverRadius);
            }
        }
    }

    return static_cast<uint32_t>(interlockedDecrement(m_numUpdateHtTasks));
}

WWRESULT Wind_Waves_FFT_Simulation_CPU_Impl::allocateAllResources()
{
    const uint N = m_activeCascade.textureSize;
    if (N == 0)
        return WWE_FAIL;

    const uint omegaCount = (N + 1u) * (N + 4u);

    if (m_displacementBuffers[0])
    {
        NVSDK_aligned_free(m_displacementBuffers[0]);
        m_displacementBuffers[0] = nullptr;
    }
    if (m_displacementBuffers[1])
    {
        NVSDK_aligned_free(m_displacementBuffers[1]);
        m_displacementBuffers[1] = nullptr;
    }

    if (m_activeCascade.writeDisplacementMap)
    {
        m_displacementBuffers[0] =
            reinterpret_cast<float4*>(NVSDK_aligned_malloc(16ull * N * N, 16));
        m_displacementBuffers[1] =
            reinterpret_cast<float4*>(NVSDK_aligned_malloc(16ull * N * N, 16));
    }
    m_currentDisplacementBuffer = nullptr;
    m_currentDisplacementBufferIndex = 0;

    if (m_archiveRing)
    {
        for (uint32_t i = 0; i < m_archiveRing->capacity; ++i)
        {
            freeAlignedArray(m_archiveRing->entries[i].buffer);
        }
        delete[] m_archiveRing->entries;
        delete m_archiveRing;
        m_archiveRing = nullptr;
    }

    if (m_activeCascade.writeDisplacementMap && m_activeCascade.archiveSlots)
    {
        const uint32_t archiveSlots = m_activeCascade.archiveSlots;
        m_archiveRing = new ArchiveRing();
        m_archiveRing->entries = new ArchiveEntry[archiveSlots];
        m_archiveRing->capacity = archiveSlots;
        m_archiveRing->startIndex = 0;
        m_archiveRing->count = 0;
        m_archiveRing->reserved = 0;

        for (uint32_t i = 0; i < archiveSlots; ++i)
        {
            m_archiveRing->entries[i].frameIndex = UINT64_MAX;
            m_archiveRing->entries[i].buffer =
                reinterpret_cast<uint8_t*>(NVSDK_aligned_malloc(16ull * N * N, 16));
        }
    }

    if (m_fftxBuffersFp16[0])
    {
        NVSDK_aligned_free(m_fftxBuffersFp16[0]);
        m_fftxBuffersFp16[0] = nullptr;
    }
    if (m_fftxBuffersFp16[1])
    {
        NVSDK_aligned_free(m_fftxBuffersFp16[1]);
        m_fftxBuffersFp16[1] = nullptr;
    }

    m_fftxBuffersFp16[0] = reinterpret_cast<uint16_t*>(NVSDK_aligned_malloc(8ull * N * N, 16));
    m_fftxBuffersFp16[1] = reinterpret_cast<uint16_t*>(NVSDK_aligned_malloc(8ull * N * N, 16));
    m_fftxRowPitch = 8u * N;

    if (m_gaussData)
    {
        NVSDK_aligned_free(m_gaussData);
        m_gaussData = nullptr;
    }
    if (m_omegaData0)
    {
        NVSDK_aligned_free(m_omegaData0);
        m_omegaData0 = nullptr;
    }
    if (m_omegaData1)
    {
        NVSDK_aligned_free(m_omegaData1);
        m_omegaData1 = nullptr;
    }

    m_gaussData = reinterpret_cast<float2*>(NVSDK_aligned_malloc(0x205020u, 16));
    m_omegaData0 = reinterpret_cast<float2*>(NVSDK_aligned_malloc(8ull * omegaCount, 16));
    m_omegaData1 = reinterpret_cast<float2*>(NVSDK_aligned_malloc(8ull * omegaCount, 16));

    if (m_gaussData)
    {
        Simulation_Util::initGaussData(m_gaussData);
    }

    if (m_omegaData1)
    {
        Simulation_Util::initOmegaData(m_activeCascade, m_omegaData1);
    }

    if (m_inverseRadius)
    {
        NVSDK_aligned_free(m_inverseRadius);
        m_inverseRadius = nullptr;
    }
    m_inverseRadius = reinterpret_cast<float*>(NVSDK_aligned_malloc(4ull * N * N, 16));
    if (m_inverseRadius)
    {
        const float halfN = static_cast<float>(N) * 0.5f;
        for (uint row = 0; row < N; ++row)
        {
            const float y = static_cast<float>(row) - halfN;
            const float y2 = y * y;
            for (uint col = 0; col < N; ++col)
            {
                const float x = static_cast<float>(col) - halfN;
                const float r2 = x * x + y2;
                m_inverseRadius[static_cast<size_t>(row) * N + col] =
                    r2 > 1.0e-12f ? 1.0f / sqrtf(r2) : 0.0f;
            }
        }
    }

    if (m_fftBuffers)
    {
        NVSDK_aligned_free(m_fftBuffers);
        m_fftBuffers = nullptr;
    }
    m_fftBuffers = reinterpret_cast<float2*>(NVSDK_aligned_malloc(24ull * N * N, 16));

    if (m_useCpu)
    {
        m_displacementTexture = nullptr;
        m_staging[0] = nullptr;
        // The recovered cleanup visits slot 0 twice; slot 1 stays owned until replacement.

        nvrhi::TextureDesc desc;
        desc.width = N;
        desc.height = N;
        desc.depth = 1;
        desc.arraySize = 1;
        desc.mipLevels = 1;
        desc.sampleCount = 1;
        desc.sampleQuality = 0;
        desc.format = nvrhi::Format::RGBA16_FLOAT;
        desc.dimension = nvrhi::TextureDimension::Texture2D;
        desc.isUAV = true;
        desc.debugName = "Displacement map updated by CPU";
        desc.initialState = nvrhi::ResourceStates::ShaderResource;
        desc.keepInitialState = true;

        m_device->createTexture(desc, &m_displacementTexture);
        if (m_displacementTexture && m_device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN && desc.keepInitialState) {
            // Establish the native image layout before exposing it to the renderer.
            nvrhi::CommandListHandle initializeCommands;
            m_device->createCommandList(nvrhi::CommandListParameters(), &initializeCommands);
            if (!initializeCommands) return WWE_FAIL;
            initializeCommands->open();
            initializeCommands->beginTrackingTextureState(m_displacementTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);
            initializeCommands->setTextureState(m_displacementTexture, nvrhi::AllSubresources, desc.initialState);
            initializeCommands->commitBarriers();
            initializeCommands->close();
            m_device->executeCommandList(initializeCommands);
        }
        if (!m_displacementTexture)
            return WWE_FAIL;

        desc.debugName = "Displacement staging map #0 updated by CPU";
        m_device->createStagingTexture(desc, nvrhi::CpuAccessMode::Write, &m_staging[0]);
        desc.debugName = "Displacement staging map #1 updated by CPU";
        m_device->createStagingTexture(desc, nvrhi::CpuAccessMode::Write, &m_staging[1]);
        if (!m_staging[0] || !m_staging[1])
            return WWE_FAIL;
    }

    m_hasDisplacementMap = true;
    m_displacementMapVersion = UINT64_MAX;
    return WWE_OK;
}

WWRESULT Wind_Waves_FFT_Simulation_CPU_Impl::archiveDisplacements(uint64_t frameIndex)
{
    if (!m_currentDisplacementBuffer || !m_archiveRing)
        return WWE_OK;

    if (m_archiveRing->count)
    {
        const uint32_t lastIndex = (m_archiveRing->startIndex + m_archiveRing->count - 1)
                                   % m_archiveRing->capacity;
        if (frameIndex == m_archiveRing->entries[lastIndex].frameIndex)
            return WWE_FAIL;
    }

    if (m_archiveRing->count == m_archiveRing->capacity)
    {
        m_archiveRing->startIndex =
            (m_archiveRing->startIndex + 1) % m_archiveRing->capacity;
        --m_archiveRing->count;
    }

    const uint32_t slotIndex =
        (m_archiveRing->startIndex + m_archiveRing->count) % m_archiveRing->capacity;
    uint8_t* archivedBuffer = m_archiveRing->entries[slotIndex].buffer;

    m_archiveRing->entries[slotIndex].frameIndex = frameIndex;
    m_archiveRing->entries[slotIndex].buffer = m_currentDisplacementBuffer;

    const uint32_t completedBufferIndex = (m_currentDisplacementBufferIndex - 1u) & 1u;
    m_displacementBuffers[completedBufferIndex] = reinterpret_cast<float4*>(archivedBuffer);

    ++m_archiveRing->count;
    return WWE_OK;
}

void Wind_Waves_FFT_Simulation_CPU_Impl::calcReinit(
    const Wind_Waves_Detailed_Simulation_Parameters::Cascade& cascade,
    bool& outRestartAll,
    bool& outReallocateResources,
    bool& outRecreateDisplacementMap,
    bool& outRefreshOmega)
{
    outRestartAll = false;
    outReallocateResources = false;
    outRefreshOmega = false;
    outRecreateDisplacementMap = false;

    const auto& current = m_hasPendingCascade ? m_pendingCascade : m_activeCascade;

    if (cascade.textureSize != current.textureSize ||
        cascade.writeDisplacementMap != current.writeDisplacementMap ||
        (cascade.writeDisplacementMap && cascade.archiveSlots != current.archiveSlots))
    {
        outRestartAll = true;
        outReallocateResources = true;
    }

    if (cascade.physicalSize != current.physicalSize ||
        cascade.textureSize != current.textureSize)
    {
        outRefreshOmega = true;
    }

    if (cascade.baseWindSpeed != current.baseWindSpeed ||
        cascade.baseWindDistance != current.baseWindDistance ||
        cascade.baseWindDependency != current.baseWindDependency ||
        cascade.baseWindDirection.x != current.baseWindDirection.x ||
        cascade.baseWindDirection.y != current.baseWindDirection.y ||
        cascade.baseSpectrumPeaking != current.baseSpectrumPeaking ||
        cascade.baseSmallWavesCutoffLength != current.baseSmallWavesCutoffLength ||
        cascade.baseSmallWavesCutoffPower != current.baseSmallWavesCutoffPower ||
        cascade.baseAmplitudeMultiplier != current.baseAmplitudeMultiplier ||
        cascade.swellWindDirection.x != current.swellWindDirection.x ||
        cascade.swellWindDirection.y != current.swellWindDirection.y ||
        cascade.swellWindSpeed != current.swellWindSpeed ||
        cascade.swellWindDistance != current.swellWindDistance ||
        cascade.swellWindDependency != current.swellWindDependency ||
        cascade.swellSpectrumPeaking != current.swellSpectrumPeaking ||
        cascade.swellSmallWavesCutoffLength != current.swellSmallWavesCutoffLength ||
        cascade.swellSmallWavesCutoffPower != current.swellSmallWavesCutoffPower ||
        cascade.swellAmplitudeMultiplier != current.swellAmplitudeMultiplier ||
        cascade.lowerGridRadius != current.lowerGridRadius ||
        cascade.upperGridRadius != current.upperGridRadius ||
        outRefreshOmega)
    {
        outRecreateDisplacementMap = true;
    }
}

void Wind_Waves_FFT_Simulation_CPU_Impl::closeAndExecuteCmdList()
{
    if (m_cmdList && m_device)
    {
        m_cmdList->close();
        m_device->executeCommandList(m_cmdList);
    }
}

WWRESULT Wind_Waves_FFT_Simulation_CPU_Impl::getDisplacementArchiveCoords(
    float time, int* prevIndex, int* nextIndex, float* interpolation) const
{
    if (!m_archiveRing || m_archiveRing->count == 0)
        return WWE_NONE;

    const uint lastIndex = m_archiveRing->count - 1;
    const float clamped = std::fminf(static_cast<float>(lastIndex),
                                     std::fmaxf(0.0f, time));
    const float base = std::floor(clamped);
    const int prev = static_cast<int>(base);
    int next = prev + (clamped - base != 0.0f ? 1 : 0);
    if ((uint)next > lastIndex)
        next = static_cast<int>(lastIndex);

    if (prevIndex)
        *prevIndex = prev;
    if (nextIndex)
        *nextIndex = next;
    if (interpolation)
        *interpolation = clamped - base;

    return WWE_OK;
}

nvrhi::TextureHandle Wind_Waves_FFT_Simulation_CPU_Impl::getDisplacementMap() const
{
    if (m_useCpu)
        return m_displacementTexture;
    return nullptr;
}

uint64_t Wind_Waves_FFT_Simulation_CPU_Impl::getDisplacementMapVersion() const
{
    return m_displacementMapVersion;
}

WWRESULT Wind_Waves_FFT_Simulation_CPU_Impl::init(
    nvrhi::IDevice* device,
    nvrhi::CommandListHandle cmdList,
    bool useCpu)
{
    m_useCpu = useCpu;
    m_device = useCpu ? device : nullptr;
    m_cmdList = useCpu ? std::move(cmdList) : nullptr;

    const WWRESULT result = allocateAllResources();
    if (result < 0)
        return result;
    return WWE_OK;
}

void Wind_Waves_FFT_Simulation_CPU_Impl::initGaussAndOmega()
{
    if (m_gaussData)
        Simulation_Util::initGaussData(m_gaussData);
    if (m_omegaData1)
        Simulation_Util::initOmegaData(m_activeCascade, m_omegaData1);
}

WWRESULT Wind_Waves_FFT_Simulation_CPU_Impl::mapDisplacementArchiveData(
    uint8_t** data,
    uint32_t* rowPitch,
    uint32_t age,
    bool* outReadOnly)
{
    const uint32_t index =
        (m_archiveRing->startIndex + m_archiveRing->count - age - 1u) %
        m_archiveRing->capacity;
    *data = m_archiveRing->entries[index].buffer;
    *rowPitch = 16u * m_activeCascade.textureSize;
    *outReadOnly = false;
    return WWE_OK;
}

WWRESULT Wind_Waves_FFT_Simulation_CPU_Impl::mapDisplacementData(
    uint8_t** data, uint32_t* rowPitch, bool* outReadOnly)
{
    if (!m_currentDisplacementBuffer)
        return WWE_NONE;

    *data = m_currentDisplacementBuffer;
    *rowPitch = 16u * m_activeCascade.textureSize;
    *outReadOnly = false;
    return WWE_OK;
}

void Wind_Waves_FFT_Simulation_CPU_Impl::openCmdList()
{
    m_cmdList->open();
}

WWRESULT Wind_Waves_FFT_Simulation_CPU_Impl::reinit(
    const Wind_Waves_Detailed_Simulation_Parameters::Cascade& cascade)
{
    bool restartAll = false;
    bool reallocateResources = false;
    bool refreshOmega = false;
    bool recreateDisplacementMap = false;

    calcReinit(cascade, restartAll, reallocateResources,
               recreateDisplacementMap, refreshOmega);

    if (m_useDelayedCascade)
    {
        m_pendingCascade = cascade;
        m_hasPendingCascade = true;
    }
    else
    {
        m_activeCascade = cascade;
    }

    if (restartAll)
    {
        releaseAllResources();
    }

    if (reallocateResources)
    {
        const WWRESULT result = allocateAllResources();
        if (result < 0)
            return result;
    }
    else if (refreshOmega)
    {
        initGaussAndOmega();
    }

    if (recreateDisplacementMap)
        m_hasDisplacementMap = true;

    m_useDelayedCascade = false;
    return WWE_OK;
}

void Wind_Waves_FFT_Simulation_CPU_Impl::releaseAll()
{
    releaseAllResources();
}

void Wind_Waves_FFT_Simulation_CPU_Impl::releaseAllResources()
{
    if (m_useCpu && m_device)
        m_device->waitForIdle();

    freeAlignedArray(m_inverseRadius);
    freeAlignedArray(m_gaussData);
    freeAlignedArray(m_omegaData0);
    freeAlignedArray(m_omegaData1);
    freeAlignedArray(m_fftBuffers);
    freeAlignedArray(m_displacementBuffers[0]);
    freeAlignedArray(m_displacementBuffers[1]);
    m_currentDisplacementBuffer = nullptr;

    if (m_archiveRing)
    {
        for (uint32_t i = 0; i < m_archiveRing->capacity; ++i)
        {
            freeAlignedArray(m_archiveRing->entries[i].buffer);
        }
        delete[] m_archiveRing->entries;
        delete m_archiveRing;
        m_archiveRing = nullptr;
    }

    freeAlignedArray(m_fftxBuffersFp16[0]);
    freeAlignedArray(m_fftxBuffersFp16[1]);
    m_currentDisplacementBufferIndex = 0;
}

void Wind_Waves_FFT_Simulation_CPU_Impl::unmapDisplacementArchiveData(uint)
{
}

void Wind_Waves_FFT_Simulation_CPU_Impl::unmapDisplacementData()
{
}
