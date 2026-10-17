#include "Local_Waves_FFT_Simulation_CPU_Impl.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

#include "CustomMemory.h"
#include "Simulation_Util.h"

namespace
{
int64_t nowNanoseconds()
{
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               Clock::now().time_since_epoch())
        .count();
}

uint16_t floatToHalfBitsTruncate(float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));

    const uint32_t exponent = (bits >> 23) & 0xffu;
    const uint32_t halfExponent =
        (((0x70u - exponent) >> 16) & 0x1fu & (exponent - 0x70u)) |
        ((bits >> 31) << 5);

    return static_cast<uint16_t>((halfExponent << 10) | ((bits >> 13) & 0x3ffu));
}

template <typename T>
T* allocateAlignedArray(size_t count)
{
    return reinterpret_cast<T*>(NVSDK_aligned_malloc(sizeof(T) * count, 16));
}

template <typename T>
void freeAlignedArray(T*& ptr)
{
    if (ptr)
    {
        NVSDK_aligned_free(ptr);
        ptr = nullptr;
    }
}

void clearFloat2(float2* data, size_t count)
{
    if (data)
        std::memset(data, 0, sizeof(float2) * count);
}
}

Local_Waves_FFT_Simulation_CPU_Impl::Local_Waves_FFT_Simulation_CPU_Impl(
    const Local_Waves_Detailed_Simulation_Parameters& params,
    ThreadPool* threadPool)
    : m_simulationTime(0.0f)
    , m_params(params)
    , m_threadPool(threadPool)
    , m_enableCpuTimers(params.enableCpuTimers)
    , m_mainThreadWaitNs(0)
    , m_currentWorkerThreadNs(0)
    , m_completedWorkerThreadNs(0)
    , m_numFFT_X_NForwardTasks(0)
    , m_numFFT_Y_NForwardTasks(0)
    , m_numGenerateEvolvingOperatorsTasks(0)
    , m_numApplyEvolvingOperatorsTasks(0)
    , m_numFFT_X_NReverseTasks(0)
    , m_numFFT_Y_NReverseTasks(0)
    , m_numGenerateTextureTasks(0)
    , m_padding1(0)
    , m_nextKickId(0)
    , m_tasksInFlight(false)
    , m_stagingPending(false)
    , m_stagedKickId(0)
    , m_cursorValid(false)
    , m_cursorKickId(0)
    , m_resetPending(false)
    , m_device(nullptr)
    , m_cmdList(nullptr)
    , m_useCpu(false)
    , m_displacementTexture(nullptr)
    , m_staging{}
    , m_initialX(nullptr)
    , m_initialY(nullptr)
    , m_height(nullptr)
    , m_slope(nullptr)
    , m_displacementX(nullptr)
    , m_displacementY(nullptr)
    , m_outputFp16{}
    , m_outputRowPitch(0)
    , m_displacementBuffers{}
    , m_currentDisplacementBuffer(nullptr)
    , m_currentDisplacementBufferIndex(0)
    , m_operatorSinOmega(nullptr)
    , m_operatorCos(nullptr)
    , m_operatorSinInvOmega(nullptr)
    , m_displacementMapVersion(std::numeric_limits<uint64_t>::max())
{
}

Local_Waves_FFT_Simulation_CPU_Impl::~Local_Waves_FFT_Simulation_CPU_Impl()
{
    if (m_tasksInFlight)
    {
        waitTasksCompletion();
        m_stagingPending = false;
    }

    releaseAllResources();
}

WWRESULT Local_Waves_FFT_Simulation_CPU_Impl::advanceReadbackCursor(bool)
{
    return WWE_NONE;
}

WWRESULT Local_Waves_FFT_Simulation_CPU_Impl::advanceStagingCursor(bool block)
{
    if (m_tasksInFlight)
    {
        if (!block && m_threadPool && !m_threadPool->isLowPriorityWorkDone())
            return WWE_WOULD_BLOCK;

        waitTasksCompletion();
    }
    else if (!m_stagingPending)
    {
        return WWE_NONE;
    }

    if (m_useCpu && m_device && m_cmdList)
    {
        m_cmdList->open();
        flip();
        m_cmdList->close();
        m_device->executeCommandList(m_cmdList);
    }
    else
    {
        flip();
    }

    return WWE_OK;
}

WWRESULT Local_Waves_FFT_Simulation_CPU_Impl::beforeReinit()
{
    if (m_tasksInFlight)
    {
        waitTasksCompletion();
        m_stagingPending = false;
    }

    m_cursorValid = false;
    return WWE_OK;
}

void Local_Waves_FFT_Simulation_CPU_Impl::completeSimulationStep(uint64_t frameIndex)
{
    uploadCompletedBuffer(frameIndex);

    const uint32_t completedIndex = m_currentDisplacementBufferIndex;
    m_currentDisplacementBuffer =
        reinterpret_cast<uint8_t*>(m_displacementBuffers[completedIndex]);
    m_displacementMapVersion = frameIndex;
    m_currentDisplacementBufferIndex = (completedIndex - 1u) & 1u;
}

nvrhi::TextureHandle Local_Waves_FFT_Simulation_CPU_Impl::getDisplacementMap() const
{
    return m_useCpu ? m_displacementTexture : nullptr;
}

uint64_t Local_Waves_FFT_Simulation_CPU_Impl::getDisplacementMapVersion() const
{
    return m_displacementMapVersion;
}

int64_t Local_Waves_FFT_Simulation_CPU_Impl::getNumApplyEvolvingOperatorsTasks() const
{
    return static_cast<int64_t>(m_params.textureSize >> 3);
}

int64_t Local_Waves_FFT_Simulation_CPU_Impl::getNumFFTTasks() const
{
    return static_cast<int64_t>(m_params.textureSize >> 5);
}

int64_t Local_Waves_FFT_Simulation_CPU_Impl::getNumGenerateEvolvingOperatorsTasks() const
{
    return static_cast<int64_t>(m_params.textureSize >> 4);
}

int64_t Local_Waves_FFT_Simulation_CPU_Impl::getNumGenerateTextureTasks() const
{
    return static_cast<int64_t>(m_params.textureSize >> 3);
}

bool Local_Waves_FFT_Simulation_CPU_Impl::getReadbackCursor(uint64_t* cursor) const
{
    if (cursor && m_cursorValid)
        *cursor = m_cursorKickId;
    return m_cursorValid;
}

bool Local_Waves_FFT_Simulation_CPU_Impl::getStagingCursor(uint64_t* cursor) const
{
    if (cursor && m_cursorValid)
        *cursor = m_cursorKickId;
    return m_cursorValid;
}

WWRESULT Local_Waves_FFT_Simulation_CPU_Impl::getTimings(
    GFSDK_WaveWorks_Local_Waves_Simulation_Stats& stats) const
{
    stats.CPU_main_thread_wait_time = static_cast<float>(m_mainThreadWaitNs) / 1000000.0f;
    stats.CPU_threads_total_time =
        static_cast<float>(m_completedWorkerThreadNs) / 1000000.0f;
    stats.GPU_simulation_time = 0.0f;
    return WWE_OK;
}

int64_t Local_Waves_FFT_Simulation_CPU_Impl::getTotalNumberOfTasks() const
{
    const uint N = m_params.textureSize;
    return static_cast<int64_t>(
        (N >> 4) + 2u * ((N >> 3) + 2u * (N >> 5)));
}

WWRESULT Local_Waves_FFT_Simulation_CPU_Impl::init(
    nvrhi::IDevice* device,
    nvrhi::CommandListHandle cmdList,
    bool useCpu)
{
    m_useCpu = useCpu;
    m_device = useCpu ? device : nullptr;
    m_cmdList = useCpu ? std::move(cmdList) : nullptr;

    const WWRESULT result = allocateAllResources();
    return result < 0 ? result : WWE_OK;
}

WWRESULT Local_Waves_FFT_Simulation_CPU_Impl::kick(
    float simulationTime,
    float2* disturbanceData,
    uint64_t& kickId)
{
    kickId = m_nextKickId;
    m_simulationTime = simulationTime;

    if (m_tasksInFlight)
    {
        waitTasksCompletion();
        flip();
    }
    else if (m_stagingPending)
    {
        flip();
    }

    const uint N = m_params.textureSize;
    const uint sampleCount = N * N;
    if (m_resetPending)
    {
        std::memset(m_initialX, 0, sizeof(float2) * sampleCount);
        std::memset(m_initialY, 0, sizeof(float2) * sampleCount);
        m_resetPending = false;
    }

    for (uint i = 0; i < sampleCount; ++i)
    {
        m_initialX[i].x += disturbanceData[i].x;
        m_initialY[i].x += disturbanceData[i].y;
        disturbanceData[i] = make_float2(0.0f, 0.0f);
    }

    m_currentWorkerThreadNs.store(0);
    resetTaskCounters();

    const uint totalTaskCount = static_cast<uint>(getTotalNumberOfTasks());
    std::vector<std::function<void()>> tasks;
    const uint taskCount = static_cast<uint>(getNumFFTTasks());
    for (uint taskIndex = 0; taskIndex < taskCount; ++taskIndex)
    {
        tasks.emplace_back([this, taskIndex]() {
            computeFFT_X_NForwardTask(taskIndex);
        });
    }
    m_threadPool->submitLowPriorityVector(std::move(tasks), totalTaskCount);

    m_tasksInFlight = true;
    m_stagedKickId = m_nextKickId;
    ++m_nextKickId;

    return WWE_OK;
}

WWRESULT Local_Waves_FFT_Simulation_CPU_Impl::mapDisplacementData(
    uint8_t** data,
    uint32_t* rowPitch,
    bool* fp16)
{
    if (!m_currentDisplacementBuffer)
        return WWE_NONE;

    *data = m_currentDisplacementBuffer;
    *rowPitch = 16u * m_params.textureSize;
    *fp16 = false;
    return WWE_OK;
}

WWRESULT Local_Waves_FFT_Simulation_CPU_Impl::prepareSimulationStep()
{
    resetTaskCounters();
    return WWE_OK;
}

WWRESULT Local_Waves_FFT_Simulation_CPU_Impl::reinit(
    const Local_Waves_Detailed_Simulation_Parameters& params)
{
    if (m_threadPool)
    {
        m_threadPool->waitForHighPriorityWorkDone();
        m_threadPool->waitForLowPriorityWorkDone();
    }

    const uint oldTextureSize = m_params.textureSize;
    m_params = params;

    if (params.textureSize == oldTextureSize)
    {
        return WWE_OK;
    }

    releaseAllResources();
    const WWRESULT result = allocateAllResources();
    return result < 0 ? result : WWE_OK;
}

void Local_Waves_FFT_Simulation_CPU_Impl::releaseAllResources()
{
    if (m_useCpu && m_device)
        m_device->waitForIdle();

    freeAlignedArray(m_displacementBuffers[0]);
    freeAlignedArray(m_displacementBuffers[1]);
    freeAlignedArray(m_outputFp16[0]);
    freeAlignedArray(m_outputFp16[1]);
    freeAlignedArray(m_initialX);
    freeAlignedArray(m_initialY);
    freeAlignedArray(m_height);
    freeAlignedArray(m_slope);
    freeAlignedArray(m_displacementX);
    freeAlignedArray(m_displacementY);
    freeAlignedArray(m_operatorCos);
    freeAlignedArray(m_operatorSinOmega);
    freeAlignedArray(m_operatorSinInvOmega);

    m_currentDisplacementBuffer = nullptr;
    m_currentDisplacementBufferIndex = 0;
    m_displacementTexture = nullptr;
    m_staging[0] = nullptr;
    m_staging[1] = nullptr;
}

void Local_Waves_FFT_Simulation_CPU_Impl::reset()
{
    m_resetPending = true;
}

void Local_Waves_FFT_Simulation_CPU_Impl::unmapDisplacementData()
{
}

WWRESULT Local_Waves_FFT_Simulation_CPU_Impl::waitStagingCursor()
{
    if (!m_tasksInFlight)
        return WWE_NONE;

    waitTasksCompletion();
    return WWE_OK;
}

void Local_Waves_FFT_Simulation_CPU_Impl::waitTasksCompletion()
{
    int64_t startNs = 0;
    if (m_enableCpuTimers)
        startNs = nowNanoseconds();

    if (m_threadPool)
        m_threadPool->waitForLowPriorityWorkDone();

    if (m_enableCpuTimers)
        m_mainThreadWaitNs = static_cast<uint64_t>(std::max<int64_t>(0, nowNanoseconds() - startNs));

    m_tasksInFlight = false;
    m_stagingPending = true;
}

void Local_Waves_FFT_Simulation_CPU_Impl::applyEvolvingOperators(
    uint startRow,
    uint rowCount)
{
    const uint N = m_params.textureSize;
    const uint endRow = startRow + rowCount;

    for (uint row = startRow; row < endRow; ++row)
    {
        const float ky = row >= (N >> 1)
                             ? static_cast<float>(row) - static_cast<float>(N)
                             : static_cast<float>(row);

        for (uint col = 0; col < N; ++col)
        {
            const uint idx = row * N + col;
            const float kx = col >= (N >> 1)
                                 ? static_cast<float>(col) - static_cast<float>(N)
                                 : static_cast<float>(col);

            const float cosTerm = m_operatorCos[idx];
            const float sinOmegaTerm = m_operatorSinOmega[idx];
            const float sinInvOmegaTerm = m_operatorSinInvOmega[idx];
            const float2 inputX = m_initialX[idx];
            const float2 inputY = m_initialY[idx];

            m_height[idx].x = inputX.x * cosTerm + sinOmegaTerm * inputY.x;
            m_height[idx].y = inputY.y * sinOmegaTerm + inputX.y * cosTerm;
            m_slope[idx].x = inputY.x * cosTerm - inputX.x * sinInvOmegaTerm;
            m_slope[idx].y = inputY.y * cosTerm - inputX.y * sinInvOmegaTerm;

            const float radius = std::sqrt(kx * kx + ky * ky);
            const float invRadius = radius > 0.0f ? 1.0f / radius : 0.0f;
            const float heightRe = m_height[idx].x;
            const float heightIm = m_height[idx].y;
            m_displacementX[idx] =
                make_float2(-kx * heightIm * invRadius, kx * heightRe * invRadius);
            m_displacementY[idx] =
                make_float2(-ky * heightIm * invRadius, ky * heightRe * invRadius);
        }
    }
}

void Local_Waves_FFT_Simulation_CPU_Impl::applyEvolvingOperatorsTask(uint taskIndex)
{
    const int64_t startNs = m_enableCpuTimers ? nowNanoseconds() : 0;
    applyEvolvingOperators(8u * taskIndex, 8u);

    if (m_numApplyEvolvingOperatorsTasks.fetch_sub(1) == 1 && m_threadPool)
    {
        const uint taskCount = static_cast<uint>(getNumFFTTasks());
        std::vector<std::function<void()>> tasks;
        tasks.reserve(taskCount);
        for (uint taskIndex = 0; taskIndex < taskCount; ++taskIndex)
            tasks.emplace_back([this, taskIndex]() {
                computeFFT_X_NReverseTask(taskIndex);
            });
        m_threadPool->enqueueLowPriorityVector(std::move(tasks));
    }

    if (m_enableCpuTimers)
        m_currentWorkerThreadNs.fetch_add(std::max<int64_t>(0, nowNanoseconds() - startNs));
}

WWRESULT Local_Waves_FFT_Simulation_CPU_Impl::allocateAllResources()
{
    const uint N = m_params.textureSize;
    if (N == 0)
        return WWE_FAIL;

    const size_t sampleCount = static_cast<size_t>(N) * N;

    freeAlignedArray(m_initialX);
    freeAlignedArray(m_initialY);
    freeAlignedArray(m_height);
    freeAlignedArray(m_slope);
    freeAlignedArray(m_displacementX);
    freeAlignedArray(m_displacementY);
    freeAlignedArray(m_operatorCos);
    freeAlignedArray(m_operatorSinOmega);
    freeAlignedArray(m_operatorSinInvOmega);

    m_initialX = allocateAlignedArray<float2>(sampleCount);
    m_initialY = allocateAlignedArray<float2>(sampleCount);
    m_height = allocateAlignedArray<float2>(sampleCount);
    m_slope = allocateAlignedArray<float2>(sampleCount);
    m_displacementX = allocateAlignedArray<float2>(sampleCount);
    m_displacementY = allocateAlignedArray<float2>(sampleCount);
    m_operatorCos = allocateAlignedArray<float>(sampleCount);
    m_operatorSinOmega = allocateAlignedArray<float>(sampleCount);
    m_operatorSinInvOmega = allocateAlignedArray<float>(sampleCount);

    if (!m_initialX || !m_initialY || !m_height || !m_slope ||
        !m_displacementX || !m_displacementY || !m_operatorSinOmega ||
        !m_operatorCos || !m_operatorSinInvOmega)
    {
        return WWE_FAIL;
    }

    clearFloat2(m_initialX, sampleCount);
    clearFloat2(m_initialY, sampleCount);
    clearFloat2(m_height, sampleCount);
    clearFloat2(m_slope, sampleCount);
    clearFloat2(m_displacementX, sampleCount);
    clearFloat2(m_displacementY, sampleCount);

    freeAlignedArray(m_displacementBuffers[0]);
    freeAlignedArray(m_displacementBuffers[1]);
    if (m_params.enableCpuDrivenDisplacementCalculation)
    {
        m_displacementBuffers[0] = allocateAlignedArray<float4>(sampleCount);
        m_displacementBuffers[1] = allocateAlignedArray<float4>(sampleCount);
        if (!m_displacementBuffers[0] || !m_displacementBuffers[1])
            return WWE_FAIL;
    }

    m_currentDisplacementBuffer = nullptr;
    freeAlignedArray(m_outputFp16[0]);
    freeAlignedArray(m_outputFp16[1]);
    m_outputFp16[0] = allocateAlignedArray<uint16_t>(4ull * sampleCount);
    m_outputFp16[1] = allocateAlignedArray<uint16_t>(4ull * sampleCount);
    m_outputRowPitch = 8u * N;
    if (!m_outputFp16[0] || !m_outputFp16[1])
        return WWE_FAIL;

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
        uint32_t mipExponent = 0;
        uint32_t mipPower = 0;
        Simulation_Util::powerOf2(N, &mipExponent, &mipPower);
        desc.mipLevels = mipExponent + 1;
        desc.sampleCount = 1;
        desc.sampleQuality = 0;
        desc.format = nvrhi::Format::RGBA16_FLOAT;
        desc.dimension = nvrhi::TextureDimension::Texture2D;
        desc.debugName = "Local waves displacement map updated by CPU";
        desc.isUAV = true;
        // Rendering wraps this image on a separate NVRHI device before the
        // first CPU upload, so its public layout must already be readable.
        desc.initialState = nvrhi::ResourceStates::ShaderResource;
        desc.keepInitialState = true;

        if (!m_device) return WWE_FAIL;
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

        desc.mipLevels = 1;
        desc.initialState = nvrhi::ResourceStates::Common;
        desc.keepInitialState = false;
        desc.debugName = "Local waves displacement staging map #0 updated by CPU";
        m_device->createStagingTexture(desc, nvrhi::CpuAccessMode::Write, &m_staging[0]);
        desc.debugName = "Local waves displacement staging map #1 updated by CPU";
        m_device->createStagingTexture(desc, nvrhi::CpuAccessMode::Write, &m_staging[1]);
        if (!m_staging[0] || !m_staging[1])
            return WWE_FAIL;
    }

    m_displacementMapVersion = std::numeric_limits<uint64_t>::max();
    return WWE_OK;
}

void Local_Waves_FFT_Simulation_CPU_Impl::computeFFT_X_NForwardTask(uint taskIndex)
{
    const int64_t startNs = m_enableCpuTimers ? nowNanoseconds() : 0;
    const uint N = m_params.textureSize;

    for (uint i = 0; i < 8u; ++i)
    {
        const size_t offset = static_cast<size_t>(4u * N) * (8u * taskIndex + i);
        Simulation_Util::FFT1DSIMD_X_4wide(m_initialX + offset, N, 1);
        Simulation_Util::FFT1DSIMD_X_4wide(m_initialY + offset, N, 1);
    }

    if (m_numFFT_X_NForwardTasks.fetch_sub(1) == 1 && m_threadPool)
    {
        const uint taskCount = static_cast<uint>(getNumFFTTasks());
        std::vector<std::function<void()>> tasks;
        tasks.reserve(taskCount);
        for (uint taskIndex = 0; taskIndex < taskCount; ++taskIndex)
            tasks.emplace_back([this, taskIndex]() {
                computeFFT_Y_NForwardTask(taskIndex);
            });
        m_threadPool->enqueueLowPriorityVector(std::move(tasks));
    }

    if (m_enableCpuTimers)
        m_currentWorkerThreadNs.fetch_add(std::max<int64_t>(0, nowNanoseconds() - startNs));
}

void Local_Waves_FFT_Simulation_CPU_Impl::computeFFT_X_NReverseTask(uint taskIndex)
{
    const int64_t startNs = m_enableCpuTimers ? nowNanoseconds() : 0;
    const uint N = m_params.textureSize;

    for (uint i = 0; i < 8u; ++i)
    {
        const size_t offset = static_cast<size_t>(4u * N) * (8u * taskIndex + i);
        Simulation_Util::FFT1DSIMD_X_4wide(m_height + offset, N, -1);
        Simulation_Util::FFT1DSIMD_X_4wide(m_slope + offset, N, -1);
        Simulation_Util::FFT1DSIMD_X_4wide(m_displacementX + offset, N, -1);
        Simulation_Util::FFT1DSIMD_X_4wide(m_displacementY + offset, N, -1);
    }

    if (m_numFFT_X_NReverseTasks.fetch_sub(1) == 1 && m_threadPool)
    {
        const uint taskCount = static_cast<uint>(getNumFFTTasks());
        std::vector<std::function<void()>> tasks;
        tasks.reserve(taskCount);
        for (uint taskIndex = 0; taskIndex < taskCount; ++taskIndex)
            tasks.emplace_back([this, taskIndex]() {
                computeFFT_Y_NReverseTask(taskIndex);
            });
        m_threadPool->enqueueLowPriorityVector(std::move(tasks));
    }

    if (m_enableCpuTimers)
        m_currentWorkerThreadNs.fetch_add(std::max<int64_t>(0, nowNanoseconds() - startNs));
}

void Local_Waves_FFT_Simulation_CPU_Impl::computeFFT_Y_NForwardTask(uint taskIndex)
{
    const int64_t startNs = m_enableCpuTimers ? nowNanoseconds() : 0;
    const uint N = m_params.textureSize;

    for (uint i = 0; i < 8u; ++i)
    {
        const size_t offset = static_cast<size_t>(4u) * (8u * taskIndex + i);
        Simulation_Util::FFT1DSIMD_Y_4wide(m_initialX + offset, N, 1);
        Simulation_Util::FFT1DSIMD_Y_4wide(m_initialY + offset, N, 1);
    }

    if (m_numFFT_Y_NForwardTasks.fetch_sub(1) == 1 && m_threadPool)
    {
        const uint taskCount = static_cast<uint>(getNumGenerateEvolvingOperatorsTasks());
        std::vector<std::function<void()>> tasks;
        tasks.reserve(taskCount);
        for (uint taskIndex = 0; taskIndex < taskCount; ++taskIndex)
            tasks.emplace_back([this, taskIndex]() {
                generateEvolvingOperatorsTask(taskIndex);
            });
        m_threadPool->enqueueLowPriorityVector(std::move(tasks));
    }

    if (m_enableCpuTimers)
        m_currentWorkerThreadNs.fetch_add(std::max<int64_t>(0, nowNanoseconds() - startNs));
}

void Local_Waves_FFT_Simulation_CPU_Impl::computeFFT_Y_NReverseTask(uint taskIndex)
{
    const int64_t startNs = m_enableCpuTimers ? nowNanoseconds() : 0;
    const uint N = m_params.textureSize;

    for (uint i = 0; i < 8u; ++i)
    {
        const size_t offset = static_cast<size_t>(4u) * (8u * taskIndex + i);
        Simulation_Util::FFT1DSIMD_Y_4wide(m_height + offset, N, -1);
        Simulation_Util::FFT1DSIMD_Y_4wide(m_slope + offset, N, -1);
        Simulation_Util::FFT1DSIMD_Y_4wide(m_displacementX + offset, N, -1);
        Simulation_Util::FFT1DSIMD_Y_4wide(m_displacementY + offset, N, -1);
    }

    if (m_numFFT_Y_NReverseTasks.fetch_sub(1) == 1 && m_threadPool)
    {
        const uint taskCount = static_cast<uint>(getNumGenerateTextureTasks());
        std::vector<std::function<void()>> tasks;
        tasks.reserve(taskCount);
        for (uint taskIndex = 0; taskIndex < taskCount; ++taskIndex)
            tasks.emplace_back([this, taskIndex]() {
                generateDisplacementDataTask(taskIndex);
            });
        m_threadPool->enqueueLowPriorityVector(std::move(tasks));
    }

    if (m_enableCpuTimers)
        m_currentWorkerThreadNs.fetch_add(std::max<int64_t>(0, nowNanoseconds() - startNs));
}

void Local_Waves_FFT_Simulation_CPU_Impl::flip()
{
    uploadCompletedBuffer(m_stagedKickId);

    const uint32_t completedIndex = m_currentDisplacementBufferIndex;
    m_currentDisplacementBuffer =
        reinterpret_cast<uint8_t*>(m_displacementBuffers[completedIndex]);
    m_displacementMapVersion = m_stagedKickId;
    m_currentDisplacementBufferIndex = (completedIndex - 1u) & 1u;

    m_completedWorkerThreadNs =
        static_cast<uint64_t>(std::max<int64_t>(0, m_currentWorkerThreadNs.load()));
    m_cursorKickId = m_stagedKickId;
    m_stagingPending = false;
    m_cursorValid = true;
}

void Local_Waves_FFT_Simulation_CPU_Impl::generateDisplacementData(
    uint startRow,
    uint rowCount)
{
    // TODO: improve me using SIMD, see _Local_Waves_FFT_Simulation_CPU_Impl.cpp:1369-1419.
    const uint N = m_params.textureSize;
    const uint endRow = startRow + rowCount;
    const uint32_t bufferIndex = m_currentDisplacementBufferIndex;
    uint16_t* fp16 = m_outputFp16[bufferIndex];
    float4* displacement = m_displacementBuffers[bufferIndex];
    const float borderWidth = static_cast<float>(N) * 0.1f;

    for (uint row = startRow; row < endRow; ++row)
    {
        uint16_t* fp16Row =
            reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(fp16) +
                                        static_cast<size_t>(row) * m_outputRowPitch);

        for (uint col = 0; col < N; ++col)
        {
            const uint idx = row * N + col;
            const float edgeDistance = std::min(
                std::min(static_cast<float>(row), static_cast<float>(col)),
                std::min(static_cast<float>(N - row - 1), static_cast<float>(N - col - 1)));
            float fade = 1.0f;
            if (borderWidth >= edgeDistance)
            {
                const float t = (borderWidth - edgeDistance) / borderWidth;
                fade = std::exp(-5.0f * t * t * m_simulationTime);
            }

            const float height = m_height[idx].x * m_params.amplitudeMultiplier * fade;
            const float dispX =
                m_displacementX[idx].x * m_params.lateralMultiplier * fade;
            const float dispY =
                m_displacementY[idx].x * m_params.lateralMultiplier * fade;

            m_initialX[idx] = make_float2(m_height[idx].x * fade, m_height[idx].y * fade);
            m_initialY[idx] = m_slope[idx];

            if (m_params.enableCpuDrivenDisplacementCalculation)
            {
                displacement[idx].x = dispX;
                displacement[idx].y = dispY;
                displacement[idx].z = height;
            }

            fp16Row[4u * col + 0u] = floatToHalfBitsTruncate(dispX);
            fp16Row[4u * col + 1u] = floatToHalfBitsTruncate(dispY);
            fp16Row[4u * col + 2u] = floatToHalfBitsTruncate(height);
            fp16Row[4u * col + 3u] = floatToHalfBitsTruncate(0.0f);
        }
    }
}

void Local_Waves_FFT_Simulation_CPU_Impl::generateDisplacementDataTask(uint taskIndex)
{
    const int64_t startNs = m_enableCpuTimers ? nowNanoseconds() : 0;
    generateDisplacementData(8u * taskIndex, 8u);
    m_numGenerateTextureTasks.fetch_sub(1);

    if (m_enableCpuTimers)
        m_currentWorkerThreadNs.fetch_add(std::max<int64_t>(0, nowNanoseconds() - startNs));
}

void Local_Waves_FFT_Simulation_CPU_Impl::generateEvolvingOperators(
    uint startRow,
    uint rowCount)
{
    // TODO: improve me using SIMD, see _Local_Waves_FFT_Simulation_CPU_Impl.cpp:1570-1678.
    const uint N = m_params.textureSize;
    const uint halfN = N >> 1;
    const uint endRow = startRow + rowCount;
    const float domainScale = TwoPif / m_params.physicalSize;
    const float amplitudeScale =
        std::pow(0.95f, m_simulationTime) / (static_cast<float>(N) * static_cast<float>(N));

    auto storeOperator = [this](uint idx, float cosTerm, float sinOmegaTerm,
                               float sinInvOmegaTerm) {
        m_operatorCos[idx] = cosTerm;
        m_operatorSinOmega[idx] = sinOmegaTerm;
        m_operatorSinInvOmega[idx] = sinInvOmegaTerm;
    };

    for (uint row = startRow; row < endRow; ++row)
    {
        const float ky = static_cast<float>(row);
        const uint mirrorRow = N - row - 1u;

        for (uint col = 0; col < halfN; ++col)
        {
            const float kx = static_cast<float>(col);
            const uint idx = row * N + col;
            const float waveNumber = std::sqrt(kx * kx + ky * ky) * domainScale;

            // omega = sqrt(k^3 * 7.4e-5 + 9.81 * k) + 1e-6 (no k == 0 special case)
            const float omega =
                std::sqrt(waveNumber * waveNumber * waveNumber * 7.4e-5f +
                          9.81f * waveNumber) +
                1e-6f;
            const float phase = omega * m_simulationTime;
            const float sinPhase = std::sin(phase);
            const float cosPhase = std::cos(phase);
            // m_operatorCos = cos * scale; m_operatorSinOmega = (k / omega) * sin * scale;
            // m_operatorSinInvOmega = (9.81 / omega) * sin * scale
            const float cosTerm = cosPhase * amplitudeScale;
            const float sinOmegaTerm = ((waveNumber / omega) * sinPhase) * amplitudeScale;
            const float sinInvOmegaTerm = ((9.81f / omega) * sinPhase) * amplitudeScale;
            storeOperator(idx, cosTerm, sinOmegaTerm, sinInvOmegaTerm);

            const uint mirrorCol = N - col - 1u;
            if (mirrorCol != col)
                storeOperator(row * N + mirrorCol, cosTerm, sinOmegaTerm, sinInvOmegaTerm);
            if (mirrorRow != row)
                storeOperator(mirrorRow * N + col, cosTerm, sinOmegaTerm, sinInvOmegaTerm);
            if (mirrorRow != row && mirrorCol != col)
                storeOperator(mirrorRow * N + mirrorCol, cosTerm, sinOmegaTerm, sinInvOmegaTerm);
        }
    }
}

void Local_Waves_FFT_Simulation_CPU_Impl::generateEvolvingOperatorsTask(uint taskIndex)
{
    const int64_t startNs = m_enableCpuTimers ? nowNanoseconds() : 0;
    generateEvolvingOperators(8u * taskIndex, 8u);

    if (m_numGenerateEvolvingOperatorsTasks.fetch_sub(1) == 1 && m_threadPool)
    {
        const uint taskCount = static_cast<uint>(getNumApplyEvolvingOperatorsTasks());
        std::vector<std::function<void()>> tasks;
        tasks.reserve(taskCount);
        for (uint taskIndex = 0; taskIndex < taskCount; ++taskIndex)
            tasks.emplace_back([this, taskIndex]() {
                applyEvolvingOperatorsTask(taskIndex);
            });
        m_threadPool->enqueueLowPriorityVector(std::move(tasks));
    }

    if (m_enableCpuTimers)
        m_currentWorkerThreadNs.fetch_add(std::max<int64_t>(0, nowNanoseconds() - startNs));
}

void Local_Waves_FFT_Simulation_CPU_Impl::resetTaskCounters()
{
    m_numFFT_X_NForwardTasks.store(static_cast<int32_t>(getNumFFTTasks()));
    m_numFFT_Y_NForwardTasks.store(static_cast<int32_t>(getNumFFTTasks()));
    m_numGenerateEvolvingOperatorsTasks.store(
        static_cast<int32_t>(getNumGenerateEvolvingOperatorsTasks()));
    m_numApplyEvolvingOperatorsTasks.store(
        static_cast<int32_t>(getNumApplyEvolvingOperatorsTasks()));
    m_numFFT_X_NReverseTasks.store(static_cast<int32_t>(getNumFFTTasks()));
    m_numFFT_Y_NReverseTasks.store(static_cast<int32_t>(getNumFFTTasks()));
    m_numGenerateTextureTasks.store(static_cast<int32_t>(getNumGenerateTextureTasks()));
}

void Local_Waves_FFT_Simulation_CPU_Impl::uploadCompletedBuffer(uint64_t version)
{
    if (!m_useCpu || !m_device || !m_cmdList || !m_displacementTexture ||
        !m_staging[m_currentDisplacementBufferIndex])
    {
        m_displacementMapVersion = version;
        return;
    }

    const uint N = m_params.textureSize;
    nvrhi::TextureSlice textureSlice = {};
    textureSlice.width = N;
    textureSlice.height = N;

    size_t rowPitch = 0;
    uint8_t* stagingData = reinterpret_cast<uint8_t*>(
        m_device->mapStagingTexture(m_staging[m_currentDisplacementBufferIndex],
                                    textureSlice, nvrhi::CpuAccessMode::Write,
                                    rowPitch));
    if (stagingData)
    {
        const uint8_t* source =
            reinterpret_cast<const uint8_t*>(m_outputFp16[m_currentDisplacementBufferIndex]);
        for (uint row = 0; row < N; ++row)
        {
            std::memcpy(stagingData + rowPitch * row,
                        source + static_cast<size_t>(row) * m_outputRowPitch,
                        m_outputRowPitch);
        }
    }
    m_device->unmapStagingTexture(m_staging[m_currentDisplacementBufferIndex]);

    m_cmdList->beginTrackingTextureState(m_displacementTexture, nvrhi::AllSubresources,
                                         nvrhi::ResourceStates::ShaderResource);
    m_cmdList->copyTexture3(m_displacementTexture, textureSlice, m_staging[m_currentDisplacementBufferIndex], textureSlice);
    m_cmdList->setTextureState(m_displacementTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    m_cmdList->commitBarriers();
    m_displacementMapVersion = version;
}
