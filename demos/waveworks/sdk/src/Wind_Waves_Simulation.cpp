#include "Wind_Waves_Simulation.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <limits>
#include <utility>
#include <vector>

#include "Simulation_Util.h"
#include "Wind_Waves_Simulation_Manager_CPU_Impl.h"
#include "Wind_Waves_Simulation_Manager_Compute_Impl.h"

#if defined(_WIN32)
#include <nvrhi/d3d11.h>
#include <nvrhi/d3d12.h>
#endif

#include <nvrhi/vulkan.h>
#include <nvrhi/factory.h>

bool InitializeWaveWorksVulkanDispatch(VkInstance instance, VkDevice device);

#include "ShaderBytecode.h"

void handle_ww_error(const wchar_t* filename, uint32_t line);

namespace
{
constexpr uint32_t kCascadeCount = 4;
constexpr uint32_t kTimerSlotCount = 4;
constexpr uint32_t kTimerSlotMask = kTimerSlotCount - 1;
constexpr uint32_t kMaxGpuSampleCount = 0x100000;
constexpr uint64_t kInvalidVersion = std::numeric_limits<uint64_t>::max();

uint32_t nextTimerSlot(uint32_t slot)
{
    return (slot + 1u) & kTimerSlotMask;
}

bool isValidSimulationAPI(GFSDK_WaveWorks_Simulation_API api)
{
    return api == GFSDK_WaveWorks_Simulation_API_CPU ||
           api == GFSDK_WaveWorks_Simulation_API_Compute;
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

nvrhi::TextureSubresourceSet singleMip(uint32_t mipLevel)
{
    return nvrhi::TextureSubresourceSet(mipLevel, 1, 0, 1);
}

nvrhi::TextureSubresourceSet arrayMip(uint32_t mipLevel)
{
    return nvrhi::TextureSubresourceSet(mipLevel, 1, 0, kCascadeCount);
}

uint32_t clampedSecondBatchMip(uint32_t mipExponent, uint32_t targetMip, uint32_t fallbackMip)
{
    return mipExponent >= targetMip ? targetMip : fallbackMip;
}

void zeroStats(GFSDK_WaveWorks_Wind_Waves_Simulation_Stats& stats)
{
    stats.CPU_main_thread_wait_time = 0.0f;
    stats.CPU_threads_total_time = 0.0f;
    stats.GPU_simulation_time = 0.0f;
    stats.GPU_update_time = 0.0f;
    stats.GPU_total_time = 0.0f;
}

float clampPositive(float value)
{
    return std::max(value, 0.0001f);
}

WWRESULT mapCursorAdvanceResult(WWRESULT result, bool& wouldBlock)
{
    if (result == WWE_OK || result == WWE_NONE)
    {
        wouldBlock = false;
        return result;
    }

    if (result == WWE_WOULD_BLOCK)
    {
        wouldBlock = true;
        return result;
    }

    if (result == WWE_FAIL)
        return WWE_FAIL;

    return WWE_INTERNAL_ERROR;
}

nvrhi::TextureHandle getDisplacementMap(Wind_Waves_FFT_Simulation* simulation)
{
    if (auto* cpuSimulation = dynamic_cast<Wind_Waves_FFT_Simulation_CPU_Impl*>(simulation))
        return cpuSimulation->getDisplacementMap();
    if (auto* computeSimulation =
            dynamic_cast<Wind_Waves_FFT_Simulation_Compute_Impl*>(simulation))
    {
        return computeSimulation->getDisplacementMap();
    }
    return nullptr;
}

uint64_t getDisplacementMapVersion(Wind_Waves_FFT_Simulation* simulation)
{
    if (auto* cpuSimulation = dynamic_cast<Wind_Waves_FFT_Simulation_CPU_Impl*>(simulation))
        return cpuSimulation->getDisplacementMapVersion();
    if (auto* computeSimulation =
            dynamic_cast<Wind_Waves_FFT_Simulation_Compute_Impl*>(simulation))
    {
        return computeSimulation->getDisplacementMapVersion();
    }
    return kInvalidVersion;
}

void dispatchCompute(
    nvrhi::ICommandList* commandList,
    nvrhi::IComputePipeline* pipeline,
    nvrhi::IBindingSet* bindingSet,
    uint32_t groupsX,
    uint32_t groupsY,
    uint32_t groupsZ)
{
    nvrhi::ComputeState state;
    state.pipeline = pipeline;
    state.bindings.push_back(bindingSet);
    commandList->setComputeState(state);
    commandList->dispatch(groupsX, groupsY, groupsZ);
}

void addDisplacementsWithTasks(
    ThreadPool* threadPool,
    const Wind_Waves_Detailed_Simulation_Parameters::Cascade* cascades,
    const std::vector<uint8_t*>& displacementData,
    const std::vector<uint32_t>& rowPitches,
    const gfsdk_float2* samplePoints,
    gfsdk_float4* displacements,
    uint32_t numSamples,
    bool readOnly)
{
    if (numSamples < 512u || !threadPool)
    {
        for (uint32_t cascadeIndex = 0; cascadeIndex < kCascadeCount; ++cascadeIndex)
        {
            Simulation_Util::addDisplacements(
                cascades[cascadeIndex], displacementData[cascadeIndex],
                rowPitches[cascadeIndex], samplePoints, displacements,
                0, numSamples, 1.0f, readOnly);
        }
        return;
    }

    const uint32_t threadCount = std::max(1u, threadPool->getThreadCount());
    const uint32_t chunkSize = std::max(128u, numSamples / threadCount);
    std::vector<std::function<void()>> tasks;
    for (uint32_t start = 0; start < numSamples; start += chunkSize)
    {
        const uint32_t count = std::min(chunkSize, numSamples - start);
        tasks.emplace_back(
            [cascades, displacementData, rowPitches, samplePoints, displacements,
             start, count, readOnly]() {
                for (uint32_t cascadeIndex = 0; cascadeIndex < kCascadeCount; ++cascadeIndex)
                {
                    Simulation_Util::addDisplacements(
                        cascades[cascadeIndex], displacementData[cascadeIndex],
                        rowPitches[cascadeIndex], samplePoints, displacements,
                        start, count, 1.0f, readOnly);
                }
            });
    }
    threadPool->prepareHighPrioritySubmission(static_cast<unsigned int>(tasks.size()));
    threadPool->submitHighPriorityVector(std::move(tasks));
    threadPool->waitForHighPriorityWorkDone();
}
}

Wind_Waves_Simulation::CascadeState::CascadeState()
    : simulation(nullptr)
    , displacementMapVersion(kInvalidVersion)
    , gpuSlotDirty{}
    , gradientTextures{}
    , momentTextures{}
    , foamEnergyTexture(nullptr)
    , gradientBindingSets{}
    , momentBindingSets{}
{
}

Wind_Waves_Simulation::CascadeState::~CascadeState() = default;

Wind_Waves_Simulation::Wind_Waves_Simulation()
    : m_foamWhitecapsThreshold(0.0f)
    , m_cascades{}
    , m_timeScale(0.0f)
    , m_numGPUs(1)
    , m_simulationAPI(GFSDK_WaveWorks_Simulation_API_CPU)
    , m_cpuThreadingModel(GFSDK_WaveWorks_Simulation_CPU_Threading_Model_Automatic)
    , m_enableGpuTimers(false)
    , m_enableCpuTimers(false)
    , m_enableGpuDrivenDisplacementCalculation(false)
    , m_padding0(0)
    , m_cascadeStates{}
    , m_numGpuSlots(1)
    , m_gpuSlotIndex(0)
    , m_simulationTime(0.0)
    , m_simulationTimeHistory{}
    , m_simulationTimeHistoryCount(0)
    , m_simulationTimeHistoryPadding(0)
    , m_simulationDeltaTime(0.0)
    , m_manager(nullptr)
    , m_threadPool(nullptr)
    , m_device(nullptr)
    , m_commandList(nullptr)
    , m_enableGraphics(false)
    , m_padding1{}
    , m_displacementTextureArray(nullptr)
    , m_gradientTextureArray(nullptr)
    , m_momentTextureArray(nullptr)
    , m_gradientFoamShader(nullptr)
    , m_renderingConstantBuffer(nullptr)
    , m_gradientPipeline(nullptr)
    , m_gradientBindingLayout(nullptr)
    , m_foamInjectionShader(nullptr)
    , m_foamInjectionPipeline(nullptr)
    , m_foamInjectionBindingLayout(nullptr)
    , m_wrapSampler(nullptr)
    , m_gpuSampleShader(nullptr)
    , m_readbackPositionsBuffer(nullptr)
    , m_readbackResultsBuffer(nullptr)
    , m_readbackResultsBufferStaging(nullptr)
    , m_calculateReadbacksCB(nullptr)
    , m_gpuSamplePipeline(nullptr)
    , m_gpuSampleBindingLayout(nullptr)
    , m_gpuSampleBindingSet(nullptr)
    , m_textureArrayShader(nullptr)
    , m_textureArrayMipmapConstantBuffer(nullptr)
    , m_textureArrayPipeline(nullptr)
    , m_textureArrayBindingSets{}
    , m_textureArrayBindingLayout(nullptr)
    , m_mipmapShader(nullptr)
    , m_mipmapPipeline(nullptr)
    , m_displacementMipmapBindingSet0(nullptr)
    , m_displacementMipmapBindingSet1(nullptr)
    , m_gradientMipmapBindingSet0(nullptr)
    , m_gradientMipmapBindingSet1(nullptr)
    , m_momentMipmapBindingSet0(nullptr)
    , m_momentMipmapBindingSet1(nullptr)
    , m_mipmapBindingLayout(nullptr)
    , m_timerResultsMs{}
    , m_timerReadIndex(0)
    , m_timerWriteIndex(1)
    , m_timerQueries{}
{
}

Wind_Waves_Simulation::~Wind_Waves_Simulation()
{
    releaseAll();
    m_commandList = nullptr;
    m_device = nullptr;
}

WWRESULT Wind_Waves_Simulation::advanceReadbackCursor(bool block, bool& wouldBlock)
{
    const WWRESULT result = m_manager->advanceReadbackCursor(block);
    return mapCursorAdvanceResult(result, wouldBlock);
}

WWRESULT Wind_Waves_Simulation::advanceStagingCursor(bool block, bool& wouldBlock)
{
    const WWRESULT result = m_manager->advanceStagingCursor(block);
    WWRESULT mappedResult = mapCursorAdvanceResult(result, wouldBlock);
    if (mappedResult != WWE_OK)
        return mappedResult;

    m_commandList->open();
    updateGradientMaps();
    m_commandList->close();
    m_device->executeCommandList(m_commandList);

    return WWE_OK;
}

WWRESULT Wind_Waves_Simulation::allocateAll()
{
    WWRESULT managerResult = allocateSimulationManager();
    if (managerResult < 0)
        return managerResult;

    for (uint32_t cascadeIndex = 0; cascadeIndex < kCascadeCount; ++cascadeIndex)
    {
        WWRESULT simulationResult = allocateSimulation(cascadeIndex, m_commandList);
        if (simulationResult < 0)
            return simulationResult;
    }

    return allocateRenderingResources();
}

WWRESULT Wind_Waves_Simulation::allocateRenderingResources() {
    // The recovered implementation restarts the AFR slot/time history whenever
    // rendering resources are allocated. Foam integration uses the elapsed time
    // across this history, so retaining stale samples here produces an invalid
    // first post-reinit timestep.
    m_numGpuSlots = m_numGPUs;
    m_gpuSlotIndex = m_numGpuSlots - 1u;
    m_simulationTimeHistoryCount = 0;

    const nvrhi::ResourceType textureSRV = nvrhi::ResourceType::Texture_SRV;
    const nvrhi::ResourceType textureUAV = nvrhi::ResourceType::Texture_UAV;
    const nvrhi::ResourceType sampler = nvrhi::ResourceType::Sampler;
    const nvrhi::ResourceType volatileConstantBuffer =
        nvrhi::ResourceType::VolatileConstantBuffer;

    nvrhi::SamplerDesc samplerDesc;
    samplerDesc.addressU = nvrhi::SamplerAddressMode::Wrap;
    samplerDesc.addressV = nvrhi::SamplerAddressMode::Wrap;
    samplerDesc.addressW = nvrhi::SamplerAddressMode::Wrap;
    samplerDesc.mipBias = 0.0f;
    samplerDesc.maxAnisotropy = 0.0f;
    samplerDesc.minFilter = true;
    samplerDesc.magFilter = true;
    samplerDesc.mipFilter = false;
    samplerDesc.reductionType = nvrhi::SamplerReductionType::Standard;
    samplerDesc.borderColor = nvrhi::Color(0.0f);

    if (m_enableGraphics) {
        const uint32_t textureSize = m_cascades[3].textureSize;
        uint32_t mipExponent = 0;
        uint32_t mipPower = 0;
        Simulation_Util::powerOf2(textureSize, &mipExponent, &mipPower);

        nvrhi::TextureDesc textureDesc;
        textureDesc.width = textureSize;
        textureDesc.height = textureSize;
        textureDesc.depth = 1;
        textureDesc.arraySize = kCascadeCount;
        textureDesc.mipLevels = mipExponent + 1;
        textureDesc.sampleCount = 1;
        textureDesc.sampleQuality = 0;
        textureDesc.format = nvrhi::Format::RGBA16_FLOAT;
        textureDesc.dimension = nvrhi::TextureDimension::Texture2DArray;
        textureDesc.isUAV = true;
        textureDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        textureDesc.keepInitialState = true;

        textureDesc.debugName = "Displacement texture array";
        m_device->createTexture(textureDesc, &m_displacementTextureArray);
        if (m_displacementTextureArray && m_device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN && textureDesc.keepInitialState) {
            // Establish the native image layout before exposing it to the renderer.
            nvrhi::CommandListHandle initializeCommands;
            m_device->createCommandList(nvrhi::CommandListParameters(), &initializeCommands);
            if (!initializeCommands) return WWE_FAIL;
            initializeCommands->open();
            initializeCommands->beginTrackingTextureState(m_displacementTextureArray, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);
            initializeCommands->setTextureState(m_displacementTextureArray, nvrhi::AllSubresources, textureDesc.initialState);
            initializeCommands->commitBarriers();
            initializeCommands->close();
            m_device->executeCommandList(initializeCommands);
        }
        textureDesc.debugName = "Gradients texture array";
        m_device->createTexture(textureDesc, &m_gradientTextureArray);
        if (m_gradientTextureArray && m_device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN && textureDesc.keepInitialState) {
            // Establish the native image layout before exposing it to the renderer.
            nvrhi::CommandListHandle initializeCommands;
            m_device->createCommandList(nvrhi::CommandListParameters(), &initializeCommands);
            if (!initializeCommands) return WWE_FAIL;
            initializeCommands->open();
            initializeCommands->beginTrackingTextureState(m_gradientTextureArray, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);
            initializeCommands->setTextureState(m_gradientTextureArray, nvrhi::AllSubresources, textureDesc.initialState);
            initializeCommands->commitBarriers();
            initializeCommands->close();
            m_device->executeCommandList(initializeCommands);
        }
        textureDesc.debugName = "Moments texture array";
        m_device->createTexture(textureDesc, &m_momentTextureArray);
        if (m_momentTextureArray && m_device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN && textureDesc.keepInitialState) {
            // Establish the native image layout before exposing it to the renderer.
            nvrhi::CommandListHandle initializeCommands;
            m_device->createCommandList(nvrhi::CommandListParameters(), &initializeCommands);
            if (!initializeCommands) return WWE_FAIL;
            initializeCommands->open();
            initializeCommands->beginTrackingTextureState(m_momentTextureArray, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);
            initializeCommands->setTextureState(m_momentTextureArray, nvrhi::AllSubresources, textureDesc.initialState);
            initializeCommands->commitBarriers();
            initializeCommands->close();
            m_device->executeCommandList(initializeCommands);
        }

        if (m_device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN) {
            m_gradientFoamShader = createComputeShader(
                m_device, BlurFoamAndCalculateGradientsAndMomentsCS_bytecode_vk,
                BlurFoamAndCalculateGradientsAndMomentsCS_bytecode_vk_size);
            m_foamInjectionShader =
                createComputeShader(m_device, InjectAndBlurFoamCS_bytecode_vk,
                                    InjectAndBlurFoamCS_bytecode_vk_size);
            m_mipmapShader = createComputeShader(m_device, GenerateMipmapsCS_bytecode_vk,
                                                 GenerateMipmapsCS_bytecode_vk_size);
            m_textureArrayShader =
                createComputeShader(m_device, CopyToTextureArraysCS_bytecode_vk,
                                    CopyToTextureArraysCS_bytecode_vk_size);
        } else {
            m_gradientFoamShader = createComputeShader(
                m_device, BlurFoamAndCalculateGradientsAndMomentsCS_bytecode_dx,
                BlurFoamAndCalculateGradientsAndMomentsCS_bytecode_dx_size);
            m_foamInjectionShader =
                createComputeShader(m_device, InjectAndBlurFoamCS_bytecode_dx,
                                    InjectAndBlurFoamCS_bytecode_dx_size);
            m_mipmapShader = createComputeShader(m_device, GenerateMipmapsCS_bytecode_dx,
                                                 GenerateMipmapsCS_bytecode_dx_size);
            m_textureArrayShader =
                createComputeShader(m_device, CopyToTextureArraysCS_bytecode_dx,
                                    CopyToTextureArraysCS_bytecode_dx_size);
        }
        nvrhi::BufferDesc constantsDesc;
        constantsDesc.byteSize = 32;
        constantsDesc.debugName = "m_hGradientsCalcPixelShaderCB";
        constantsDesc.isConstantBuffer = true;
        constantsDesc.isVolatile = true;
        constantsDesc.maxVersions = 4096;
        constantsDesc.initialState = nvrhi::ResourceStates::Common;
        constantsDesc.keepInitialState = false;
        constantsDesc.cpuAccess = nvrhi::CpuAccessMode::None;
        m_device->createBuffer(constantsDesc, &m_renderingConstantBuffer);

        constantsDesc.byteSize = 16;
        constantsDesc.debugName = "m_hCopyToTextureArrysAndGenerateMipmapsCB";
        m_device->createBuffer(constantsDesc, &m_textureArrayMipmapConstantBuffer);
        m_device->createSampler(samplerDesc, &m_wrapSampler);

        m_gradientBindingLayout = createComputeBindingLayout(
            m_device,
            {bindingLayoutItem(0, textureUAV), bindingLayoutItem(1, textureUAV),
             bindingLayoutItem(2, textureUAV), bindingLayoutItem(0, textureSRV),
             bindingLayoutItem(0, sampler), bindingLayoutItem(0, volatileConstantBuffer)});
        m_gradientPipeline =
            createComputePipeline(m_device, m_gradientFoamShader, m_gradientBindingLayout);

        m_foamInjectionBindingLayout = createComputeBindingLayout(
            m_device,
            {bindingLayoutItem(0, textureSRV), bindingLayoutItem(0, textureUAV),
             bindingLayoutItem(0, sampler), bindingLayoutItem(0, volatileConstantBuffer)});
        m_foamInjectionPipeline = createComputePipeline(m_device, m_foamInjectionShader,
                                                        m_foamInjectionBindingLayout);

        for (uint32_t cascadeIndex = 0; cascadeIndex < kCascadeCount; ++cascadeIndex) {
            CascadeState& state = m_cascadeStates[cascadeIndex];
            const uint32_t cascadeTextureSize = m_cascades[cascadeIndex].textureSize;

            nvrhi::TextureDesc cascadeTextureDesc;
            cascadeTextureDesc.width = cascadeTextureSize;
            cascadeTextureDesc.height = cascadeTextureSize;
            cascadeTextureDesc.depth = 1;
            cascadeTextureDesc.arraySize = 1;
            cascadeTextureDesc.mipLevels = 1;
            cascadeTextureDesc.sampleCount = 1;
            cascadeTextureDesc.sampleQuality = 0;
            cascadeTextureDesc.format = nvrhi::Format::RGBA16_FLOAT;
            cascadeTextureDesc.dimension = nvrhi::TextureDimension::Texture2D;
            cascadeTextureDesc.isUAV = true;
            cascadeTextureDesc.initialState = nvrhi::ResourceStates::ShaderResource;
            cascadeTextureDesc.keepInitialState = true;

            for (uint32_t slot = 0; slot < m_numGpuSlots; ++slot) {
                cascadeTextureDesc.debugName = "Gradient texture for cascade";
                m_device->createTexture(cascadeTextureDesc, &state.gradientTextures[slot]);
                if (state.gradientTextures[slot] && m_device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN && cascadeTextureDesc.keepInitialState) {
                    // Establish the native image layout before exposing it to the renderer.
                    nvrhi::CommandListHandle initializeCommands;
                    m_device->createCommandList(nvrhi::CommandListParameters(), &initializeCommands);
                    if (!initializeCommands) return WWE_FAIL;
                    initializeCommands->open();
                    initializeCommands->beginTrackingTextureState(state.gradientTextures[slot], nvrhi::AllSubresources, nvrhi::ResourceStates::Common);
                    initializeCommands->setTextureState(state.gradientTextures[slot], nvrhi::AllSubresources, cascadeTextureDesc.initialState);
                    initializeCommands->commitBarriers();
                    initializeCommands->close();
                    m_device->executeCommandList(initializeCommands);
                }
                state.gpuSlotDirty[slot] = true;
                cascadeTextureDesc.debugName = "Moments texture for cascade";
                m_device->createTexture(cascadeTextureDesc, &state.momentTextures[slot]);
                if (state.momentTextures[slot] && m_device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN && cascadeTextureDesc.keepInitialState) {
                    // Establish the native image layout before exposing it to the renderer.
                    nvrhi::CommandListHandle initializeCommands;
                    m_device->createCommandList(nvrhi::CommandListParameters(), &initializeCommands);
                    if (!initializeCommands) return WWE_FAIL;
                    initializeCommands->open();
                    initializeCommands->beginTrackingTextureState(state.momentTextures[slot], nvrhi::AllSubresources, nvrhi::ResourceStates::Common);
                    initializeCommands->setTextureState(state.momentTextures[slot], nvrhi::AllSubresources, cascadeTextureDesc.initialState);
                    initializeCommands->commitBarriers();
                    initializeCommands->close();
                    m_device->executeCommandList(initializeCommands);
                }
            }

            cascadeTextureDesc.format = nvrhi::Format::R16_FLOAT;
            m_device->createTexture(cascadeTextureDesc, &state.foamEnergyTexture);
            if (state.foamEnergyTexture && m_device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN && cascadeTextureDesc.keepInitialState) {
                // Establish the native image layout before exposing it to the renderer.
                nvrhi::CommandListHandle initializeCommands;
                m_device->createCommandList(nvrhi::CommandListParameters(), &initializeCommands);
                if (!initializeCommands) return WWE_FAIL;
                initializeCommands->open();
                initializeCommands->beginTrackingTextureState(state.foamEnergyTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);
                initializeCommands->setTextureState(state.foamEnergyTexture, nvrhi::AllSubresources, cascadeTextureDesc.initialState);
                initializeCommands->commitBarriers();
                initializeCommands->close();
                m_device->executeCommandList(initializeCommands);
            }

            for (uint32_t slot = 0; slot < m_numGpuSlots; ++slot) {
                nvrhi::ITexture* displacementTexture = nullptr;
                {
                    nvrhi::TextureHandle retained = getDisplacementMap(state.simulation);
                    displacementTexture = retained.Get();
                }
                state.gradientBindingSets[slot] = createComputeBindingSet(
                    m_device, m_gradientBindingLayout,
                    {nvrhi::BindingSetItem::Texture_UAV(
                         0, displacementTexture, nvrhi::Format::RGBA16_FLOAT,
                         nvrhi::TextureSubresourceSet(0, 1, 0, ~0u)),
                     nvrhi::BindingSetItem::Texture_UAV(
                         1, state.gradientTextures[slot], nvrhi::Format::RGBA16_FLOAT,
                         nvrhi::TextureSubresourceSet(0, 1, 0, ~0u)),
                     nvrhi::BindingSetItem::Texture_UAV(
                         2, state.momentTextures[slot], nvrhi::Format::RGBA16_FLOAT,
                         nvrhi::TextureSubresourceSet(0, 1, 0, ~0u)),
                     nvrhi::BindingSetItem::Texture_SRV(0, state.foamEnergyTexture,
                                                        nvrhi::Format::R16_FLOAT),
                     nvrhi::BindingSetItem::Sampler(0, m_wrapSampler),
                     nvrhi::BindingSetItem::ConstantBuffer(0, m_renderingConstantBuffer)});

                state.momentBindingSets[slot] = createComputeBindingSet(
                    m_device, m_foamInjectionBindingLayout,
                    {nvrhi::BindingSetItem::Texture_SRV(0, state.gradientTextures[slot],
                                                        nvrhi::Format::RGBA16_FLOAT),
                     nvrhi::BindingSetItem::Texture_UAV(
                         0, state.foamEnergyTexture, nvrhi::Format::R16_FLOAT,
                         nvrhi::TextureSubresourceSet(0, 1, 0, ~0u)),
                     nvrhi::BindingSetItem::Sampler(0, m_wrapSampler),
                     nvrhi::BindingSetItem::ConstantBuffer(0, m_renderingConstantBuffer)});
            }
            state.displacementMapVersion = kInvalidVersion;
        }

        m_textureArrayBindingLayout = createComputeBindingLayout(
            m_device, {bindingLayoutItem(0, textureSRV), bindingLayoutItem(1, textureSRV),
                       bindingLayoutItem(2, textureSRV), bindingLayoutItem(3, textureSRV),
                       bindingLayoutItem(4, textureSRV), bindingLayoutItem(5, textureSRV),
                       bindingLayoutItem(6, textureSRV), bindingLayoutItem(7, textureSRV),
                       bindingLayoutItem(8, textureSRV), bindingLayoutItem(9, textureSRV),
                       bindingLayoutItem(10, textureSRV), bindingLayoutItem(11, textureSRV),
                       bindingLayoutItem(0, textureUAV), bindingLayoutItem(1, textureUAV),
                       bindingLayoutItem(2, textureUAV), bindingLayoutItem(0, sampler),
                       bindingLayoutItem(0, volatileConstantBuffer)});
        m_textureArrayPipeline = createComputePipeline(m_device, m_textureArrayShader,
                                                       m_textureArrayBindingLayout);
        for (uint32_t slot = 0; slot < m_numGpuSlots; ++slot) {
            nvrhi::ITexture* displacementTextures[kCascadeCount];
            {
                nvrhi::TextureHandle retained[kCascadeCount];
                for (uint32_t cascadeIndex = 0; cascadeIndex < kCascadeCount;
                     ++cascadeIndex) {
                    retained[cascadeIndex] =
                        getDisplacementMap(m_cascadeStates[cascadeIndex].simulation);
                    displacementTextures[cascadeIndex] = retained[cascadeIndex].Get();
                }
            }
            m_textureArrayBindingSets[slot] = createComputeBindingSet(
                m_device, m_textureArrayBindingLayout,
                {nvrhi::BindingSetItem::Texture_SRV(
                     0, displacementTextures[0], nvrhi::Format::RGBA16_FLOAT, singleMip(0)),
                 nvrhi::BindingSetItem::Texture_SRV(
                     1, displacementTextures[1], nvrhi::Format::RGBA16_FLOAT, singleMip(0)),
                 nvrhi::BindingSetItem::Texture_SRV(
                     2, displacementTextures[2], nvrhi::Format::RGBA16_FLOAT, singleMip(0)),
                 nvrhi::BindingSetItem::Texture_SRV(
                     3, displacementTextures[3], nvrhi::Format::RGBA16_FLOAT, singleMip(0)),
                 nvrhi::BindingSetItem::Texture_SRV(
                     4, m_cascadeStates[0].gradientTextures[slot],
                     nvrhi::Format::RGBA16_FLOAT, singleMip(0)),
                 nvrhi::BindingSetItem::Texture_SRV(
                     5, m_cascadeStates[1].gradientTextures[slot],
                     nvrhi::Format::RGBA16_FLOAT, singleMip(0)),
                 nvrhi::BindingSetItem::Texture_SRV(
                     6, m_cascadeStates[2].gradientTextures[slot],
                     nvrhi::Format::RGBA16_FLOAT, singleMip(0)),
                 nvrhi::BindingSetItem::Texture_SRV(
                     7, m_cascadeStates[3].gradientTextures[slot],
                     nvrhi::Format::RGBA16_FLOAT, singleMip(0)),
                 nvrhi::BindingSetItem::Texture_SRV(
                     8, m_cascadeStates[0].momentTextures[slot],
                     nvrhi::Format::RGBA16_FLOAT, singleMip(0)),
                 nvrhi::BindingSetItem::Texture_SRV(
                     9, m_cascadeStates[1].momentTextures[slot],
                     nvrhi::Format::RGBA16_FLOAT, singleMip(0)),
                 nvrhi::BindingSetItem::Texture_SRV(
                     10, m_cascadeStates[2].momentTextures[slot],
                     nvrhi::Format::RGBA16_FLOAT, singleMip(0)),
                 nvrhi::BindingSetItem::Texture_SRV(
                     11, m_cascadeStates[3].momentTextures[slot],
                     nvrhi::Format::RGBA16_FLOAT, singleMip(0)),
                 nvrhi::BindingSetItem::Texture_UAV(0, m_displacementTextureArray,
                                                    nvrhi::Format::RGBA16_FLOAT,
                                                    arrayMip(0)),
                 nvrhi::BindingSetItem::Texture_UAV(
                     1, m_gradientTextureArray, nvrhi::Format::RGBA16_FLOAT, arrayMip(0)),
                 nvrhi::BindingSetItem::Texture_UAV(
                     2, m_momentTextureArray, nvrhi::Format::RGBA16_FLOAT, arrayMip(0)),
                 nvrhi::BindingSetItem::Sampler(0, m_wrapSampler),
                 nvrhi::BindingSetItem::ConstantBuffer(
                     0, m_textureArrayMipmapConstantBuffer)});
        }

        m_mipmapBindingLayout = createComputeBindingLayout(
            m_device,
            {bindingLayoutItem(0, textureSRV), bindingLayoutItem(0, textureUAV),
             bindingLayoutItem(1, textureUAV), bindingLayoutItem(2, textureUAV),
             bindingLayoutItem(3, textureUAV), bindingLayoutItem(4, textureUAV),
             bindingLayoutItem(0, sampler), bindingLayoutItem(0, volatileConstantBuffer)});
        m_mipmapPipeline =
            createComputePipeline(m_device, m_mipmapShader, m_mipmapBindingLayout);

        auto createMipmapBindingSet = [&](nvrhi::ITexture* texture, uint32_t sourceMip,
                                          uint32_t targetMip0, uint32_t targetMip1,
                                          uint32_t targetMip2, uint32_t targetMip3,
                                          uint32_t targetMip4) {
            return createComputeBindingSet(
                m_device, m_mipmapBindingLayout,
                {nvrhi::BindingSetItem::Texture_SRV(0, texture, nvrhi::Format::RGBA16_FLOAT,
                                                    arrayMip(sourceMip)),
                 nvrhi::BindingSetItem::Texture_UAV(0, texture, nvrhi::Format::RGBA16_FLOAT,
                                                    arrayMip(targetMip0)),
                 nvrhi::BindingSetItem::Texture_UAV(1, texture, nvrhi::Format::RGBA16_FLOAT,
                                                    arrayMip(targetMip1)),
                 nvrhi::BindingSetItem::Texture_UAV(2, texture, nvrhi::Format::RGBA16_FLOAT,
                                                    arrayMip(targetMip2)),
                 nvrhi::BindingSetItem::Texture_UAV(3, texture, nvrhi::Format::RGBA16_FLOAT,
                                                    arrayMip(targetMip3)),
                 nvrhi::BindingSetItem::Texture_UAV(4, texture, nvrhi::Format::RGBA16_FLOAT,
                                                    arrayMip(targetMip4)),
                 nvrhi::BindingSetItem::Sampler(0, m_wrapSampler),
                 nvrhi::BindingSetItem::ConstantBuffer(
                     0, m_textureArrayMipmapConstantBuffer)});
        };

        m_displacementMipmapBindingSet0 =
            createMipmapBindingSet(m_displacementTextureArray, 0, 1, 2, 3, 4, 5);
        m_displacementMipmapBindingSet1 = createMipmapBindingSet(
            m_displacementTextureArray, 5, clampedSecondBatchMip(mipExponent, 6, 0),
            clampedSecondBatchMip(mipExponent, 7, 1),
            clampedSecondBatchMip(mipExponent, 8, 2),
            clampedSecondBatchMip(mipExponent, 9, 3),
            clampedSecondBatchMip(mipExponent, 10, 4));
        m_gradientMipmapBindingSet0 =
            createMipmapBindingSet(m_gradientTextureArray, 0, 1, 2, 3, 4, 5);
        m_gradientMipmapBindingSet1 = createMipmapBindingSet(
            m_gradientTextureArray, 5, clampedSecondBatchMip(mipExponent, 6, 0),
            clampedSecondBatchMip(mipExponent, 7, 1),
            clampedSecondBatchMip(mipExponent, 8, 2),
            clampedSecondBatchMip(mipExponent, 9, 3),
            clampedSecondBatchMip(mipExponent, 10, 4));
        m_momentMipmapBindingSet0 =
            createMipmapBindingSet(m_momentTextureArray, 0, 1, 2, 3, 4, 5);
        m_momentMipmapBindingSet1 = createMipmapBindingSet(
            m_momentTextureArray, 5, clampedSecondBatchMip(mipExponent, 6, 0),
            clampedSecondBatchMip(mipExponent, 7, 1),
            clampedSecondBatchMip(mipExponent, 8, 2),
            clampedSecondBatchMip(mipExponent, 9, 3),
            clampedSecondBatchMip(mipExponent, 10, 4));

        for (uint32_t i = 0; i < array_dim(m_timerQueries); ++i) {
            m_device->createTimerQuery(&m_timerQueries[i]);
            m_timerResultsMs[i] = 0.0f;
        }
        m_timerReadIndex = 0;
        m_timerWriteIndex = 1;
    }

    const bool allocateGpuSampling =
        m_enableGpuDrivenDisplacementCalculation &&
        (m_simulationAPI == GFSDK_WaveWorks_Simulation_API_Compute ||
         (m_simulationAPI == GFSDK_WaveWorks_Simulation_API_CPU && m_enableGraphics));
    if (allocateGpuSampling) {
        if (m_device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN) {
            m_gpuSampleShader =
                createComputeShader(m_device, CalculateDisplacementReadbacksCS_bytecode_vk,
                                    CalculateDisplacementReadbacksCS_bytecode_vk_size);
        } else {
            m_gpuSampleShader =
                createComputeShader(m_device, CalculateDisplacementReadbacksCS_bytecode_dx,
                                    CalculateDisplacementReadbacksCS_bytecode_dx_size);
        }
        if (!m_wrapSampler)
            m_device->createSampler(samplerDesc, &m_wrapSampler);

        nvrhi::BufferDesc constantsDesc;
        constantsDesc.byteSize = 48;
        constantsDesc.debugName = "m_hCalculateReadbacksCB";
        constantsDesc.isConstantBuffer = true;
        constantsDesc.isVolatile = true;
        constantsDesc.maxVersions = 4096;
        m_device->createBuffer(constantsDesc, &m_calculateReadbacksCB);

        nvrhi::BufferDesc sampleDesc;
        sampleDesc.byteSize = kMaxGpuSampleCount * sizeof(gfsdk_float2);
        sampleDesc.structStride = sizeof(gfsdk_float2);
        sampleDesc.debugName = "m_hReadbackPositionsBuffer";
        sampleDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        sampleDesc.keepInitialState = true;
        sampleDesc.cpuAccess = nvrhi::CpuAccessMode::Write;
        m_device->createBuffer(sampleDesc, &m_readbackPositionsBuffer);

        sampleDesc.byteSize = kMaxGpuSampleCount * sizeof(gfsdk_float4);
        sampleDesc.structStride = sizeof(gfsdk_float4);
        sampleDesc.canHaveUAVs = true;
        sampleDesc.debugName = "m_hReadbackResultsBuffer";
        sampleDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
        sampleDesc.cpuAccess = nvrhi::CpuAccessMode::None;
        m_device->createBuffer(sampleDesc, &m_readbackResultsBuffer);

        sampleDesc.canHaveUAVs = false;
        sampleDesc.debugName = "m_hReadbackResultsBufferStaging";
        sampleDesc.cpuAccess = nvrhi::CpuAccessMode::Read;
        m_device->createBuffer(sampleDesc, &m_readbackResultsBufferStaging);

        const nvrhi::ResourceType structuredSRV = nvrhi::ResourceType::StructuredBuffer_SRV;
        const nvrhi::ResourceType structuredUAV = nvrhi::ResourceType::StructuredBuffer_UAV;

        m_gpuSampleBindingLayout = createComputeBindingLayout(
            m_device,
            {bindingLayoutItem(0, structuredSRV), bindingLayoutItem(1, textureSRV),
             bindingLayoutItem(2, textureSRV), bindingLayoutItem(3, textureSRV),
             bindingLayoutItem(4, textureSRV), bindingLayoutItem(0, structuredUAV),
             bindingLayoutItem(0, volatileConstantBuffer), bindingLayoutItem(0, sampler)});
        nvrhi::ITexture* displacementTextures[kCascadeCount];
        {
            nvrhi::TextureHandle retained[kCascadeCount];
            for (uint32_t cascadeIndex = 0; cascadeIndex < kCascadeCount; ++cascadeIndex) {
                retained[cascadeIndex] =
                    getDisplacementMap(m_cascadeStates[cascadeIndex].simulation);
                displacementTextures[cascadeIndex] = retained[cascadeIndex].Get();
            }
        }
        m_gpuSampleBindingSet = createComputeBindingSet(
            m_device, m_gpuSampleBindingLayout,
            {nvrhi::BindingSetItem::StructuredBuffer_SRV(0, m_readbackPositionsBuffer,
                                                         nvrhi::Format::RG32_FLOAT),
             nvrhi::BindingSetItem::Texture_SRV(1, displacementTextures[0],
                                                nvrhi::Format::RGBA16_FLOAT,
                                                nvrhi::TextureSubresourceSet(0, 1, 0, 0)),
             nvrhi::BindingSetItem::Texture_SRV(2, displacementTextures[1],
                                                nvrhi::Format::RGBA16_FLOAT,
                                                nvrhi::TextureSubresourceSet(0, 1, 0, 0)),
             nvrhi::BindingSetItem::Texture_SRV(3, displacementTextures[2],
                                                nvrhi::Format::RGBA16_FLOAT,
                                                nvrhi::TextureSubresourceSet(0, 1, 0, 0)),
             nvrhi::BindingSetItem::Texture_SRV(4, displacementTextures[3],
                                                nvrhi::Format::RGBA16_FLOAT,
                                                nvrhi::TextureSubresourceSet(0, 1, 0, 0)),
             nvrhi::BindingSetItem::StructuredBuffer_UAV(0, m_readbackResultsBuffer,
                                                         nvrhi::Format::RGBA32_FLOAT),
             nvrhi::BindingSetItem::ConstantBuffer(0, m_calculateReadbacksCB),
             nvrhi::BindingSetItem::Sampler(0, m_wrapSampler)});
        m_gpuSamplePipeline =
            createComputePipeline(m_device, m_gpuSampleShader, m_gpuSampleBindingLayout);
    }

    return WWE_OK;
}

WWRESULT Wind_Waves_Simulation::allocateSimulation(uint32_t cascadeIndex,
                                                   nvrhi::CommandListHandle commandList) {
    if (!m_manager || cascadeIndex >= kCascadeCount)
        return WWE_FAIL;

    CascadeState& state = m_cascadeStates[cascadeIndex];
    state.simulation = m_manager->createSimulation(m_cascades[cascadeIndex]);
    if (!state.simulation)
        return WWE_FAIL;

    if (auto* cpuSimulation =
            dynamic_cast<Wind_Waves_FFT_Simulation_CPU_Impl*>(state.simulation))
    {
        return cpuSimulation->init(m_device.Get(), commandList, m_enableGraphics);
    }

    if (auto* computeSimulation =
            dynamic_cast<Wind_Waves_FFT_Simulation_Compute_Impl*>(state.simulation))
    {
        return computeSimulation->init(m_device.Get(), commandList, m_enableGraphics);
    }

    return WWE_FAIL;
}

WWRESULT Wind_Waves_Simulation::allocateSimulationManager()
{
    if (m_manager)
        return WWE_OK;

    if (m_simulationAPI == GFSDK_WaveWorks_Simulation_API_CPU)
    {
        const Wind_Waves_Detailed_Simulation_Parameters params = makeDetailedParams();
        m_manager = new Wind_Waves_Simulation_Manager_CPU_Impl(params, m_threadPool);
    }
    else if (m_simulationAPI == GFSDK_WaveWorks_Simulation_API_Compute)
    {
        m_manager = new Wind_Waves_Simulation_Manager_Compute_Impl();
    }
    else
    {
        return WWE_FAIL;
    }

    return m_manager ? WWE_OK : WWE_FAIL;
}

WWRESULT Wind_Waves_Simulation::archiveDisplacements()
{
    if (!m_manager || !m_cascades[0].archiveSlots)
        return WWE_FAIL;
    return m_manager->archiveDisplacements();
}

WWRESULT Wind_Waves_Simulation::consumeAvailableTimerSlot(uint32_t& slot)
{
    if (!m_enableGraphics)
        return WWE_OK;

    if (m_timerReadIndex == m_timerWriteIndex)
    {
        const uint32_t next = nextTimerSlot(m_timerReadIndex);
        m_timerResultsMs[next] = m_device->getTimerQueryTime(m_timerQueries[next]) * 1000.0f;
        m_timerReadIndex = next;
    }

    slot = m_timerWriteIndex;
    m_timerWriteIndex = nextTimerSlot(m_timerWriteIndex);
    return WWE_OK;
}

void Wind_Waves_Simulation::consumeGPUSlot()
{
    if (m_numGpuSlots)
        m_gpuSlotIndex = (m_gpuSlotIndex + 1u) % m_numGpuSlots;
}

WWRESULT Wind_Waves_Simulation::getArchivedDisplacements(
    float coord,
    const gfsdk_float2* samplePoints,
    gfsdk_float4* displacements,
    uint32_t numSamples)
{
    std::vector<uint8_t*> previousData;
    std::vector<uint8_t*> nextData;
    std::vector<uint32_t> rowPitches;
    uint32_t previousIndex = 0;
    uint32_t nextIndex = 0;
    float interpolation = 0.0f;
    // written by every map call, the value of the last mapped archive is used
    bool readOnly = false;

    std::memset(displacements, 0, sizeof(gfsdk_float4) * numSamples);

    // pseudo: the archive coordinates are queried from cascade 0 only
    {
        Wind_Waves_FFT_Simulation* simulation = m_cascadeStates[0].simulation;
        WWRESULT coordsResult = WWE_FAIL;

        if (auto* cpuSimulation = dynamic_cast<Wind_Waves_FFT_Simulation_CPU_Impl*>(simulation))
        {
            int previous = 0;
            int next = 0;
            coordsResult = cpuSimulation->getDisplacementArchiveCoords(
                coord, &previous, &next, &interpolation);
            previousIndex = static_cast<uint32_t>(previous);
            nextIndex = static_cast<uint32_t>(next);
        }
        else if (auto* computeSimulation =
                     dynamic_cast<Wind_Waves_FFT_Simulation_Compute_Impl*>(simulation))
        {
            coordsResult = computeSimulation->getDisplacementArchiveCoords(
                coord, &previousIndex, &nextIndex, &interpolation);
        }

        if (coordsResult != WWE_OK)
            return WWE_FAIL;
    }

    auto mapArchive = [this, &readOnly](
                          uint32_t cascadeIndex,
                          uint32_t archiveIndex,
                          uint8_t** data,
                          uint32_t* rowPitch) -> WWRESULT {
        Wind_Waves_FFT_Simulation* simulation = m_cascadeStates[cascadeIndex].simulation;

        if (auto* cpuSimulation = dynamic_cast<Wind_Waves_FFT_Simulation_CPU_Impl*>(simulation))
            return cpuSimulation->mapDisplacementArchiveData(data, rowPitch, archiveIndex, &readOnly);
        if (auto* computeSimulation =
                dynamic_cast<Wind_Waves_FFT_Simulation_Compute_Impl*>(simulation))
        {
            return computeSimulation->mapDisplacementArchiveData(
                data, rowPitch, archiveIndex, &readOnly);
        }
        return WWE_FAIL;
    };

    // pseudo: a single archive entry is used when both coordinates agree, otherwise the
    // previous and the next entries are blended; a failing map leaves earlier maps in place
    const bool singleArchive = previousIndex == nextIndex;
    for (uint32_t cascadeIndex = 0; cascadeIndex < kCascadeCount; ++cascadeIndex)
    {
        uint8_t* data = nullptr;
        uint32_t rowPitch = 0;
        if (mapArchive(cascadeIndex, previousIndex, &data, &rowPitch) != WWE_OK)
            return WWE_FAIL;
        previousData.push_back(data);
        rowPitches.push_back(rowPitch);

        if (!singleArchive)
        {
            // pseudo: the row pitch of the second map is not stored
            uint8_t* nextArchiveData = nullptr;
            uint32_t nextRowPitch = 0;
            if (mapArchive(cascadeIndex, nextIndex, &nextArchiveData, &nextRowPitch) != WWE_OK)
                return WWE_FAIL;
            nextData.push_back(nextArchiveData);
        }
    }

    // pseudo: every task (and the serial path) accumulates all four cascades for its sample range
    auto addSampleRange = [&](uint32_t start, uint32_t count) {
        for (uint32_t cascadeIndex = 0; cascadeIndex < kCascadeCount; ++cascadeIndex)
        {
            if (singleArchive)
            {
                Simulation_Util::addDisplacements(
                    m_cascades[cascadeIndex], previousData[cascadeIndex],
                    static_cast<int>(rowPitches[cascadeIndex]), samplePoints, displacements,
                    start, count, 1.0f, readOnly);
            }
            else
            {
                Simulation_Util::addDisplacements(
                    m_cascades[cascadeIndex], previousData[cascadeIndex],
                    static_cast<int>(rowPitches[cascadeIndex]), samplePoints, displacements,
                    start, count, 1.0f - interpolation, readOnly);
                Simulation_Util::addDisplacements(
                    m_cascades[cascadeIndex], nextData[cascadeIndex],
                    static_cast<int>(rowPitches[cascadeIndex]), samplePoints, displacements,
                    start, count, interpolation, readOnly);
            }
        }
    };

    if (numSamples >= 0x200u)
    {
        const uint32_t chunkSize = std::max(128u, numSamples / m_threadPool->getThreadCount());
        std::vector<std::function<void()>> tasks;
        uint32_t start = 0;

        if (chunkSize < numSamples)
        {
            do
            {
                tasks.emplace_back(
                    [&addSampleRange, start, chunkSize]() { addSampleRange(start, chunkSize); });
                start += chunkSize;
            } while (chunkSize + start < numSamples);
        }
        if (start < numSamples)
        {
            tasks.emplace_back([&addSampleRange, start, numSamples]() {
                addSampleRange(start, numSamples - start);
            });
        }

        m_threadPool->prepareHighPrioritySubmission(static_cast<unsigned int>(tasks.size()));
        m_threadPool->submitHighPriorityVector(std::move(tasks));
        m_threadPool->waitForHighPriorityWorkDone();
    }
    else
    {
        addSampleRange(0, numSamples);
    }

    // pseudo: only the previous archive entry is unmapped, for all four cascades
    for (uint32_t cascadeIndex = 0; cascadeIndex < kCascadeCount; ++cascadeIndex)
    {
        Wind_Waves_FFT_Simulation* simulation = m_cascadeStates[cascadeIndex].simulation;

        if (auto* cpuSimulation = dynamic_cast<Wind_Waves_FFT_Simulation_CPU_Impl*>(simulation))
            cpuSimulation->unmapDisplacementArchiveData(previousIndex);
        else if (auto* computeSimulation =
                     dynamic_cast<Wind_Waves_FFT_Simulation_Compute_Impl*>(simulation))
        {
            computeSimulation->unmapDisplacementArchiveData(previousIndex);
        }
    }

    return WWE_OK;
}

float Wind_Waves_Simulation::getConservativeMaxDisplacementEstimate() const
{
    const auto& cascade = m_cascades[0];
    const float baseWindSpeed = clampPositive(cascade.baseWindSpeed);
    const float baseWindDistance = clampPositive(cascade.baseWindDistance);
    const float baseEnergy =
        0.25f * 0.076f * std::pow(baseWindSpeed * baseWindSpeed /
                                      (baseWindDistance * 9.8100004f),
                                  0.22f) *
        9.8100004f * 9.8100004f;
    const float baseOmega = std::pow(96.236107f / (baseWindDistance * baseWindSpeed), 0.33f);
    const float baseDenominator =
        1.0f - std::log(clampPositive(cascade.baseSpectrumPeaking)) * 0.287f;
    const float baseEstimate =
        baseEnergy / std::pow(baseOmega * 22.0f, 4.0f) / baseDenominator;

    const float swellWindSpeed = clampPositive(cascade.swellWindSpeed);
    const float swellWindDistance = clampPositive(cascade.swellWindDistance);
    const float swellEnergy =
        0.25f * 0.076f * std::pow(swellWindSpeed * swellWindSpeed /
                                      (swellWindDistance * 9.8100004f),
                                  0.22f) *
        9.8100004f * 9.8100004f;
    const float swellOmega =
        std::pow(96.236107f / (swellWindDistance * swellWindSpeed), 0.33f);
    const float swellDenominator =
        1.0f - std::log(clampPositive(cascade.swellSpectrumPeaking)) * 0.287f;
    const float swellEstimate =
        swellEnergy / std::pow(swellOmega * 22.0f, 4.0f) / swellDenominator;

    return (std::sqrt(std::max(0.0f, baseEstimate)) * 4.0f +
            std::sqrt(std::max(0.0f, swellEstimate)) * 4.0f) *
           3.0f;
}

WWRESULT Wind_Waves_Simulation::getDisplacements(
    const gfsdk_float2* samplePoints,
    gfsdk_float4* displacements,
    uint32_t numSamples,
    bool calculateOnGPU)
{
    if (!samplePoints || !displacements)
        return WWE_FAIL;
    if (calculateOnGPU)
    {
        if (!m_enableGpuDrivenDisplacementCalculation || !m_device || !m_commandList ||
            numSamples > kMaxGpuSampleCount ||
            (m_simulationAPI == GFSDK_WaveWorks_Simulation_API_CPU && !m_enableGraphics))
        {
            return WWE_FAIL;
        }
        if (!m_readbackPositionsBuffer || !m_readbackResultsBuffer ||
            !m_readbackResultsBufferStaging || !m_calculateReadbacksCB ||
            !m_gpuSamplePipeline || !m_gpuSampleBindingSet)
        {
            return WWE_FAIL;
        }

        nvrhi::TextureHandle displacementTextures[kCascadeCount];
        for (uint32_t cascadeIndex = 0; cascadeIndex < kCascadeCount; ++cascadeIndex)
        {
            displacementTextures[cascadeIndex] =
                getDisplacementMap(m_cascadeStates[cascadeIndex].simulation);
            if (!displacementTextures[cascadeIndex])
                return WWE_FAIL;
        }

        void* inputData =
            m_device->mapBuffer(m_readbackPositionsBuffer, nvrhi::CpuAccessMode::Write);
        if (!inputData)
            return WWE_FAIL;
        std::memcpy(inputData, samplePoints, sizeof(gfsdk_float2) * numSamples);
        m_device->unmapBuffer(m_readbackPositionsBuffer);

        m_commandList->open();
        for (nvrhi::ITexture* displacementTexture : displacementTextures)
        {
            m_commandList->beginTrackingTextureState(
                displacementTexture,
                nvrhi::AllSubresources,
                nvrhi::ResourceStates::ShaderResource);
        }

        struct GpuSampleConstants
        {
            uint32_t sampleCount; // offset 0
            float uvWarpingAmplitude; // offset 4
            float uvWarpingFrequency; // offset 8
            float pad0; // offset 12
            float uvScale[kCascadeCount]; // offset 16
            float uvShift[kCascadeCount]; // offset 32
        } constants = {
            numSamples,
            m_cascades[0].warpAmplitude,
            m_cascades[0].wrapFrequency,
            0.0f,
            {1.0f / m_cascades[0].physicalSize,
             1.0f / m_cascades[1].physicalSize,
             1.0f / m_cascades[2].physicalSize,
             1.0f / m_cascades[3].physicalSize},
            {0.5f / static_cast<float>(m_cascades[0].textureSize),
             0.5f / static_cast<float>(m_cascades[1].textureSize),
             0.5f / static_cast<float>(m_cascades[2].textureSize),
             0.5f / static_cast<float>(m_cascades[3].textureSize)},
        };
        m_commandList->writeBuffer(
            m_calculateReadbacksCB,
            &constants,
            sizeof(constants));
        dispatchCompute(
            m_commandList,
            m_gpuSamplePipeline,
            m_gpuSampleBindingSet,
            (numSamples >> 7u) + 1u,
            1u,
            1u);
        m_commandList->copyBuffer(
            m_readbackResultsBufferStaging,
            0,
            m_readbackResultsBuffer,
            0,
            sizeof(gfsdk_float4) * numSamples);

        for (nvrhi::ITexture* displacementTexture : displacementTextures)
        {
            m_commandList->setTextureState(displacementTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
            m_commandList->commitBarriers();
        }
        m_commandList->close();
        m_device->executeCommandList(m_commandList);

        void* outputData =
            m_device->mapBuffer(m_readbackResultsBufferStaging, nvrhi::CpuAccessMode::Read);
        if (!outputData)
            return WWE_FAIL;
        std::memcpy(displacements, outputData, sizeof(gfsdk_float4) * numSamples);
        m_device->unmapBuffer(m_readbackResultsBufferStaging);
        return WWE_OK;
    }

    std::memset(displacements, 0, sizeof(gfsdk_float4) * numSamples);

    std::vector<uint8_t*> displacementData;
    std::vector<uint32_t> rowPitches;
    bool readOnly = false;
    for (uint32_t cascadeIndex = 0; cascadeIndex < kCascadeCount; ++cascadeIndex)
    {
        Wind_Waves_FFT_Simulation* simulation = m_cascadeStates[cascadeIndex].simulation;
        if (!simulation)
            return WWE_FAIL;

        uint8_t* mappedData = nullptr;
        uint32_t rowPitch = 0;
        WWRESULT mapResult = WWE_FAIL;

        if (auto* cpuSimulation = dynamic_cast<Wind_Waves_FFT_Simulation_CPU_Impl*>(simulation))
        {
            mapResult =
                cpuSimulation->mapDisplacementData(&mappedData, &rowPitch, &readOnly);
        }
        else if (auto* computeSimulation =
                     dynamic_cast<Wind_Waves_FFT_Simulation_Compute_Impl*>(simulation))
        {
            mapResult =
                computeSimulation->mapDisplacementData(&mappedData, &rowPitch, &readOnly);
        }

        if (mapResult != WWE_OK)
            return WWE_FAIL;

        displacementData.push_back(mappedData);
        rowPitches.push_back(rowPitch);
    }

    addDisplacementsWithTasks(
        m_threadPool, m_cascades, displacementData, rowPitches,
        samplePoints, displacements, numSamples, readOnly);

    for (uint32_t cascadeIndex = 0; cascadeIndex < kCascadeCount; ++cascadeIndex)
    {
        Wind_Waves_FFT_Simulation* simulation = m_cascadeStates[cascadeIndex].simulation;
        if (auto* cpuSimulation = dynamic_cast<Wind_Waves_FFT_Simulation_CPU_Impl*>(simulation))
            cpuSimulation->unmapDisplacementData();
        else if (auto* computeSimulation =
                     dynamic_cast<Wind_Waves_FFT_Simulation_Compute_Impl*>(simulation))
            computeSimulation->unmapDisplacementData();
    }

    return WWE_OK;
}

bool Wind_Waves_Simulation::getReadbackCursor(uint64_t* cursor) const
{
    return m_manager && m_manager->getReadbackCursor(cursor);
}

WWRESULT Wind_Waves_Simulation::getRenderingResources(
    GFSDK_WaveWorks_Wind_Waves_Rendering_Data& data) const
{
    if (!m_enableGraphics)
        return WWE_FAIL;

    data.cascade0_UV_scale = 1.0f / m_cascades[0].physicalSize;
    data.cascade1_UV_scale = 1.0f / m_cascades[1].physicalSize;
    data.cascade2_UV_scale = 1.0f / m_cascades[2].physicalSize;
    data.cascade3_UV_scale = 1.0f / m_cascades[3].physicalSize;
    data.cascade0_UV_offset = 0.5f / static_cast<float>(m_cascades[0].textureSize);
    data.cascade1_UV_offset = 0.5f / static_cast<float>(m_cascades[1].textureSize);
    data.cascade2_UV_offset = 0.5f / static_cast<float>(m_cascades[2].textureSize);
    data.cascade3_UV_offset = 0.5f / static_cast<float>(m_cascades[3].textureSize);
    data.uv_warping_amplitude = m_cascades[0].warpAmplitude;
    data.uv_warping_frequency = m_cascades[0].wrapFrequency;
    data.cascade_to_cascade_scale = 5.23f;
    data.foam_whitecaps_threshold = m_foamWhitecapsThreshold;
    data.size_of_texture_arrays = m_cascades[3].textureSize;

    auto api = m_device->getGraphicsAPI();
    if(api == nvrhi::GraphicsAPI::D3D11) {
        data.displacements_texture_array =
            m_displacementTextureArray->getNativeObject(nvrhi::ObjectTypes::D3D11_Resource);
        data.gradients_texture_array =
            m_gradientTextureArray->getNativeObject(nvrhi::ObjectTypes::D3D11_Resource);
        data.moments_texture_array =
            m_momentTextureArray->getNativeObject(nvrhi::ObjectTypes::D3D11_Resource);
    } else if (api == nvrhi::GraphicsAPI::D3D12) {
        data.displacements_texture_array =
            m_displacementTextureArray->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource);
        data.gradients_texture_array =
            m_gradientTextureArray->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource);
        data.moments_texture_array =
            m_momentTextureArray->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource);
    } else if (api == nvrhi::GraphicsAPI::VULKAN) {
        data.displacements_texture_array =
            m_displacementTextureArray->getNativeObject(nvrhi::ObjectTypes::VK_Image);
        data.gradients_texture_array =
            m_gradientTextureArray->getNativeObject(nvrhi::ObjectTypes::VK_Image);
        data.moments_texture_array =
            m_momentTextureArray->getNativeObject(nvrhi::ObjectTypes::VK_Image);
    }
    else
    {
        return WWE_FAIL;
    }

    return data.displacements_texture_array && data.gradients_texture_array &&
            data.moments_texture_array ?
        WWE_OK :
        WWE_FAIL;
}

bool Wind_Waves_Simulation::getStagingCursor(uint64_t* cursor) const
{
    return m_manager && m_manager->getStagingCursor(cursor);
}

WWRESULT Wind_Waves_Simulation::getStats(
    GFSDK_WaveWorks_Wind_Waves_Simulation_Stats& stats) const
{
    if (!m_manager)
        return WWE_FAIL;

    zeroStats(stats);
    const WWRESULT result = m_manager->getTimings(stats);
    if (result < 0)
        return result;

    stats.GPU_update_time = m_timerResultsMs[m_timerReadIndex];
    stats.GPU_total_time = stats.GPU_simulation_time + stats.GPU_update_time;
    return WWE_OK;
}

WWRESULT Wind_Waves_Simulation::init11(
    void* deviceContext,
    const Wind_Waves_Detailed_Simulation_Parameters& params,
    bool enableGraphics)
{
    if (!isValidSimulationAPI(params.simulationAPI))
        return WWE_FAIL;
    if ((enableGraphics || params.simulationAPI == GFSDK_WaveWorks_Simulation_API_Compute) &&
        !deviceContext)
    {
        return WWE_FAIL;
    }

    releaseAll();
    m_enableGraphics = enableGraphics;
    setDetailedParams(params);
    allocateThreadPool(params.cpuThreadingModel);

#if defined(_WIN32)
    if (deviceContext)
    {
        nvrhi::d3d11::DeviceDesc deviceDesc;
        deviceDesc.messageCallback = m_messageCallback;
        deviceDesc.context = reinterpret_cast<ID3D11DeviceContext*>(deviceContext);
        nvrhi::createGraphicsFactory(nvrhi::GraphicsFactoryDesc())->createD3D11Device(deviceDesc, &m_device);
        if (!m_device) return WWE_FAIL;

        nvrhi::CommandListParameters commandListParams;
        commandListParams.enableImmediateExecution = true;
        commandListParams.uploadChunkSize = 0x10000;
        commandListParams.scratchChunkSize = 0x10000;
        commandListParams.scratchMaxMemory = 0x40000000;
        commandListParams.queueType = nvrhi::CommandQueue::Graphics;
        m_device->createCommandList(commandListParams, &m_commandList);
        if (!m_commandList)
            return WWE_FAIL;
    }
#else
    if (deviceContext)
        return WWE_FAIL;
#endif

    return allocateAll();
}

WWRESULT Wind_Waves_Simulation::init12(
    void* device,
    void* queue,
    const Wind_Waves_Detailed_Simulation_Parameters& params,
    bool enableGraphics)
{
    if ((enableGraphics && (!device || !queue)) ||
        (params.simulationAPI == GFSDK_WaveWorks_Simulation_API_Compute && !device))
    {
        return WWE_FAIL;
    }

    m_enableGraphics = enableGraphics;
    setDetailedParams(params);

#if defined(_WIN32)
    nvrhi::d3d12::DeviceDesc deviceDesc;
    deviceDesc.errorCB = m_messageCallback;
    deviceDesc.pDevice = reinterpret_cast<ID3D12Device*>(device);
    deviceDesc.pGraphicsCommandQueue = reinterpret_cast<ID3D12CommandQueue*>(queue);
    nvrhi::d3d12::DeviceHandle d3d12Device;
    nvrhi::createGraphicsFactory(nvrhi::GraphicsFactoryDesc())->createD3D12Device(deviceDesc, &d3d12Device);
    m_device = d3d12Device.Get();
    if (!m_device) return WWE_FAIL;

    nvrhi::CommandListParameters commandListParams;
    commandListParams.enableImmediateExecution = true;
    commandListParams.uploadChunkSize = 0x10000;
    commandListParams.scratchChunkSize = 0x10000;
    commandListParams.scratchMaxMemory = 0x40000000;
    commandListParams.queueType = nvrhi::CommandQueue::Graphics;
    m_device->createCommandList(commandListParams, &m_commandList);
    if (!m_commandList)
        return WWE_FAIL;
#else
    return WWE_FAIL;
#endif

    allocateThreadPool(params.cpuThreadingModel);
    const WWRESULT result = allocateAll();
    return result >= 0 ? WWE_OK : result;
}

WWRESULT Wind_Waves_Simulation::initVK(
    void* instance,
    void* physicalDevice,
    void* device,
    void* graphicsQueue,
    int graphicsQueueIndex,
    void* transferQueue,
    int transferQueueIndex,
    void* computeQueue,
    int computeQueueIndex,
    const char** instanceExtensions,
    int numInstanceExtensions,
    const char** layers,
    int numLayers,
    const char** deviceExtensions,
    int numDeviceExtensions,
    const Wind_Waves_Detailed_Simulation_Parameters& params,
    bool enableGraphics)
{
    if ((enableGraphics || params.simulationAPI == GFSDK_WaveWorks_Simulation_API_Compute) &&
        (!instance || !physicalDevice || !device || !computeQueue))
    {
        return WWE_FAIL;
    }

    m_enableGraphics = enableGraphics;
    setDetailedParams(params);

    nvrhi::vulkan::DeviceDesc deviceDesc;
    deviceDesc.errorCB = m_messageCallback;
    deviceDesc.instance = instance ? *static_cast<VkInstance*>(instance) : VK_NULL_HANDLE;
    deviceDesc.physicalDevice = physicalDevice ? *static_cast<VkPhysicalDevice*>(physicalDevice) : VK_NULL_HANDLE;
    deviceDesc.device = device ? *static_cast<VkDevice*>(device) : VK_NULL_HANDLE;
    deviceDesc.graphicsQueue = graphicsQueue ? *static_cast<VkQueue*>(graphicsQueue) : VK_NULL_HANDLE;
    deviceDesc.graphicsQueueIndex = graphicsQueueIndex;
    deviceDesc.transferQueue = transferQueue ? *static_cast<VkQueue*>(transferQueue) : VK_NULL_HANDLE;
    deviceDesc.transferQueueIndex = transferQueueIndex;
    deviceDesc.computeQueue = computeQueue ? *static_cast<VkQueue*>(computeQueue) : VK_NULL_HANDLE;
    deviceDesc.computeQueueIndex = computeQueueIndex;
    deviceDesc.instanceExtensions = instanceExtensions;
    deviceDesc.numInstanceExtensions = numInstanceExtensions;
    deviceDesc.deviceExtensions = deviceExtensions;
    deviceDesc.numDeviceExtensions = numDeviceExtensions;
    if (!InitializeWaveWorksVulkanDispatch(deviceDesc.instance, deviceDesc.device))
        return WWE_FAIL;
    nvrhi::vulkan::DeviceHandle vulkanDevice;
    nvrhi::createGraphicsFactory(nvrhi::GraphicsFactoryDesc())->createVulkanDevice(deviceDesc, &vulkanDevice);
    m_device = vulkanDevice.Get();
    if (!m_device) return WWE_FAIL;

    nvrhi::CommandListParameters commandListParams;
    commandListParams.enableImmediateExecution = true;
    commandListParams.uploadChunkSize = 0x10000;
    commandListParams.scratchChunkSize = 0x10000;
    commandListParams.scratchMaxMemory = 0x40000000;
    commandListParams.queueType = nvrhi::CommandQueue::Graphics;
    m_device->createCommandList(commandListParams, &m_commandList);
    if (!m_commandList)
        return WWE_FAIL;

    allocateThreadPool(params.cpuThreadingModel);
    const WWRESULT result = allocateAll();
    return result >= 0 ? WWE_OK : result;
}

WWRESULT Wind_Waves_Simulation::kick(uint64_t* kickId)
{
    // pseudo: device vtable +352 is IDevice::runGarbageCollection (waitForIdle is +344)
    m_device->runGarbageCollection();

    consumeGPUSlot();

    m_commandList->open();

    uint64_t localKickId = 0;
    const WWRESULT result = m_manager->kick(m_simulationTime, localKickId);
    if (result < 0)
        return result;

    if (!updateGradientMaps())
        return WWE_FAIL;

    if (kickId)
        *kickId = localKickId;

    m_commandList->close();
    m_device->executeCommandList(m_commandList);

    return WWE_OK;
}

WWRESULT Wind_Waves_Simulation::reinit(
    const Wind_Waves_Detailed_Simulation_Parameters& params)
{
    if (!isValidSimulationAPI(params.simulationAPI))
        return WWE_FAIL;
    if (params.simulationAPI == GFSDK_WaveWorks_Simulation_API_Compute && !m_device)
        return WWE_FAIL;

    const bool recreateThreadPool = params.cpuThreadingModel != m_cpuThreadingModel;
    const bool simulationApiChanged = params.simulationAPI != m_simulationAPI;
    const bool recreateManager = recreateThreadPool || simulationApiChanged;
    const bool recreateRendering = simulationApiChanged || shouldRecreateRendering(params);

    if (recreateThreadPool)
    {
        // pseudo: waits for the low-priority work group to drain before the pool is deleted
        m_threadPool->waitForLowPriorityWorkDone();
        allocateThreadPool(params.cpuThreadingModel);
    }

    setDetailedParams(params);

    // pseudo: beforeReinit runs after the thread pool swap and the parameter copy
    WWRESULT beforeResult = m_manager->beforeReinit(params, true);
    if (beforeResult < 0)
        return beforeResult;

    if (recreateManager)
    {
        for (uint32_t i = 0; i < kCascadeCount; ++i)
            releaseSimulation(i);
        releaseSimulationManager();

        WWRESULT managerResult = allocateSimulationManager();
        if (managerResult < 0)
            return managerResult;

        for (uint32_t i = 0; i < kCascadeCount; ++i)
        {
            WWRESULT simulationResult = allocateSimulation(i, m_commandList);
            if (simulationResult < 0)
                return simulationResult;
        }
    }
    else
    {
        for (uint32_t i = 0; i < kCascadeCount; ++i)
        {
            Wind_Waves_FFT_Simulation* simulation = m_cascadeStates[i].simulation;
            if (!simulation)
                return WWE_FAIL;

            WWRESULT reinitResult = WWE_FAIL;
            if (auto* cpuSimulation =
                    dynamic_cast<Wind_Waves_FFT_Simulation_CPU_Impl*>(simulation))
            {
                reinitResult = cpuSimulation->reinit(m_cascades[i]);
            }
            else if (auto* computeSimulation =
                         dynamic_cast<Wind_Waves_FFT_Simulation_Compute_Impl*>(simulation))
            {
                reinitResult = computeSimulation->reinit(m_cascades[i]);
            }

            if (reinitResult < 0)
            {
                handle_ww_error(
                    L"C:\\Work\\dev\\waveworks_example.package\\waveworks\\src\\Wind_Waves_Simulation.cpp",
                    0x16Au);
                return reinitResult;
            }
        }
    }

    if (recreateRendering)
    {
        waitForAllInFlightTimers();
        releaseRenderingResources();
        // pseudo: the result of allocateRenderingResources is ignored
        allocateRenderingResources();
    }

    return WWE_OK;
}

void Wind_Waves_Simulation::releaseAll()
{
    for (uint32_t i = 0; i < kCascadeCount; ++i)
        releaseSimulation(i);
    releaseRenderingResources();
    releaseSimulationManager();

    if (m_threadPool)
    {
        delete m_threadPool;
        m_threadPool = nullptr;
    }
}

void Wind_Waves_Simulation::releaseRenderingResources()
{
    m_displacementTextureArray = nullptr;
    m_gradientTextureArray = nullptr;
    m_momentTextureArray = nullptr;
    m_gradientFoamShader = nullptr;
    m_renderingConstantBuffer = nullptr;
    m_gradientPipeline = nullptr;
    m_gradientBindingLayout = nullptr;
    m_foamInjectionShader = nullptr;
    m_foamInjectionPipeline = nullptr;
    m_foamInjectionBindingLayout = nullptr;
    m_wrapSampler = nullptr;
    m_gpuSampleShader = nullptr;
    m_readbackPositionsBuffer = nullptr;
    m_readbackResultsBuffer = nullptr;
    m_readbackResultsBufferStaging = nullptr;
    m_calculateReadbacksCB = nullptr;
    m_gpuSamplePipeline = nullptr;
    m_gpuSampleBindingLayout = nullptr;
    m_gpuSampleBindingSet = nullptr;
    m_textureArrayShader = nullptr;
    m_textureArrayMipmapConstantBuffer = nullptr;
    m_textureArrayPipeline = nullptr;
    for (nvrhi::BindingSetHandle& set : m_textureArrayBindingSets)
        set = nullptr;
    m_textureArrayBindingLayout = nullptr;
    m_mipmapShader = nullptr;
    m_mipmapPipeline = nullptr;
    m_displacementMipmapBindingSet0 = nullptr;
    m_displacementMipmapBindingSet1 = nullptr;
    m_gradientMipmapBindingSet0 = nullptr;
    m_gradientMipmapBindingSet1 = nullptr;
    m_momentMipmapBindingSet0 = nullptr;
    m_momentMipmapBindingSet1 = nullptr;
    m_mipmapBindingLayout = nullptr;

    for (nvrhi::TimerQueryHandle& timerQuery : m_timerQueries)
        timerQuery = nullptr;

    for (CascadeState& state : m_cascadeStates)
    {
        for (nvrhi::TextureHandle& texture : state.gradientTextures)
            texture = nullptr;
        for (nvrhi::TextureHandle& texture : state.momentTextures)
            texture = nullptr;
        state.foamEnergyTexture = nullptr;
        for (nvrhi::BindingSetHandle& set : state.gradientBindingSets)
            set = nullptr;
        for (nvrhi::BindingSetHandle& set : state.momentBindingSets)
            set = nullptr;
    }
}

void Wind_Waves_Simulation::releaseSimulation(uint32_t cascadeIndex)
{
    if (cascadeIndex >= kCascadeCount)
        return;

    Wind_Waves_FFT_Simulation*& simulation = m_cascadeStates[cascadeIndex].simulation;
    if (simulation && m_manager)
        m_manager->releaseSimulation(simulation);
    else
        delete simulation;
    simulation = nullptr;
}

void Wind_Waves_Simulation::releaseSimulationManager()
{
    delete m_manager;
    m_manager = nullptr;
}

void Wind_Waves_Simulation::setSimulationTime(double appTime)
{
    m_simulationTime = static_cast<double>(m_timeScale) * appTime;

    if (m_simulationTimeHistoryCount != 0)
    {
        for (uint32_t i = m_simulationTimeHistoryCount - 1u; i > 0; --i)
            m_simulationTimeHistory[i] = m_simulationTimeHistory[i - 1u];
        m_simulationTimeHistory[0] = m_simulationTime;
    }
    else
    {
        m_simulationTimeHistoryCount = m_numGpuSlots + 1u;
        for (uint32_t i = 0; i < m_simulationTimeHistoryCount; ++i)
            m_simulationTimeHistory[i] = m_simulationTime;
    }

    m_simulationDeltaTime =
        m_simulationTimeHistory[0] - m_simulationTimeHistory[m_numGpuSlots];
    if (m_simulationDeltaTime <= 0.0)
        m_simulationDeltaTime = 0.0;
}

bool Wind_Waves_Simulation::updateGradientMaps()
{
    if (!m_enableGraphics)
        return true;

    const uint32_t textureSize = m_cascades[3].textureSize;

    nvrhi::TextureHandle displacementTextures[kCascadeCount];
    uint64_t displacementVersions[kCascadeCount] = {};
    for (uint32_t cascadeIndex = 0; cascadeIndex < kCascadeCount; ++cascadeIndex)
    {
        CascadeState& state = m_cascadeStates[cascadeIndex];
        displacementTextures[cascadeIndex] = getDisplacementMap(state.simulation);
        displacementVersions[cascadeIndex] = getDisplacementMapVersion(state.simulation);
    }

    uint32_t timerSlot = 0;
    consumeAvailableTimerSlot(timerSlot);
    if (m_enableGpuTimers && m_timerQueries[timerSlot] && m_device)
    {
        m_device->resetTimerQuery(m_timerQueries[timerSlot]);
        m_commandList->beginTimerQuery(m_timerQueries[timerSlot]);
    }

    m_commandList->beginMarker("WaveWorks: Postprocessing FFT results");
    m_commandList->beginMarker("WaveWorks: Gradients / moments / foam generation");

    for (uint32_t cascadeIndex = 0; cascadeIndex < kCascadeCount; ++cascadeIndex)
    {
        CascadeState& state = m_cascadeStates[cascadeIndex];
        if (state.displacementMapVersion == displacementVersions[cascadeIndex])
            continue;

        if (state.gpuSlotDirty[m_gpuSlotIndex])
        {
            m_commandList->clearTextureFloat(
                state.gradientTextures[m_gpuSlotIndex],
                singleMip(0),
                nvrhi::Color(0.0f, 0.0f, 1.0f, 0.0f));
            m_commandList->clearTextureFloat(
                state.foamEnergyTexture,
                singleMip(0),
                nvrhi::Color(0.0f));
            state.gpuSlotDirty[m_gpuSlotIndex] = false;
        }

        const Wind_Waves_Detailed_Simulation_Parameters::Cascade& cascade =
            m_cascades[cascadeIndex];
        const float deltaTime = static_cast<float>(m_simulationDeltaTime);
        float foamSpatialDissipation =
            deltaTime * cascade.foamDissipationSpeed * 1000.0f / cascade.physicalSize;
        if (foamSpatialDissipation >= 0.5f)
            foamSpatialDissipation = 0.5f;

        struct GradientConstants
        {
            uint32_t simulationGridSize; // offset 0
            float texelsPerMeter; // offset 4
            float foamTemporalDissipation; // offset 8
            float foamSpatialDissipation; // offset 12
            float foamGenerationThreshold; // offset 16
            float foamGenerationAmount; // offset 20
            float pad0; // offset 24
            float pad1; // offset 28
        } gradientConstants = {
            cascade.textureSize,
            static_cast<float>(cascade.textureSize) / cascade.physicalSize,
            std::pow(cascade.foamFalloffSpeed, deltaTime * 50.0f),
            foamSpatialDissipation / static_cast<float>(cascade.textureSize),
            cascade.foamGenerationThreshold,
            deltaTime * cascade.foamGenerationAmount * 50.0f,
            0.0f,
            0.0f,
        };
        m_commandList->writeBuffer(
            m_renderingConstantBuffer,
            &gradientConstants,
            sizeof(gradientConstants));

        const uint32_t groups = cascade.textureSize >> 4u;
        dispatchCompute(
            m_commandList,
            m_gradientPipeline,
            state.gradientBindingSets[m_gpuSlotIndex],
            groups,
            groups,
            1u);
        dispatchCompute(
            m_commandList,
            m_foamInjectionPipeline,
            state.momentBindingSets[m_gpuSlotIndex],
            groups,
            groups,
            1u);
    }

    m_commandList->endMarker();

    struct TextureArrayConstants
    {
        uint32_t sourceMipSize; // offset 0
        uint32_t targetTextureSize; // offset 4
        float pad0; // offset 8
        float pad1; // offset 12
    } textureArrayConstants = {
        textureSize,
        textureSize,
        0.0f,
        0.0f,
    };
    m_commandList->beginMarker("WaveWorks: Assembling texture arrays");
    m_commandList->writeBuffer(
        m_textureArrayMipmapConstantBuffer,
        &textureArrayConstants,
        sizeof(textureArrayConstants));
    const uint32_t groups = textureSize >> 4u;
    dispatchCompute(
        m_commandList,
        m_textureArrayPipeline,
        m_textureArrayBindingSets[m_gpuSlotIndex],
        groups,
        groups,
        kCascadeCount);
    m_commandList->endMarker();

    m_commandList->beginMarker("WaveWorks: Generating mipmaps for texture arrays");
    dispatchCompute(
        m_commandList,
        m_mipmapPipeline,
        m_displacementMipmapBindingSet0,
        groups,
        groups,
        kCascadeCount);
    dispatchCompute(
        m_commandList,
        m_mipmapPipeline,
        m_gradientMipmapBindingSet0,
        groups,
        groups,
        kCascadeCount);
    dispatchCompute(
        m_commandList,
        m_mipmapPipeline,
        m_momentMipmapBindingSet0,
        groups,
        groups,
        kCascadeCount);

    textureArrayConstants.sourceMipSize = textureSize >> 5u;
    m_commandList->writeBuffer(
        m_textureArrayMipmapConstantBuffer,
        &textureArrayConstants,
        sizeof(textureArrayConstants));

    uint32_t coarseGroups = textureSize >> 9u;
    if (coarseGroups == 0)
        coarseGroups = 1;

    dispatchCompute(
        m_commandList,
        m_mipmapPipeline,
        m_displacementMipmapBindingSet1,
        coarseGroups,
        coarseGroups,
        kCascadeCount);
    dispatchCompute(
        m_commandList,
        m_mipmapPipeline,
        m_gradientMipmapBindingSet1,
        coarseGroups,
        coarseGroups,
        kCascadeCount);
    dispatchCompute(
        m_commandList,
        m_mipmapPipeline,
        m_momentMipmapBindingSet1,
        coarseGroups,
        coarseGroups,
        kCascadeCount);
    m_commandList->endMarker();
    m_commandList->endMarker();

    for (uint32_t cascadeIndex = 0; cascadeIndex < kCascadeCount; ++cascadeIndex)
        m_cascadeStates[cascadeIndex].displacementMapVersion =
            displacementVersions[cascadeIndex];

    if (m_enableGpuTimers && m_timerQueries[timerSlot])
        m_commandList->endTimerQuery(m_timerQueries[timerSlot]);

    return true;
}

WWRESULT Wind_Waves_Simulation::waitForAllInFlightTimers()
{
    if (!m_enableGraphics)
        return WWE_OK;

    for (uint32_t next = nextTimerSlot(m_timerReadIndex);
         next != m_timerWriteIndex;
         next = nextTimerSlot(next))
    {
        m_device->getTimerQueryTime(m_timerQueries[next]);
    }

    return WWE_OK;
}

WWRESULT Wind_Waves_Simulation::waitStagingCursor()
{
    if (!m_manager)
        return WWE_FAIL;

    const WWRESULT result = m_manager->waitStagingCursor();
    if (result == WWE_OK || result == WWE_NONE)
        return result;
    return WWE_FAIL;
}

void Wind_Waves_Simulation::MessageCallback::message(
    nvrhi::MessageSeverity severity, const char* text) noexcept
{
    std::fprintf(stderr, "WaveWorks NVRHI [%d]: %s\n", int(severity), text);
}

Wind_Waves_Detailed_Simulation_Parameters Wind_Waves_Simulation::makeDetailedParams() const
{
    Wind_Waves_Detailed_Simulation_Parameters params{};
    params.foamWhitecapsThreshold = m_foamWhitecapsThreshold;
    for (uint32_t i = 0; i < kCascadeCount; ++i)
        params.cascades[i] = m_cascades[i];
    params.timeScale = m_timeScale;
    params.numGPUs = m_numGPUs;
    params.simulationAPI = m_simulationAPI;
    params.cpuThreadingModel = m_cpuThreadingModel;
    params.enableGpuTimers = m_enableGpuTimers;
    params.enableCpuTimers = m_enableCpuTimers;
    params.enableGpuDrivenDisplacementCalculation = m_enableGpuDrivenDisplacementCalculation;
    return params;
}

void Wind_Waves_Simulation::allocateThreadPool(
    GFSDK_WaveWorks_Simulation_CPU_Threading_Model threadingModel)
{
    if (m_threadPool)
    {
        delete m_threadPool;
        m_threadPool = nullptr;
    }

    m_threadPool = new ThreadPool(static_cast<unsigned int>(threadingModel));
}

void Wind_Waves_Simulation::resetTimerRing()
{
    m_timerReadIndex = 0;
    m_timerWriteIndex = 1;
    for (float& result : m_timerResultsMs)
        result = 0.0f;
}

void Wind_Waves_Simulation::setDetailedParams(
    const Wind_Waves_Detailed_Simulation_Parameters& params)
{
    m_foamWhitecapsThreshold = params.foamWhitecapsThreshold;
    for (uint32_t i = 0; i < kCascadeCount; ++i)
        m_cascades[i] = params.cascades[i];
    m_timeScale = params.timeScale;
    m_numGPUs = params.numGPUs;
    m_simulationAPI = params.simulationAPI;
    m_cpuThreadingModel = params.cpuThreadingModel;
    m_enableGpuTimers = params.enableGpuTimers;
    m_enableCpuTimers = params.enableCpuTimers;
    m_enableGpuDrivenDisplacementCalculation =
        params.enableGpuDrivenDisplacementCalculation;
    m_padding0 = 0;
}

bool Wind_Waves_Simulation::shouldRecreateRendering(
    const Wind_Waves_Detailed_Simulation_Parameters& params) const
{
    if (params.enableGpuDrivenDisplacementCalculation !=
            m_enableGpuDrivenDisplacementCalculation)
    {
        return true;
    }

    for (uint32_t i = 0; i < kCascadeCount; ++i)
    {
        if (params.cascades[i].textureSize != m_cascades[i].textureSize ||
            params.cascades[i].writeDisplacementMap !=
                m_cascades[i].writeDisplacementMap)
        {
            return true;
        }
    }

    return false;
}
