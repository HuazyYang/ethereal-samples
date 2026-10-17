#include "Wind_Waves_FFT_Simulation_Compute_Impl.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <utility>
#include <vector>

#include "Simulation_Util.h"
#include "Wind_Waves_Simulation_Manager_Compute_Impl.h"

#include "ShaderBytecode.h"

namespace
{
constexpr uint32_t kReadbackSlotCount = 4;
constexpr uint32_t kReadbackSlotMask = kReadbackSlotCount - 1;
constexpr uint32_t kGaussSourceSize = 512;
constexpr uint32_t kGaussSourceStride = 516;
constexpr uint64_t kInvalidFrameIndex = std::numeric_limits<uint64_t>::max();

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

uint32_t asUint(float value)
{
    uint32_t result = 0;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

void writeFloat(std::array<uint32_t, 32>& constants, size_t index, float value)
{
    constants[index] = asUint(value);
}

void writeDouble(std::array<uint32_t, 32>& constants, size_t index, double value)
{
    std::memcpy(&constants[index], &value, sizeof(value));
}

float2 normalizedOrXAxis(const float2& value)
{
    const float length = std::sqrt(value.x * value.x + value.y * value.y);
    if (length <= 0.0f)
        return make_float2(1.0f, 0.0f);
    return make_float2(value.x / length, value.y / length);
}

// pseudo: reinit 2618-2638. Matches the recovered binary: the two wind-direction compares are
// intentionally cross-component (new .x against current .y) and the new .y is never compared.
bool h0InputsChanged(
    const Wind_Waves_Detailed_Simulation_Parameters::Cascade& next,
    const Wind_Waves_Detailed_Simulation_Parameters::Cascade& current)
{
    return next.baseWindSpeed != current.baseWindSpeed ||
           next.baseWindDistance != current.baseWindDistance ||
           next.baseSpectrumPeaking != current.baseSpectrumPeaking ||
           next.baseWindDirection.x != current.baseWindDirection.y ||
           next.baseWindDependency != current.baseWindDependency ||
           next.baseSmallWavesCutoffLength != current.baseSmallWavesCutoffLength ||
           next.baseSmallWavesCutoffPower != current.baseSmallWavesCutoffPower ||
           next.baseAmplitudeMultiplier != current.baseAmplitudeMultiplier ||
           next.swellWindSpeed != current.swellWindSpeed ||
           next.swellWindDistance != current.swellWindDistance ||
           next.swellSpectrumPeaking != current.swellSpectrumPeaking ||
           next.swellWindDirection.x != current.swellWindDirection.y ||
           next.swellWindDependency != current.swellWindDependency ||
           next.swellSmallWavesCutoffLength != current.swellSmallWavesCutoffLength ||
           next.swellSmallWavesCutoffPower != current.swellSmallWavesCutoffPower ||
           next.swellAmplitudeMultiplier != current.swellAmplitudeMultiplier ||
           next.lowerGridRadius != current.lowerGridRadius ||
           next.upperGridRadius != current.upperGridRadius;
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

Wind_Waves_FFT_Simulation_Compute_Impl::ReadbackArchiveFIFOSlot::ReadbackArchiveFIFOSlot()
    : frameIndex(kInvalidFrameIndex)
    , staging(nullptr)
{
}

Wind_Waves_FFT_Simulation_Compute_Impl::ReadbackArchiveFIFOSlot::~ReadbackArchiveFIFOSlot() =
    default;

Wind_Waves_FFT_Simulation_Compute_Impl::Wind_Waves_FFT_Simulation_Compute_Impl(
    Wind_Waves_Simulation_Manager_Compute_Impl* manager,
    const Wind_Waves_Detailed_Simulation_Parameters::Cascade& cascade)
    : m_manager(manager)
    , m_params(cascade)
    , m_fftSize(0)
    , m_halfFFTSizePlusOne(0)
    , m_clearDisplacementTexture(true)
    , m_gaussOmegaReady(false)
    , m_refreshH0Pending(false)
    , m_padding0{}
    , m_readbackReadIndex(0)
    , m_readbackWriteIndex(1)
    , m_readbacksReady(false)
    , m_padding1{}
    , m_readbackKickIds{kInvalidFrameIndex, kInvalidFrameIndex, kInvalidFrameIndex,
                         kInvalidFrameIndex}
    , m_displacementMapVersion(kInvalidFrameIndex)
    , m_timerResultsMs{}
    , m_timerReadIndex(0)
    , m_timerWriteIndex(1)
    , m_device(nullptr)
    , m_cmdList(nullptr)
    , m_gaussBuffer(nullptr)
    , m_omegaBuffer(nullptr)
    , m_h0Buffer(nullptr)
    , m_htBuffer(nullptr)
    , m_dtBuffer(nullptr)
    , m_displacementTexture(nullptr)
    , m_constantBuffer(nullptr)
    , m_readbackStaging{}
    , m_readbackEvents{}
    , m_currentReadbackStaging(nullptr)
    , m_updateH0Pipeline(nullptr)
    , m_updateH0BindingSet(nullptr)
    , m_updateH0BindingLayout(nullptr)
    , m_updateColumnsPipeline(nullptr)
    , m_updateColumnsBindingSet(nullptr)
    , m_updateColumnsBindingLayout(nullptr)
    , m_updateHtPipeline(nullptr)
    , m_updateHtBindingSet(nullptr)
    , m_updateHtBindingLayout(nullptr)
    , m_readbackArchive(nullptr)
    , m_timerQueries{}
    , m_updateH0Shader(nullptr)
    , m_updateHtShader(nullptr)
    , m_updateColumnsShader(nullptr)
{
    if (m_params.numGPUs == 1)
        m_clearDisplacementTexture = false;
}

Wind_Waves_FFT_Simulation_Compute_Impl::~Wind_Waves_FFT_Simulation_Compute_Impl()
{
    releaseAllResources();
}

WWRESULT Wind_Waves_FFT_Simulation_Compute_Impl::allocateAllResources()
{
    if (!m_device || !isPowerOfTwo(m_params.textureSize))
        return WWE_FAIL;

    const uint32_t N = m_params.textureSize;
    const uint32_t halfN = (N >> 1u) + 1u;
    const size_t fullSampleCount = static_cast<size_t>(N) * static_cast<size_t>(N);
    const size_t paddedSampleCount =
        static_cast<size_t>(N + 1u) * static_cast<size_t>(N + 1u);
    const size_t halfSampleCount = static_cast<size_t>(N) * static_cast<size_t>(halfN);
    const size_t halfSquareSampleCount = static_cast<size_t>(halfN) * halfN;

    uint32_t byteSize = 0;
    nvrhi::BufferDesc bufferDesc;
    bufferDesc.structStride = sizeof(float2);
    bufferDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    bufferDesc.keepInitialState = true;

    if (!checkedByteSize(fullSampleCount, sizeof(float2), byteSize))
        return WWE_FAIL;
    bufferDesc.byteSize = byteSize;
    bufferDesc.debugName = "Gauss Buffer";
    m_device->createBuffer(bufferDesc, &m_gaussBuffer);

    if (!checkedByteSize(halfSquareSampleCount, sizeof(float2), byteSize))
        return WWE_FAIL;
    bufferDesc.byteSize = byteSize;
    bufferDesc.debugName = "Omega Buffer";
    m_device->createBuffer(bufferDesc, &m_omegaBuffer);

    bufferDesc.canHaveUAVs = true;
    bufferDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;

    if (!checkedByteSize(paddedSampleCount, sizeof(float2), byteSize))
        return WWE_FAIL;
    bufferDesc.byteSize = byteSize;
    bufferDesc.debugName = "H0 Buffer";
    m_device->createBuffer(bufferDesc, &m_h0Buffer);

    if (!checkedByteSize(halfSampleCount, sizeof(float2), byteSize))
        return WWE_FAIL;
    bufferDesc.byteSize = byteSize;
    bufferDesc.debugName = "Ht Buffer";
    m_device->createBuffer(bufferDesc, &m_htBuffer);

    if (!checkedByteSize(halfSampleCount, sizeof(float4), byteSize))
        return WWE_FAIL;
    bufferDesc.byteSize = byteSize;
    bufferDesc.structStride = sizeof(float4);
    bufferDesc.debugName = "Dt Buffer";
    m_device->createBuffer(bufferDesc, &m_dtBuffer);

    nvrhi::TextureDesc textureDesc;
    textureDesc.width = N;
    textureDesc.height = N;
    textureDesc.depth = 1;
    textureDesc.arraySize = 1;
    textureDesc.mipLevels = 1;
    textureDesc.sampleCount = 1;
    textureDesc.sampleQuality = 0;
    textureDesc.format = nvrhi::Format::RGBA16_FLOAT;
    textureDesc.dimension = nvrhi::TextureDimension::Texture2D;
    textureDesc.debugName = "Displacement Texture";
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

    bufferDesc = nvrhi::BufferDesc();
    bufferDesc.byteSize = 128;
    bufferDesc.debugName = "Wind waves constant buffer";
    bufferDesc.isConstantBuffer = true;
    bufferDesc.isVolatile = true;
    bufferDesc.maxVersions = 4096;
    bufferDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
    bufferDesc.keepInitialState = true;
    m_device->createBuffer(bufferDesc, &m_constantBuffer);

    if (!m_gaussBuffer || !m_omegaBuffer || !m_h0Buffer || !m_htBuffer || !m_dtBuffer ||
        !m_displacementTexture || !m_constantBuffer)
    {
        return WWE_FAIL;
    }

    m_fftSize = N;
    m_halfFFTSizePlusOne = halfN;

    if (m_params.writeDisplacementMap)
    {
        nvrhi::TextureDesc stagingDesc = textureDesc;
        stagingDesc.isUAV = false;
        stagingDesc.debugName = "Displacement Staging Texture";

        for (uint32_t i = 0; i < kReadbackSlotCount; ++i)
        {
            m_device->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read, &m_readbackStaging[i]);
            m_device->createEventQuery(&m_readbackEvents[i]);
            if (!m_readbackStaging[i] || !m_readbackEvents[i])
                return WWE_FAIL;

            m_readbackKickIds[i] = kInvalidFrameIndex;
            m_device->resetEventQuery(m_readbackEvents[i]);
        }

        m_readbackReadIndex = 0;
        m_readbackWriteIndex = 1;
        m_currentReadbackStaging = nullptr;

        if (m_params.archiveSlots != 0)
        {
            m_readbackArchive = new (std::nothrow) ReadbackArchiveFIFO();
            if (!m_readbackArchive)
                return WWE_FAIL;

            m_readbackArchive->entries =
                new (std::nothrow) ReadbackArchiveFIFOSlot[m_params.archiveSlots];
            if (!m_readbackArchive->entries)
                return WWE_FAIL;

            m_readbackArchive->capacity = m_params.archiveSlots;
            m_readbackArchive->startIndex = 0;
            m_readbackArchive->count = 0;
            m_readbackArchive->reserved = 0;

            for (uint32_t i = 0; i < m_readbackArchive->capacity; ++i)
            {
                m_readbackArchive->entries[i].frameIndex = kInvalidFrameIndex;
                m_device->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read, &m_readbackArchive->entries[i].staging);
                if (!m_readbackArchive->entries[i].staging)
                    return WWE_FAIL;
            }
        }

        m_readbacksReady = true;
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
        m_updateH0Shader = createComputeShader(
            m_device,
            UpdateH0CS_bytecode_vk,
            UpdateH0CS_bytecode_vk_size);
        m_updateHtShader = createComputeShader(
            m_device,
            UpdateHtAndRowsCS_bytecode_vk,
            UpdateHtAndRowsCS_bytecode_vk_size);
        m_updateColumnsShader = createComputeShader(
            m_device,
            UpdateColumnsAndWriteDisplacementsCS_bytecode_vk,
            UpdateColumnsAndWriteDisplacementsCS_bytecode_vk_size);
    }
    else
    {
        m_updateH0Shader = createComputeShader(
            m_device,
            UpdateH0CS_bytecode_dx,
            UpdateH0CS_bytecode_dx_size);
        m_updateHtShader = createComputeShader(
            m_device,
            UpdateHtAndRowsCS_bytecode_dx,
            UpdateHtAndRowsCS_bytecode_dx_size);
        m_updateColumnsShader = createComputeShader(
            m_device,
            UpdateColumnsAndWriteDisplacementsCS_bytecode_dx,
            UpdateColumnsAndWriteDisplacementsCS_bytecode_dx_size);
    }
    if (!m_updateH0Shader || !m_updateHtShader || !m_updateColumnsShader)
        return WWE_FAIL;

    const nvrhi::ResourceType structuredSRV = nvrhi::ResourceType::StructuredBuffer_SRV;
    const nvrhi::ResourceType structuredUAV = nvrhi::ResourceType::StructuredBuffer_UAV;
    const nvrhi::ResourceType textureUAV = nvrhi::ResourceType::Texture_UAV;
    const nvrhi::ResourceType constantBuffer =
        m_constantBuffer->getDesc().isVolatile
            ? nvrhi::ResourceType::VolatileConstantBuffer
            : nvrhi::ResourceType::ConstantBuffer;

    m_updateH0BindingLayout = createComputeBindingLayout(
        m_device,
        {bindingLayoutItem(0, structuredSRV),
         bindingLayoutItem(0, structuredUAV),
         bindingLayoutItem(0, constantBuffer)});
    m_updateH0BindingSet = createComputeBindingSet(
        m_device,
        m_updateH0BindingLayout,
        {nvrhi::BindingSetItem::StructuredBuffer_SRV(0, m_gaussBuffer),
         nvrhi::BindingSetItem::StructuredBuffer_UAV(0, m_h0Buffer),
         nvrhi::BindingSetItem::ConstantBuffer(0, m_constantBuffer)});
    m_updateH0Pipeline =
        createComputePipeline(m_device, m_updateH0Shader, m_updateH0BindingLayout);

    m_updateHtBindingLayout = createComputeBindingLayout(
        m_device,
        {bindingLayoutItem(0, structuredSRV),
         bindingLayoutItem(1, structuredSRV),
         bindingLayoutItem(0, structuredUAV),
         bindingLayoutItem(1, structuredUAV),
         bindingLayoutItem(0, constantBuffer)});
    m_updateHtBindingSet = createComputeBindingSet(
        m_device,
        m_updateHtBindingLayout,
        {nvrhi::BindingSetItem::StructuredBuffer_SRV(0, m_h0Buffer),
         nvrhi::BindingSetItem::StructuredBuffer_SRV(1, m_omegaBuffer),
         nvrhi::BindingSetItem::StructuredBuffer_UAV(0, m_htBuffer),
         nvrhi::BindingSetItem::StructuredBuffer_UAV(1, m_dtBuffer),
         nvrhi::BindingSetItem::ConstantBuffer(0, m_constantBuffer)});
    m_updateHtPipeline =
        createComputePipeline(m_device, m_updateHtShader, m_updateHtBindingLayout);

    m_updateColumnsBindingLayout = createComputeBindingLayout(
        m_device,
        {bindingLayoutItem(0, structuredSRV),
         bindingLayoutItem(1, structuredSRV),
         bindingLayoutItem(0, textureUAV),
         bindingLayoutItem(0, constantBuffer)});
    m_updateColumnsBindingSet = createComputeBindingSet(
        m_device,
        m_updateColumnsBindingLayout,
        {nvrhi::BindingSetItem::StructuredBuffer_SRV(0, m_htBuffer),
         nvrhi::BindingSetItem::StructuredBuffer_SRV(1, m_dtBuffer),
         nvrhi::BindingSetItem::Texture_UAV(
             0, m_displacementTexture, nvrhi::Format::RGBA16_FLOAT),
         nvrhi::BindingSetItem::ConstantBuffer(0, m_constantBuffer)});
    m_updateColumnsPipeline = createComputePipeline(
        m_device, m_updateColumnsShader, m_updateColumnsBindingLayout);

    if (!m_updateH0BindingLayout || !m_updateH0BindingSet || !m_updateH0Pipeline ||
        !m_updateHtBindingLayout || !m_updateHtBindingSet || !m_updateHtPipeline ||
        !m_updateColumnsBindingLayout || !m_updateColumnsBindingSet ||
        !m_updateColumnsPipeline)
    {
        return WWE_FAIL;
    }

    m_gaussOmegaReady = false;
    m_refreshH0Pending = true;
    m_displacementMapVersion = kInvalidFrameIndex;
    return WWE_OK;
}

WWRESULT Wind_Waves_FFT_Simulation_Compute_Impl::archiveDisplacements()
{
    if (!m_params.writeDisplacementMap || !m_readbacksReady)
        return WWE_OK;

    const uint64_t frameIndex = m_readbackKickIds[m_readbackReadIndex];
    if (frameIndex == kInvalidFrameIndex || !m_readbackArchive)
        return WWE_OK;

    ReadbackArchiveFIFO& archive = *m_readbackArchive;
    if (archive.count != 0)
    {
        const uint32_t lastIndex =
            (archive.startIndex + archive.count - 1u) % archive.capacity;
        if (archive.entries[lastIndex].frameIndex == frameIndex)
            return WWE_FAIL;
    }

    if (archive.count == archive.capacity)
    {
        archive.startIndex = (archive.startIndex + 1u) % archive.capacity;
        --archive.count;
    }

    const uint32_t archiveIndex =
        (archive.startIndex + archive.count) % archive.capacity;
    ReadbackArchiveFIFOSlot& slot = archive.entries[archiveIndex];

    m_readbackStaging[m_readbackReadIndex] = slot.staging;
    slot.staging = m_currentReadbackStaging;
    slot.frameIndex = frameIndex;
    ++archive.count;
    return WWE_OK;
}

WWRESULT Wind_Waves_FFT_Simulation_Compute_Impl::canCollectSingleReadbackResultWithoutBlocking()
{
    if (!m_readbacksReady)
        return WWE_FAIL;

    const uint32_t next = nextRingIndex(m_readbackReadIndex);
    if (next == m_readbackWriteIndex)
        return WWE_NONE;

    if (!m_device || !m_readbackEvents[next] || !m_device->pollEventQuery(m_readbackEvents[next]))
        return WWE_NONE;

    return WWE_OK;
}

WWRESULT Wind_Waves_FFT_Simulation_Compute_Impl::collectSingleReadbackResult(bool block)
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

WWRESULT Wind_Waves_FFT_Simulation_Compute_Impl::consumeAvailableReadbackSlot(
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

WWRESULT Wind_Waves_FFT_Simulation_Compute_Impl::consumeAvailableTimerSlot(
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

WWRESULT Wind_Waves_FFT_Simulation_Compute_Impl::getDisplacementArchiveCoords(
    float time,
    uint32_t* prevIndex,
    uint32_t* nextIndex,
    float* interpolation) const
{
    if (!m_readbackArchive || m_readbackArchive->count == 0)
        return WWE_NONE;

    const uint32_t lastIndex = m_readbackArchive->count - 1u;
    const float clamped =
        std::fmin(static_cast<float>(lastIndex), std::fmax(0.0f, time));
    const float base = std::floor(clamped);
    const uint32_t prev = static_cast<uint32_t>(base);
    uint32_t next = prev;
    if (clamped - base != 0.0f)
        next = prev + 1u;
    if (next > lastIndex)
        next = lastIndex;

    if (prevIndex)
        *prevIndex = prev;
    if (nextIndex)
        *nextIndex = next;
    if (interpolation)
        *interpolation = clamped - base;
    return WWE_OK;
}

nvrhi::TextureHandle Wind_Waves_FFT_Simulation_Compute_Impl::getDisplacementMap() const
{
    return m_displacementTexture;
}

uint64_t Wind_Waves_FFT_Simulation_Compute_Impl::getDisplacementMapVersion() const
{
    return m_displacementMapVersion;
}

bool Wind_Waves_FFT_Simulation_Compute_Impl::getReadbackCursor(uint64_t* cursor) const
{
    if (!m_params.writeDisplacementMap || !m_readbacksReady)
        return false;

    const uint64_t frameIndex = m_readbackKickIds[m_readbackReadIndex];
    if (frameIndex == kInvalidFrameIndex)
        return false;

    if (cursor)
        *cursor = frameIndex;
    return true;
}

float Wind_Waves_FFT_Simulation_Compute_Impl::getSimulationTime() const
{
    return m_timerResultsMs[m_timerReadIndex];
}

bool Wind_Waves_FFT_Simulation_Compute_Impl::hasReadbacksInFlight() const
{
    return m_params.writeDisplacementMap && m_readbacksReady &&
           nextRingIndex(m_readbackReadIndex) != m_readbackWriteIndex;
}

WWRESULT Wind_Waves_FFT_Simulation_Compute_Impl::init(
    nvrhi::IDevice* device,
    nvrhi::CommandListHandle cmdList,
    bool useCompute)
{
    if (m_device)
        releaseAllResources();

    if (!useCompute)
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

WWRESULT Wind_Waves_FFT_Simulation_Compute_Impl::initGaussAndOmega()
{
    if (!m_cmdList || !m_gaussBuffer || !m_omegaBuffer)
        return WWE_FAIL;

    const uint32_t N = m_fftSize;
    const uint32_t halfN = m_halfFFTSizePlusOne;
    if (N == 0 || halfN == 0)
        return WWE_FAIL;

    std::vector<float2> gaussUpload(static_cast<size_t>(N) * N, make_float2(0.0f));
    if (N <= kGaussSourceSize)
    {
        std::vector<float2> gaussSource(
            static_cast<size_t>(kGaussSourceStride) * (kGaussSourceSize + 1u),
            make_float2(0.0f));
        Simulation_Util::initGaussData(gaussSource.data());

        const uint32_t offset = (kGaussSourceSize - N) >> 1u;
        for (uint32_t row = 0; row < N; ++row)
        {
            const float2* src =
                gaussSource.data() + static_cast<size_t>(row + offset) * kGaussSourceStride +
                offset;
            float2* dst = gaussUpload.data() + static_cast<size_t>(row) * N;
            std::memcpy(dst, src, sizeof(float2) * N);
        }
    }

    std::vector<float2> omegaScratch(static_cast<size_t>(N + 4u) * (N + 1u),
                                     make_float2(0.0f));
    std::vector<float2> omegaUpload(static_cast<size_t>(halfN) * halfN, make_float2(0.0f));
    Simulation_Util::initOmegaData(m_params, omegaScratch.data());
    for (uint32_t row = 0; row < halfN; ++row)
    {
        const float2* src = omegaScratch.data() + static_cast<size_t>(row) * (N + 4u);
        float2* dst = omegaUpload.data() + static_cast<size_t>(row) * halfN;
        std::memcpy(dst, src, sizeof(float2) * halfN);
    }

    m_cmdList->writeBuffer(m_gaussBuffer, gaussUpload.data(),
                           gaussUpload.size() * sizeof(float2));
    m_cmdList->writeBuffer(m_omegaBuffer, omegaUpload.data(),
                           omegaUpload.size() * sizeof(float2));

    m_gaussOmegaReady = true;
    m_refreshH0Pending = true;
    return WWE_OK;
}

WWRESULT Wind_Waves_FFT_Simulation_Compute_Impl::kick(double simulationTime, uint64_t kickId)
{
    if (!m_gaussOmegaReady)
    {
        const WWRESULT result = initGaussAndOmega();
        if (result < 0)
            return result;
    }

    uint32_t timerSlot = 0;
    WWRESULT timerResult = consumeAvailableTimerSlot(timerSlot, kickId);
    if (timerResult < 0)
        return timerResult;

    uint32_t readbackSlot = 0;
    if (m_readbacksReady)
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

    m_cmdList->beginMarker("WaveWorks: FFT for cascade");
    updateConstantBuffer(simulationTime);

    if (m_clearDisplacementTexture)
    {
        m_cmdList->clearTextureFloat(
            m_displacementTexture, nvrhi::AllSubresources, nvrhi::Color(0.0f));
    }

    nvrhi::ComputeState computeState;
    auto dispatchPipeline =
        [&](nvrhi::IComputePipeline* pipeline,
            nvrhi::IBindingSet* bindingSet,
            uint32_t groupsX,
            uint32_t groupsY)
    {
        computeState.pipeline = pipeline;
        computeState.bindings.resize(0);
        computeState.bindings.push_back(bindingSet);
        m_cmdList->setComputeState(computeState);
        m_cmdList->dispatch(groupsX, groupsY, 1u);
    };

    if (m_refreshH0Pending)
    {
        dispatchPipeline(m_updateH0Pipeline, m_updateH0BindingSet, 1u, m_fftSize);
        m_refreshH0Pending = false;
    }
    dispatchPipeline(m_updateHtPipeline, m_updateHtBindingSet, 1u, m_halfFFTSizePlusOne);
    dispatchPipeline(m_updateColumnsPipeline, m_updateColumnsBindingSet, 1u, m_fftSize);

    m_cmdList->endMarker();
    if (m_timerQueries[timerSlot])
        m_cmdList->endTimerQuery(m_timerQueries[timerSlot]);

    if (m_readbacksReady)
    {
        const nvrhi::TextureSlice slice = fullResolutionSlice();
        m_cmdList->copyTexture2(m_readbackStaging[readbackSlot], slice, m_displacementTexture, slice);
        m_device->resetEventQuery(m_readbackEvents[readbackSlot]);
        m_device->pollEventQuery(m_readbackEvents[readbackSlot]);
        m_device->setEventQuery(m_readbackEvents[readbackSlot], nvrhi::CommandQueue::Graphics);
    }

    m_displacementMapVersion = kickId;
    return WWE_OK;
}

WWRESULT Wind_Waves_FFT_Simulation_Compute_Impl::mapDisplacementArchiveData(
    uint8_t** data,
    uint32_t* rowPitch,
    uint32_t age,
    bool* outReadOnly)
{
    const ReadbackArchiveFIFO& archive = *m_readbackArchive;
    const uint32_t index =
        (archive.startIndex + archive.count - age - 1u) % archive.capacity;
    const ReadbackArchiveFIFOSlot& slot = archive.entries[index];
    size_t mappedRowPitch = 0;
    *data = reinterpret_cast<uint8_t*>(
        m_device->mapStagingTexture(slot.staging, fullResolutionSlice(),
                                    nvrhi::CpuAccessMode::Read, mappedRowPitch));
    *rowPitch = static_cast<uint32_t>(mappedRowPitch);
    *outReadOnly = true;
    return WWE_OK;
}

WWRESULT Wind_Waves_FFT_Simulation_Compute_Impl::mapDisplacementData(
    uint8_t** data,
    uint32_t* rowPitch,
    bool* outReadOnly)
{
    if (!getReadbackCursor(nullptr) || !m_currentReadbackStaging)
        return WWE_NONE;

    size_t mappedRowPitch = 0;
    *data = reinterpret_cast<uint8_t*>(
        m_device->mapStagingTexture(m_currentReadbackStaging, fullResolutionSlice(),
                                    nvrhi::CpuAccessMode::Read, mappedRowPitch));
    *rowPitch = static_cast<uint32_t>(mappedRowPitch);
    *outReadOnly = true;
    return WWE_OK;
}

WWRESULT Wind_Waves_FFT_Simulation_Compute_Impl::reinit(
    const Wind_Waves_Detailed_Simulation_Parameters::Cascade& cascade)
{
    const bool needsReallocation =
        cascade.textureSize != m_params.textureSize ||
        cascade.writeDisplacementMap != m_params.writeDisplacementMap;

    if (needsReallocation && m_manager)
    {
        const WWRESULT result = m_manager->beforeReallocateSimulation();
        if (result < 0)
            return result;
    }

    if (cascade.physicalSize != m_params.physicalSize)
        m_gaussOmegaReady = false;

    if (h0InputsChanged(cascade, m_params))
        m_refreshH0Pending = true;

    if (!needsReallocation)
    {
        m_params = cascade;
        return WWE_OK;
    }

    releaseAllResources();
    m_params = cascade;

    if (!m_device)
        return WWE_OK;

    const WWRESULT result = allocateAllResources();
    return result < 0 ? result : WWE_OK;
}

void Wind_Waves_FFT_Simulation_Compute_Impl::releaseAllResources()
{
    waitForAllInFlightReadbacks();
    waitForAllInFlightTimers();

    if (m_readbackArchive)
    {
        delete[] m_readbackArchive->entries;
        m_readbackArchive->entries = nullptr;
        delete m_readbackArchive;
        m_readbackArchive = nullptr;
    }

    m_gaussBuffer = nullptr;
    m_omegaBuffer = nullptr;
    m_h0Buffer = nullptr;
    m_htBuffer = nullptr;
    m_dtBuffer = nullptr;
    m_displacementTexture = nullptr;
    m_constantBuffer = nullptr;

    for (uint32_t i = 0; i < kReadbackSlotCount; ++i)
    {
        m_readbackStaging[i] = nullptr;
        m_readbackEvents[i] = nullptr;
        m_timerQueries[i] = nullptr;
        m_readbackKickIds[i] = kInvalidFrameIndex;
        m_timerResultsMs[i] = 0.0f;
    }

    m_currentReadbackStaging = nullptr;
    releasePipelineResources();
    m_readbacksReady = false;
    m_gaussOmegaReady = false;
    m_refreshH0Pending = true;
    m_readbackReadIndex = 0;
    m_readbackWriteIndex = 1;
    m_timerReadIndex = 0;
    m_timerWriteIndex = 1;
}

WWRESULT Wind_Waves_FFT_Simulation_Compute_Impl::resetReadbacks()
{
    if (!m_readbacksReady)
        return WWE_OK;

    const WWRESULT result = waitForAllInFlightReadbacks();
    if (result < 0)
        return result;

    m_readbackReadIndex = 0;
    m_readbackWriteIndex = 1;
    for (uint64_t& kickId : m_readbackKickIds)
        kickId = kInvalidFrameIndex;
    m_currentReadbackStaging = nullptr;
    return WWE_OK;
}

void Wind_Waves_FFT_Simulation_Compute_Impl::unmapDisplacementArchiveData(uint32_t age)
{
    if (!m_device || !m_readbackArchive || age >= m_readbackArchive->count)
        return;

    const ReadbackArchiveFIFO& archive = *m_readbackArchive;
    const uint32_t index =
        (archive.startIndex + archive.count - age - 1u) % archive.capacity;
    if (archive.entries[index].staging)
        m_device->unmapStagingTexture(archive.entries[index].staging);
}

void Wind_Waves_FFT_Simulation_Compute_Impl::unmapDisplacementData()
{
    if (m_device && m_currentReadbackStaging)
        m_device->unmapStagingTexture(m_currentReadbackStaging);
}

void Wind_Waves_FFT_Simulation_Compute_Impl::updateConstantBuffer(double simulationTime)
{
    std::array<uint32_t, 32> constants{};
    constants[0] = m_fftSize;
    constants[1] = m_fftSize + 1u;
    constants[2] = m_fftSize >> 1u;
    constants[3] = m_halfFFTSizePlusOne;
    constants[4] = (m_fftSize + 1u) * (m_fftSize + 1u) - 1u;

    uint32_t fftLog2 = 0;
    uint32_t fftPower = 0;
    Simulation_Util::powerOf2(m_fftSize, &fftLog2, &fftPower);
    constants[5] = 32u - fftLog2;
    writeFloat(constants, 6, m_params.lowerGridRadius);
    writeFloat(constants, 7, m_params.upperGridRadius);
    writeFloat(constants, 8, TwoPif / m_params.physicalSize);
    writeFloat(constants, 9, m_params.displacementScale);
    writeDouble(constants, 10, simulationTime);

    const float2 baseDirection = normalizedOrXAxis(m_params.baseWindDirection);
    writeFloat(constants, 12, baseDirection.x);
    writeFloat(constants, 13, baseDirection.y);
    writeFloat(constants, 14, m_params.baseWindSpeed);
    writeFloat(constants, 15, m_params.baseWindDistance);
    writeFloat(constants, 16, m_params.baseWindDependency);
    writeFloat(constants, 17, m_params.baseSmallWavesCutoffLength);
    writeFloat(constants, 18, m_params.baseSmallWavesCutoffPower);
    writeFloat(constants, 19, m_params.baseSpectrumPeaking);

    const float2 swellDirection = normalizedOrXAxis(m_params.swellWindDirection);
    writeFloat(constants, 20, swellDirection.x);
    writeFloat(constants, 21, swellDirection.y);
    writeFloat(constants, 22, m_params.baseAmplitudeMultiplier / m_params.physicalSize);
    writeFloat(constants, 23, m_params.swellWindSpeed);
    writeFloat(constants, 24, m_params.swellWindDistance);
    writeFloat(constants, 25, m_params.swellWindDependency);
    writeFloat(constants, 26, m_params.swellSmallWavesCutoffLength);
    writeFloat(constants, 27, m_params.swellSmallWavesCutoffPower);
    writeFloat(constants, 28, m_params.swellSpectrumPeaking);
    writeFloat(constants, 29, m_params.swellAmplitudeMultiplier / m_params.physicalSize);

    m_cmdList->writeBuffer(m_constantBuffer, constants.data(), constants.size() * sizeof(uint32_t));
}

WWRESULT Wind_Waves_FFT_Simulation_Compute_Impl::waitForAllInFlightReadbacks()
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

WWRESULT Wind_Waves_FFT_Simulation_Compute_Impl::waitForAllInFlightTimers()
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

nvrhi::TextureSlice Wind_Waves_FFT_Simulation_Compute_Impl::fullResolutionSlice() const
{
    nvrhi::TextureSlice slice;
    slice.width = m_fftSize;
    slice.height = m_fftSize;
    slice.depth = 1;
    return slice;
}

void Wind_Waves_FFT_Simulation_Compute_Impl::releasePipelineResources()
{
    m_updateH0Pipeline = nullptr;
    m_updateH0BindingSet = nullptr;
    m_updateH0BindingLayout = nullptr;
    m_updateColumnsPipeline = nullptr;
    m_updateColumnsBindingSet = nullptr;
    m_updateColumnsBindingLayout = nullptr;
    m_updateHtPipeline = nullptr;
    m_updateHtBindingSet = nullptr;
    m_updateHtBindingLayout = nullptr;
    m_updateH0Shader = nullptr;
    m_updateHtShader = nullptr;
    m_updateColumnsShader = nullptr;
}
