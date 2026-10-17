#include "Local_Waves_FFT_Simulation_Compute_Impl.h"

#include <cstring>
#include <limits>
#include <utility>

#include "CustomMemory.h"
#include "Simulation_Util.h"

#include "ShaderBytecode.h"

namespace
{
constexpr uint32_t kReadbackSlotCount = 4;
constexpr uint32_t kReadbackSlotMask = kReadbackSlotCount - 1;
constexpr uint64_t kInvalidKickId = std::numeric_limits<uint64_t>::max();

uint32_t nextRingIndex(uint32_t index)
{
    return (index + 1u) & kReadbackSlotMask;
}

bool isPowerOfTwo(uint32_t value)
{
    return value != 0 && (value & (value - 1u)) == 0;
}

bool checkedByteSize(size_t count, size_t elementSize, uint32_t& byteSize)
{
    if (elementSize != 0 && count > std::numeric_limits<uint32_t>::max() / elementSize)
        return false;

    byteSize = static_cast<uint32_t>(count * elementSize);
    return true;
}

nvrhi::BindingLayoutItem bindingLayoutItem(uint32_t slot, nvrhi::ResourceType type)
{
    nvrhi::BindingLayoutItem item{};
    item.size = 1;
    item.slot = slot;
    item.type = type;

    return item;
}

nvrhi::ShaderHandle createComputeShader(
    nvrhi::IDevice* device,
    const void* bytecode,
    size_t bytecodeSize)
{
    nvrhi::ShaderHandle result;
    device->createShader(nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Compute), bytecode, bytecodeSize, &result);
    return result;
}

nvrhi::BindingLayoutHandle createComputeBindingLayout(
    nvrhi::IDevice* device,
    std::initializer_list<nvrhi::BindingLayoutItem> items)
{
    nvrhi::BindingLayoutDesc desc;
    desc.visibility = nvrhi::ShaderType::Compute;
    desc.bindings = items;
    nvrhi::BindingLayoutHandle result;
    device->createBindingLayout(desc, &result);
    return result;
}

nvrhi::BindingSetHandle createComputeBindingSet(
    nvrhi::IDevice* device,
    nvrhi::IBindingLayout* layout,
    std::initializer_list<nvrhi::BindingSetItem> items)
{
    nvrhi::BindingSetDesc desc;
    desc.bindings = items;
    nvrhi::BindingSetHandle result;
    device->createBindingSet(desc, layout, &result);
    return result;
}

nvrhi::ComputePipelineHandle createComputePipeline(
    nvrhi::IDevice* device,
    nvrhi::IShader* shader,
    nvrhi::IBindingLayout* layout)
{
    nvrhi::ComputePipelineDesc desc;
    desc.CS = shader;
    desc.bindingLayouts = {layout};
    nvrhi::ComputePipelineHandle result;
    device->createComputePipeline(desc, &result);
    return result;
}
}

Local_Waves_FFT_Simulation_Compute_Impl::Local_Waves_FFT_Simulation_Compute_Impl(
    const Local_Waves_Detailed_Simulation_Parameters& params)
    : m_params(params)
    , m_padding0(0)
    , m_readbackReadIndex(0)
    , m_readbackWriteIndex(1)
    , m_readbacksReady(false)
    , m_padding1{}
    , m_readbackKickIds{kInvalidKickId, kInvalidKickId, kInvalidKickId, kInvalidKickId}
    , m_displacementMapVersion(kInvalidKickId)
    , m_timerResultsMs{}
    , m_timerReadIndex(0)
    , m_timerWriteIndex(1)
    , m_nextKickId(0)
    , m_stagingCursorValid(false)
    , m_padding2{}
    , m_stagingCursorKickId(0)
    , m_resetPending(false)
    , m_padding3{}
    , m_disturbanceScratch(nullptr)
    , m_device(nullptr)
    , m_cmdList(nullptr)
    , m_displacementTexture(nullptr)
    , m_disturbanceBuffer(nullptr)
    , m_readbackStaging{}
    , m_readbackEvents{}
    , m_currentReadbackStaging(nullptr)
    , m_timerQueries{}
    , m_addDisturbancesShader(nullptr)
    , m_addDisturbancesPipeline(nullptr)
    , m_addDisturbancesBindingSet(nullptr)
    , m_addDisturbancesBindingLayout(nullptr)
    , m_forwardRowsShader(nullptr)
    , m_forwardRowsPipeline(nullptr)
    , m_forwardRowsBindingSet(nullptr)
    , m_forwardRowsBindingLayout(nullptr)
    , m_forwardColumnsShader(nullptr)
    , m_forwardColumnsPipeline(nullptr)
    , m_forwardColumnsBindingSet(nullptr)
    , m_forwardColumnsBindingLayout(nullptr)
    , m_evolveShader(nullptr)
    , m_evolvePipeline(nullptr)
    , m_evolveBindingSet(nullptr)
    , m_evolveBindingLayout(nullptr)
    , m_reverseRowsShader(nullptr)
    , m_reverseRowsPipeline(nullptr)
    , m_reverseRowsBindingSet(nullptr)
    , m_reverseRowsBindingLayout(nullptr)
    , m_reverseColumnsShader(nullptr)
    , m_reverseColumnsPipeline(nullptr)
    , m_reverseColumnsBindingSet(nullptr)
    , m_reverseColumnsBindingLayout(nullptr)
    , m_verticalDisplacementAndPotentialBuffer(nullptr)
    , m_verticalFrequencyBuffer(nullptr)
    , m_lateralDisplacementBuffer(nullptr)
    , m_lateralFrequencyBuffer(nullptr)
    , m_constantBuffer(nullptr)
    , m_intermediateBuffer1(nullptr)
    , m_intermediateBuffer2(nullptr)
{
}

Local_Waves_FFT_Simulation_Compute_Impl::~Local_Waves_FFT_Simulation_Compute_Impl()
{
    releaseAllResources();
}

WWRESULT Local_Waves_FFT_Simulation_Compute_Impl::advanceReadbackCursor(bool block)
{
    if (!m_params.enableCpuDrivenDisplacementCalculation || !m_readbacksReady)
        return WWE_NONE;

    const uint32_t next = nextRingIndex(m_readbackReadIndex);
    if (next == m_readbackWriteIndex)
        return WWE_NONE;

    if (!block)
    {
        if (!m_device || !m_readbackEvents[next] ||
            !m_device->pollEventQuery(m_readbackEvents[next]))
        {
            return WWE_WOULD_BLOCK;
        }
    }

    return collectSingleReadbackResult(true);
}

WWRESULT Local_Waves_FFT_Simulation_Compute_Impl::advanceStagingCursor(bool)
{
    return WWE_NONE;
}

WWRESULT Local_Waves_FFT_Simulation_Compute_Impl::beforeReinit()
{
    return WWE_OK;
}

WWRESULT Local_Waves_FFT_Simulation_Compute_Impl::beforeReallocateSimulation()
{
    m_stagingCursorValid = false;
    return resetReadbacks();
}

WWRESULT Local_Waves_FFT_Simulation_Compute_Impl::canCollectSingleReadbackResultWithoutBlocking()
{
    if (!m_readbacksReady)
        return WWE_FAIL;

    const uint32_t next = nextRingIndex(m_readbackReadIndex);
    if (next == m_readbackWriteIndex)
        return WWE_NONE;

    if (!m_device || !m_readbackEvents[next] ||
        !m_device->pollEventQuery(m_readbackEvents[next]))
    {
        return WWE_NONE;
    }

    return WWE_OK;
}

WWRESULT Local_Waves_FFT_Simulation_Compute_Impl::collectSingleReadbackResult(bool block)
{
    if (!m_readbacksReady)
        return WWE_NONE;

    const uint32_t next = nextRingIndex(m_readbackReadIndex);
    if (next == m_readbackWriteIndex)
        return WWE_NONE;

    if (!m_device || !m_readbackEvents[next])
        return WWE_NONE;

    if (block)
    {
        m_device->waitEventQuery(m_readbackEvents[next]);
    }
    else if (!m_device->pollEventQuery(m_readbackEvents[next]))
    {
        return WWE_NONE;
    }

    m_readbackReadIndex = next;
    m_currentReadbackStaging = m_readbackStaging[next];
    return WWE_OK;
}

WWRESULT Local_Waves_FFT_Simulation_Compute_Impl::consumeAvailableReadbackSlot(
    uint32_t& slot,
    uint64_t kickId)
{
    if (!m_readbacksReady)
        return WWE_OK;

    if (m_readbackReadIndex == m_readbackWriteIndex)
    {
        const WWRESULT result = collectSingleReadbackResult(true);
        if (result < 0)
            return result;
    }

    slot = m_readbackWriteIndex;
    m_readbackWriteIndex = nextRingIndex(m_readbackWriteIndex);
    m_readbackKickIds[slot] = kickId;

    if (m_device && m_readbackEvents[slot])
        m_device->pollEventQuery(m_readbackEvents[slot]);

    return WWE_OK;
}

WWRESULT Local_Waves_FFT_Simulation_Compute_Impl::consumeAvailableTimerSlot(
    uint32_t& slot,
    uint64_t kickId)
{
    (void)kickId;

    if (m_timerReadIndex == m_timerWriteIndex)
    {
        const uint32_t next = nextRingIndex(m_timerReadIndex);
        if (m_device && m_timerQueries[next])
            m_timerResultsMs[next] = m_device->getTimerQueryTime(m_timerQueries[next]) * 1000.0f;
        m_timerReadIndex = next;
    }

    slot = m_timerWriteIndex;
    m_timerWriteIndex = nextRingIndex(m_timerWriteIndex);
    return WWE_OK;
}

nvrhi::TextureHandle Local_Waves_FFT_Simulation_Compute_Impl::getDisplacementMap() const
{
    return m_displacementTexture;
}

uint64_t Local_Waves_FFT_Simulation_Compute_Impl::getDisplacementMapVersion() const
{
    return m_displacementMapVersion;
}

bool Local_Waves_FFT_Simulation_Compute_Impl::getReadbackCursor(uint64_t* cursor) const
{
    if (!m_params.enableCpuDrivenDisplacementCalculation || !m_readbacksReady)
        return false;

    const uint64_t kickId = m_readbackKickIds[m_readbackReadIndex];
    if (kickId == kInvalidKickId)
        return false;

    if (cursor)
        *cursor = kickId;
    return true;
}

bool Local_Waves_FFT_Simulation_Compute_Impl::getStagingCursor(uint64_t* cursor) const
{
    if (cursor && m_stagingCursorValid)
        *cursor = m_stagingCursorKickId;
    return m_stagingCursorValid;
}

WWRESULT Local_Waves_FFT_Simulation_Compute_Impl::getTimings(
    GFSDK_WaveWorks_Local_Waves_Simulation_Stats& stats) const
{
    stats.CPU_main_thread_wait_time = 0.0f;
    stats.CPU_threads_total_time = 0.0f;
    stats.GPU_simulation_time = m_timerResultsMs[m_timerReadIndex];
    return WWE_OK;
}

bool Local_Waves_FFT_Simulation_Compute_Impl::hasReadbacksInFlight() const
{
    return m_params.enableCpuDrivenDisplacementCalculation && m_readbacksReady &&
           nextRingIndex(m_readbackReadIndex) != m_readbackWriteIndex;
}

WWRESULT Local_Waves_FFT_Simulation_Compute_Impl::init(
    nvrhi::IDevice* device,
    nvrhi::CommandListHandle cmdList,
    bool useCpu)
{
    if (m_device)
        releaseAllResources();

    if (!useCpu)
    {
        m_device = nullptr;
        m_cmdList = nullptr;
        return WWE_FAIL;
    }

    m_device = device;
    m_cmdList = std::move(cmdList);
    if (!m_device || !m_cmdList)
        return WWE_FAIL;

    const WWRESULT result = allocateAllResources();
    return result < 0 ? result : WWE_OK;
}

WWRESULT Local_Waves_FFT_Simulation_Compute_Impl::kick(
    float simulationTime,
    float2* disturbanceData,
    uint64_t& kickId)
{
    kickId = m_nextKickId;
    collectSingleReadbackResult(false);

    uint32_t readbackSlot = 0;
    const bool copyReadback =
        m_readbacksReady && m_params.enableCpuDrivenDisplacementCalculation;

    nvrhi::IBuffer* computeBuffers[] = {
        m_verticalDisplacementAndPotentialBuffer.Get(),
        m_lateralDisplacementBuffer.Get(),
        m_verticalFrequencyBuffer.Get(),
        m_lateralFrequencyBuffer.Get(),
        m_intermediateBuffer1.Get(),
        m_intermediateBuffer2.Get(),
    };

    for (nvrhi::IBuffer* buffer : computeBuffers)
    {
        if (buffer)
            m_cmdList->beginTrackingBufferState(buffer, nvrhi::ResourceStates::UnorderedAccess);
    }

    m_cmdList->beginTrackingTextureState(
        m_displacementTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);

    uint32_t timerSlot = 0;
    consumeAvailableTimerSlot(timerSlot, kickId);

    if (copyReadback)
    {
        const WWRESULT result = consumeAvailableReadbackSlot(readbackSlot, kickId);
        if (result < 0)
            return result;
    }

    if (m_timerQueries[timerSlot])
    {
        m_device->resetTimerQuery(m_timerQueries[timerSlot]);
        m_cmdList->beginTimerQuery(m_timerQueries[timerSlot]);
    }

    m_cmdList->beginMarker("WaveWorks: simulating local waves");

    if (m_resetPending)
    {
        clearComputeBuffers();
        m_resetPending = false;
    }

    const uint32_t disturbanceCount = gatherDisturbances(disturbanceData);
    if (disturbanceCount != 0)
    {
        void* uploadData = m_device->mapBuffer(m_disturbanceBuffer, nvrhi::CpuAccessMode::Write);
        std::memcpy(uploadData, m_disturbanceScratch,
                    sizeof(DisturbanceUpload) * disturbanceCount);
        m_device->unmapBuffer(m_disturbanceBuffer);
    }

    uint32_t fftLog2 = 0;
    uint32_t fftPower = 0;
    Simulation_Util::powerOf2(m_params.textureSize, &fftLog2, &fftPower);

    struct Constants
    {
        uint32_t textureSize; // offset 0
        uint32_t fftStride; // offset 4
        float physicalSize; // offset 8
        float simulationTime; // offset 12
        float attenuation; // offset 16
        uint32_t disturbanceCount; // offset 20
        float amplitudeMultiplier; // offset 24
        float lateralMultiplier; // offset 28
    } constants = {
        m_params.textureSize,
        32u - fftLog2,
        m_params.physicalSize,
        simulationTime,
        0.95f,
        disturbanceCount,
        m_params.amplitudeMultiplier,
        m_params.lateralMultiplier,
    };

    m_cmdList->writeBuffer(m_constantBuffer, &constants, sizeof(constants));

    nvrhi::ComputeState computeState;
    auto dispatchPipeline =
        [&](nvrhi::IComputePipeline* pipeline,
            nvrhi::IBindingSet* bindingSet,
            uint32_t groupsX,
            uint32_t groupsY,
            uint32_t groupsZ)
    {
        computeState.pipeline = pipeline;
        computeState.bindings.resize(0);
        computeState.bindings.push_back(bindingSet);
        m_cmdList->setComputeState(computeState);
        m_cmdList->dispatch(groupsX, groupsY, groupsZ);
    };

    const uint32_t N = m_params.textureSize;
    if (disturbanceCount != 0)
    {
        dispatchPipeline(m_addDisturbancesPipeline, m_addDisturbancesBindingSet,
                         (disturbanceCount >> 7u) + 1u, 1u, 1u);
    }
    dispatchPipeline(m_forwardRowsPipeline, m_forwardRowsBindingSet, 1u, N, 1u);
    dispatchPipeline(m_forwardColumnsPipeline, m_forwardColumnsBindingSet, 1u, N, 1u);
    dispatchPipeline(m_evolvePipeline, m_evolveBindingSet, N >> 4u, N >> 4u, 1u);
    dispatchPipeline(m_reverseRowsPipeline, m_reverseRowsBindingSet, 1u, N, 1u);
    dispatchPipeline(m_reverseColumnsPipeline, m_reverseColumnsBindingSet, 1u, N, 1u);

    m_cmdList->endMarker();

    if (copyReadback)
    {
        const nvrhi::TextureSlice slice = fullResolutionSlice();
        m_cmdList->copyTexture2(m_readbackStaging[readbackSlot], slice, m_displacementTexture, slice);
        m_device->resetEventQuery(m_readbackEvents[readbackSlot]);
        m_device->pollEventQuery(m_readbackEvents[readbackSlot]);
        m_device->setEventQuery(m_readbackEvents[readbackSlot], nvrhi::CommandQueue::Graphics);
    }

    if (m_timerQueries[timerSlot])
        m_cmdList->endTimerQuery(m_timerQueries[timerSlot]);

    for (nvrhi::IBuffer* buffer : computeBuffers)
    {
        if (buffer)
            {
                m_cmdList->setBufferState(buffer, nvrhi::ResourceStates::UnorderedAccess);
                m_cmdList->commitBarriers();
            }
    }

    m_cmdList->setTextureState(m_displacementTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    m_cmdList->commitBarriers();

    m_displacementMapVersion = kickId;
    m_stagingCursorValid = true;
    m_stagingCursorKickId = m_nextKickId;
    ++m_nextKickId;
    return WWE_OK;
}

WWRESULT Local_Waves_FFT_Simulation_Compute_Impl::mapDisplacementData(
    uint8_t** data,
    uint32_t* rowPitch,
    bool* fp16)
{
    if (!getReadbackCursor(nullptr) || !m_currentReadbackStaging)
        return WWE_NONE;

    size_t mappedRowPitch = 0;
    *data = reinterpret_cast<uint8_t*>(
        m_device->mapStagingTexture(m_currentReadbackStaging,
                                    fullResolutionSlice(),
                                    nvrhi::CpuAccessMode::Read,
                                    mappedRowPitch));
    *rowPitch = static_cast<uint32_t>(mappedRowPitch);
    *fp16 = true;
    return WWE_OK;
}

WWRESULT Local_Waves_FFT_Simulation_Compute_Impl::reinit(
    const Local_Waves_Detailed_Simulation_Parameters& params)
{
    const bool needsReallocation =
        params.textureSize != m_params.textureSize ||
        params.enableCpuDrivenDisplacementCalculation !=
            m_params.enableCpuDrivenDisplacementCalculation;

    if (!needsReallocation)
    {
        m_params = params;
        return WWE_OK;
    }

    WWRESULT resetResult = beforeReallocateSimulation();
    if (resetResult < 0)
        return resetResult;

    releaseAllResources();
    m_params = params;

    const WWRESULT result = allocateAllResources();
    return result < 0 ? result : WWE_OK;
}

void Local_Waves_FFT_Simulation_Compute_Impl::releaseAllResources()
{
    waitForAllInFlightReadbacks();
    waitForAllInFlightTimers();

    m_displacementTexture = nullptr;
    m_disturbanceBuffer = nullptr;
    for (uint32_t i = 0; i < kReadbackSlotCount; ++i)
    {
        m_readbackStaging[i] = nullptr;
        m_readbackEvents[i] = nullptr;
        m_timerQueries[i] = nullptr;
        m_timerResultsMs[i] = 0.0f;
        m_readbackKickIds[i] = kInvalidKickId;
    }
    m_currentReadbackStaging = nullptr;
    releasePipelineResources();

    if (m_disturbanceScratch)
    {
        NVSDK_aligned_free(m_disturbanceScratch);
        m_disturbanceScratch = nullptr;
    }

    m_readbacksReady = false;
}

void Local_Waves_FFT_Simulation_Compute_Impl::reset()
{
    m_resetPending = true;
}

WWRESULT Local_Waves_FFT_Simulation_Compute_Impl::resetReadbacks()
{
    if (!m_readbacksReady)
        return WWE_OK;

    const WWRESULT result = waitForAllInFlightReadbacks();
    if (result < 0)
        return result;

    m_readbackReadIndex = 0;
    m_readbackWriteIndex = 1;
    m_readbackKickIds[0] = kInvalidKickId;
    m_currentReadbackStaging = nullptr;
    return WWE_OK;
}

void Local_Waves_FFT_Simulation_Compute_Impl::unmapDisplacementData()
{
    if (m_device && m_currentReadbackStaging)
        m_device->unmapStagingTexture(m_currentReadbackStaging);
}

WWRESULT Local_Waves_FFT_Simulation_Compute_Impl::waitForAllInFlightReadbacks()
{
    while (hasReadbacksInFlight())
    {
        const WWRESULT result = collectSingleReadbackResult(true);
        if (result < 0)
            return result;
        if (result == WWE_NONE)
            break;
    }

    return WWE_OK;
}

WWRESULT Local_Waves_FFT_Simulation_Compute_Impl::waitForAllInFlightTimers()
{
    if (!m_device)
        return WWE_OK;

    for (uint32_t next = nextRingIndex(m_timerReadIndex);
         next != m_timerWriteIndex;
         next = nextRingIndex(next))
    {
        if (m_timerQueries[next])
            m_device->getTimerQueryTime(m_timerQueries[next]);
    }

    return WWE_OK;
}

WWRESULT Local_Waves_FFT_Simulation_Compute_Impl::waitStagingCursor()
{
    return WWE_NONE;
}

WWRESULT Local_Waves_FFT_Simulation_Compute_Impl::allocateAllResources()
{
    if (!m_device || !isPowerOfTwo(m_params.textureSize))
        return WWE_FAIL;

    const uint32_t N = m_params.textureSize;
    const size_t sampleCount = static_cast<size_t>(N) * static_cast<size_t>(N);
    uint32_t byteSize = 0;
    if (!checkedByteSize(sampleCount, sizeof(DisturbanceUpload), byteSize))
        return WWE_FAIL;

    // The recovered allocation unit replaces this pointer without releasing its old block.
    m_disturbanceScratch = reinterpret_cast<DisturbanceUpload*>(
        NVSDK_aligned_malloc(sizeof(DisturbanceUpload) * sampleCount, 16));
    if (!m_disturbanceScratch)
        return WWE_FAIL;
    std::memset(m_disturbanceScratch, 0, sizeof(DisturbanceUpload) * sampleCount);

    uint32_t mipExponent = 0;
    uint32_t mipPower = 0;
    Simulation_Util::powerOf2(N, &mipExponent, &mipPower);

    nvrhi::TextureDesc textureDesc;
    textureDesc.width = N;
    textureDesc.height = N;
    textureDesc.depth = 1;
    textureDesc.arraySize = 1;
    textureDesc.mipLevels = mipExponent + 1;
    textureDesc.sampleCount = 1;
    textureDesc.sampleQuality = 0;
    textureDesc.format = nvrhi::Format::RGBA16_FLOAT;
    textureDesc.dimension = nvrhi::TextureDimension::Texture2D;
    textureDesc.debugName = "Interactive simulation displacements texture";
    textureDesc.isUAV = true;
    textureDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    textureDesc.keepInitialState = true;

    m_device->createTexture(textureDesc, &m_displacementTexture);
    if (m_displacementTexture && m_device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN && textureDesc.keepInitialState) {
        // Establish the native image layout before exposing it to the renderer.
        nvrhi::CommandListHandle initializeCommands;
        m_device->createCommandList(nvrhi::CommandListParameters(), &initializeCommands);
        if (!initializeCommands) return WWE_FAIL;
        initializeCommands->open();
        initializeCommands->beginTrackingTextureState(m_displacementTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);
        initializeCommands->setTextureState(m_displacementTexture, nvrhi::AllSubresources, textureDesc.initialState);
        initializeCommands->commitBarriers();
        initializeCommands->close();
        m_device->executeCommandList(initializeCommands);
    }
    if (!m_displacementTexture)
        return WWE_FAIL;

    if (m_params.enableCpuDrivenDisplacementCalculation)
    {
        textureDesc.mipLevels = 1;
        textureDesc.isUAV = false;
        textureDesc.debugName = "Local waves Displacement Staging Texture";
        for (uint32_t i = 0; i < kReadbackSlotCount; ++i)
        {
            m_device->createStagingTexture(textureDesc, nvrhi::CpuAccessMode::Read, &m_readbackStaging[i]);
            m_device->createEventQuery(&m_readbackEvents[i]);
            if (!m_readbackStaging[i] || !m_readbackEvents[i])
                return WWE_FAIL;
            m_readbackKickIds[i] = kInvalidKickId;
            m_device->resetEventQuery(m_readbackEvents[i]);
        }

        m_readbackReadIndex = 0;
        m_readbackWriteIndex = 1;
        m_currentReadbackStaging = nullptr;
        m_readbacksReady = true;
    }

    nvrhi::BufferDesc disturbanceDesc;
    disturbanceDesc.byteSize = byteSize;
    disturbanceDesc.structStride = sizeof(DisturbanceUpload);
    disturbanceDesc.debugName = "Disturbances Buffer";
    disturbanceDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    disturbanceDesc.keepInitialState = true;
    disturbanceDesc.cpuAccess = nvrhi::CpuAccessMode::Write;
    m_device->createBuffer(disturbanceDesc, &m_disturbanceBuffer);
    if (!m_disturbanceBuffer)
        return WWE_FAIL;

    nvrhi::BufferDesc dataDesc;
    dataDesc.byteSize = byteSize;
    dataDesc.structStride = sizeof(DisturbanceUpload);
    dataDesc.canHaveUAVs = true;
    dataDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    dataDesc.keepInitialState = true;

    dataDesc.debugName = "Vertical displacements and velocity potential";
    m_device->createBuffer(dataDesc, &m_verticalDisplacementAndPotentialBuffer);
    dataDesc.debugName = "Vertical displacements and velocity potential in frequency space";
    m_device->createBuffer(dataDesc, &m_verticalFrequencyBuffer);
    dataDesc.debugName = "Lateral displacements ";
    m_device->createBuffer(dataDesc, &m_lateralDisplacementBuffer);
    dataDesc.debugName = "Lateral displacements in frequency space";
    m_device->createBuffer(dataDesc, &m_lateralFrequencyBuffer);
    dataDesc.debugName = "Intermediate buffer 1 for 2D FFT";
    m_device->createBuffer(dataDesc, &m_intermediateBuffer1);
    dataDesc.debugName = "Intermediate buffer 2 for 2D FFT";
    m_device->createBuffer(dataDesc, &m_intermediateBuffer2);

    nvrhi::BufferDesc constantDesc;
    constantDesc.byteSize = 32;
    constantDesc.debugName = "Interactive simulation constant buffer";
    constantDesc.isConstantBuffer = true;
    constantDesc.isVolatile = true;
    constantDesc.maxVersions = 4096;
    constantDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
    constantDesc.keepInitialState = true;
    m_device->createBuffer(constantDesc, &m_constantBuffer);

    if (!m_verticalDisplacementAndPotentialBuffer || !m_verticalFrequencyBuffer ||
        !m_lateralDisplacementBuffer || !m_lateralFrequencyBuffer ||
        !m_intermediateBuffer1 || !m_intermediateBuffer2 || !m_constantBuffer)
    {
        return WWE_FAIL;
    }

    for (uint32_t i = 0; i < kReadbackSlotCount; ++i)
    {
        m_device->createTimerQuery(&m_timerQueries[i]);
        m_timerResultsMs[i] = 0.0f;
    }
    m_timerReadIndex = 0;
    m_timerWriteIndex = 1;

    if (m_device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN)
    {
        m_addDisturbancesShader = createComputeShader(
            m_device,
            AddDisturbancesForLocalWavesCS_bytecode_vk,
            AddDisturbancesForLocalWavesCS_bytecode_vk_size);
        m_forwardRowsShader = createComputeShader(
            m_device,
            ForwardFFTStep1RowsForLocalWavesCS_bytecode_vk,
            ForwardFFTStep1RowsForLocalWavesCS_bytecode_vk_size);
        m_forwardColumnsShader = createComputeShader(
            m_device,
            ForwardFFTStep2ColumnsForLocalWavesCS_bytecode_vk,
            ForwardFFTStep2ColumnsForLocalWavesCS_bytecode_vk_size);
        m_evolveShader = createComputeShader(
            m_device,
            EvolveInFrequencySpaceForLocalWavesCS_bytecode_vk,
            EvolveInFrequencySpaceForLocalWavesCS_bytecode_vk_size);
        m_reverseRowsShader = createComputeShader(
            m_device,
            ReverseFFTStep1RowsForLocalWavesCS_bytecode_vk,
            ReverseFFTStep1RowsForLocalWavesCS_bytecode_vk_size);
        m_reverseColumnsShader = createComputeShader(
            m_device,
            ReverseFFTStep2ColumnsAndWriteDisplacementsForLocalWavesCS_bytecode_vk,
            ReverseFFTStep2ColumnsAndWriteDisplacementsForLocalWavesCS_bytecode_vk_size);
    }
    else
    {
        m_addDisturbancesShader = createComputeShader(
            m_device,
            AddDisturbancesForLocalWavesCS_bytecode_dx,
            AddDisturbancesForLocalWavesCS_bytecode_dx_size);
        m_forwardRowsShader = createComputeShader(
            m_device,
            ForwardFFTStep1RowsForLocalWavesCS_bytecode_dx,
            ForwardFFTStep1RowsForLocalWavesCS_bytecode_dx_size);
        m_forwardColumnsShader = createComputeShader(
            m_device,
            ForwardFFTStep2ColumnsForLocalWavesCS_bytecode_dx,
            ForwardFFTStep2ColumnsForLocalWavesCS_bytecode_dx_size);
        m_evolveShader = createComputeShader(
            m_device,
            EvolveInFrequencySpaceForLocalWavesCS_bytecode_dx,
            EvolveInFrequencySpaceForLocalWavesCS_bytecode_dx_size);
        m_reverseRowsShader = createComputeShader(
            m_device,
            ReverseFFTStep1RowsForLocalWavesCS_bytecode_dx,
            ReverseFFTStep1RowsForLocalWavesCS_bytecode_dx_size);
        m_reverseColumnsShader = createComputeShader(
            m_device,
            ReverseFFTStep2ColumnsAndWriteDisplacementsForLocalWavesCS_bytecode_dx,
            ReverseFFTStep2ColumnsAndWriteDisplacementsForLocalWavesCS_bytecode_dx_size);
    }

    if (!m_addDisturbancesShader || !m_forwardRowsShader || !m_forwardColumnsShader ||
        !m_evolveShader || !m_reverseRowsShader || !m_reverseColumnsShader)
    {
        return WWE_FAIL;
    }

    const nvrhi::ResourceType structuredSRV = nvrhi::ResourceType::StructuredBuffer_SRV;
    const nvrhi::ResourceType structuredUAV = nvrhi::ResourceType::StructuredBuffer_UAV;
    const nvrhi::ResourceType textureUAV = nvrhi::ResourceType::Texture_UAV;
    const nvrhi::ResourceType constantBuffer =
        m_constantBuffer->getDesc().isVolatile
            ? nvrhi::ResourceType::VolatileConstantBuffer
            : nvrhi::ResourceType::ConstantBuffer;

    m_addDisturbancesBindingLayout = createComputeBindingLayout(
        m_device,
        {bindingLayoutItem(0, structuredSRV),
         bindingLayoutItem(0, structuredUAV),
         bindingLayoutItem(0, constantBuffer)});
    m_addDisturbancesBindingSet = createComputeBindingSet(
        m_device,
        m_addDisturbancesBindingLayout,
        {nvrhi::BindingSetItem::StructuredBuffer_SRV(0, m_disturbanceBuffer),
         nvrhi::BindingSetItem::StructuredBuffer_UAV(
             0, m_verticalDisplacementAndPotentialBuffer),
         nvrhi::BindingSetItem::ConstantBuffer(0, m_constantBuffer)});
    m_addDisturbancesPipeline = createComputePipeline(
        m_device, m_addDisturbancesShader, m_addDisturbancesBindingLayout);

    m_forwardRowsBindingLayout = createComputeBindingLayout(
        m_device,
        {bindingLayoutItem(0, structuredSRV),
         bindingLayoutItem(0, structuredUAV),
         bindingLayoutItem(0, constantBuffer)});
    m_forwardRowsBindingSet = createComputeBindingSet(
        m_device,
        m_forwardRowsBindingLayout,
        {nvrhi::BindingSetItem::StructuredBuffer_SRV(
             0, m_verticalDisplacementAndPotentialBuffer),
         nvrhi::BindingSetItem::StructuredBuffer_UAV(0, m_intermediateBuffer1),
         nvrhi::BindingSetItem::ConstantBuffer(0, m_constantBuffer)});
    m_forwardRowsPipeline = createComputePipeline(
        m_device, m_forwardRowsShader, m_forwardRowsBindingLayout);

    m_forwardColumnsBindingLayout = createComputeBindingLayout(
        m_device,
        {bindingLayoutItem(0, structuredSRV),
         bindingLayoutItem(0, structuredUAV),
         bindingLayoutItem(0, constantBuffer)});
    m_forwardColumnsBindingSet = createComputeBindingSet(
        m_device,
        m_forwardColumnsBindingLayout,
        {nvrhi::BindingSetItem::StructuredBuffer_SRV(0, m_intermediateBuffer1),
         nvrhi::BindingSetItem::StructuredBuffer_UAV(0, m_verticalFrequencyBuffer),
         nvrhi::BindingSetItem::ConstantBuffer(0, m_constantBuffer)});
    m_forwardColumnsPipeline = createComputePipeline(
        m_device, m_forwardColumnsShader, m_forwardColumnsBindingLayout);

    m_evolveBindingLayout = createComputeBindingLayout(
        m_device,
        {bindingLayoutItem(0, structuredUAV),
         bindingLayoutItem(1, structuredUAV),
         bindingLayoutItem(0, constantBuffer)});
    m_evolveBindingSet = createComputeBindingSet(
        m_device,
        m_evolveBindingLayout,
        {nvrhi::BindingSetItem::StructuredBuffer_UAV(0, m_verticalFrequencyBuffer),
         nvrhi::BindingSetItem::StructuredBuffer_UAV(1, m_lateralFrequencyBuffer),
         nvrhi::BindingSetItem::ConstantBuffer(0, m_constantBuffer)});
    m_evolvePipeline =
        createComputePipeline(m_device, m_evolveShader, m_evolveBindingLayout);

    m_reverseRowsBindingLayout = createComputeBindingLayout(
        m_device,
        {bindingLayoutItem(0, structuredSRV),
         bindingLayoutItem(1, structuredSRV),
         bindingLayoutItem(0, structuredUAV),
         bindingLayoutItem(1, structuredUAV),
         bindingLayoutItem(0, constantBuffer)});
    m_reverseRowsBindingSet = createComputeBindingSet(
        m_device,
        m_reverseRowsBindingLayout,
        {nvrhi::BindingSetItem::StructuredBuffer_SRV(0, m_verticalFrequencyBuffer),
         nvrhi::BindingSetItem::StructuredBuffer_SRV(1, m_lateralFrequencyBuffer),
         nvrhi::BindingSetItem::StructuredBuffer_UAV(0, m_intermediateBuffer1),
         nvrhi::BindingSetItem::StructuredBuffer_UAV(1, m_intermediateBuffer2),
         nvrhi::BindingSetItem::ConstantBuffer(0, m_constantBuffer)});
    m_reverseRowsPipeline = createComputePipeline(
        m_device, m_reverseRowsShader, m_reverseRowsBindingLayout);

    m_reverseColumnsBindingLayout = createComputeBindingLayout(
        m_device,
        {bindingLayoutItem(0, structuredSRV),
         bindingLayoutItem(1, structuredSRV),
         bindingLayoutItem(0, structuredUAV),
         bindingLayoutItem(1, structuredUAV),
         bindingLayoutItem(2, textureUAV),
         bindingLayoutItem(0, constantBuffer)});
    m_reverseColumnsBindingSet = createComputeBindingSet(
        m_device,
        m_reverseColumnsBindingLayout,
        {nvrhi::BindingSetItem::StructuredBuffer_SRV(0, m_intermediateBuffer1),
         nvrhi::BindingSetItem::StructuredBuffer_SRV(1, m_intermediateBuffer2),
         nvrhi::BindingSetItem::StructuredBuffer_UAV(
             0, m_verticalDisplacementAndPotentialBuffer),
         nvrhi::BindingSetItem::StructuredBuffer_UAV(
             1, m_lateralDisplacementBuffer),
         nvrhi::BindingSetItem::Texture_UAV(
             2, m_displacementTexture, nvrhi::Format::RGBA16_FLOAT),
         nvrhi::BindingSetItem::ConstantBuffer(0, m_constantBuffer)});
    m_reverseColumnsPipeline = createComputePipeline(
        m_device, m_reverseColumnsShader, m_reverseColumnsBindingLayout);

    if (!m_addDisturbancesBindingLayout || !m_addDisturbancesBindingSet ||
        !m_addDisturbancesPipeline || !m_forwardRowsBindingLayout ||
        !m_forwardRowsBindingSet || !m_forwardRowsPipeline ||
        !m_forwardColumnsBindingLayout || !m_forwardColumnsBindingSet ||
        !m_forwardColumnsPipeline || !m_evolveBindingLayout || !m_evolveBindingSet ||
        !m_evolvePipeline || !m_reverseRowsBindingLayout ||
        !m_reverseRowsBindingSet || !m_reverseRowsPipeline ||
        !m_reverseColumnsBindingLayout || !m_reverseColumnsBindingSet ||
        !m_reverseColumnsPipeline)
    {
        return WWE_FAIL;
    }

    m_displacementMapVersion = kInvalidKickId;
    return WWE_OK;
}

void Local_Waves_FFT_Simulation_Compute_Impl::clearComputeBuffers()
{
    if (!m_cmdList)
        return;

    if (m_verticalDisplacementAndPotentialBuffer)
        m_cmdList->clearBufferUInt(m_verticalDisplacementAndPotentialBuffer, 0);
}

uint32_t Local_Waves_FFT_Simulation_Compute_Impl::gatherDisturbances(
    float2* disturbanceData)
{
    const uint32_t N = m_params.textureSize;
    uint32_t count = 0;
    for (uint32_t row = 0; row < N; ++row)
    {
        for (uint32_t column = 0; column < N; ++column)
        {
            const size_t index = static_cast<size_t>(row) * N + column;
            const float2 disturbance = disturbanceData[index];
            if (disturbance.x != 0.0f || disturbance.y != 0.0f)
            {
                m_disturbanceScratch[count].row = static_cast<float>(row);
                m_disturbanceScratch[count].column = static_cast<float>(column);
                m_disturbanceScratch[count].displacement = disturbance.x;
                m_disturbanceScratch[count].velocityPotential = disturbance.y;
                ++count;
            }
            disturbanceData[index] = make_float2(0.0f);
        }
    }

    return count;
}

nvrhi::TextureSlice Local_Waves_FFT_Simulation_Compute_Impl::fullResolutionSlice() const
{
    nvrhi::TextureSlice slice;
    slice.width = m_params.textureSize;
    slice.height = m_params.textureSize;
    return slice;
}

void Local_Waves_FFT_Simulation_Compute_Impl::releasePipelineResources()
{
    m_addDisturbancesShader = nullptr;
    m_addDisturbancesPipeline = nullptr;
    m_addDisturbancesBindingSet = nullptr;
    m_addDisturbancesBindingLayout = nullptr;

    m_forwardRowsShader = nullptr;
    m_forwardRowsPipeline = nullptr;
    m_forwardRowsBindingSet = nullptr;
    m_forwardRowsBindingLayout = nullptr;

    m_forwardColumnsShader = nullptr;
    m_forwardColumnsPipeline = nullptr;
    m_forwardColumnsBindingSet = nullptr;
    m_forwardColumnsBindingLayout = nullptr;

    m_evolveShader = nullptr;
    m_evolvePipeline = nullptr;
    m_evolveBindingSet = nullptr;
    m_evolveBindingLayout = nullptr;

    m_reverseRowsShader = nullptr;
    m_reverseRowsPipeline = nullptr;
    m_reverseRowsBindingSet = nullptr;
    m_reverseRowsBindingLayout = nullptr;

    m_reverseColumnsShader = nullptr;
    m_reverseColumnsPipeline = nullptr;
    m_reverseColumnsBindingSet = nullptr;
    m_reverseColumnsBindingLayout = nullptr;

    m_verticalDisplacementAndPotentialBuffer = nullptr;
    m_verticalFrequencyBuffer = nullptr;
    m_lateralDisplacementBuffer = nullptr;
    m_lateralFrequencyBuffer = nullptr;
    m_constantBuffer = nullptr;
    m_intermediateBuffer1 = nullptr;
    m_intermediateBuffer2 = nullptr;
}
