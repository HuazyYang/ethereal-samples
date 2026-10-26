#include "Local_Waves_Simulation.h"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <limits>
#include <utility>
#include <vector>

#include "CustomMemory.h"
#include "Simulation_Util.h"

#if defined(_WIN32)
#include <nvrhi/d3d11.h>
#include <nvrhi/d3d12.h>
#endif

#include <nvrhi/vulkan.h>
#include <nvrhi/factory.h>

#include "ShaderBytecode.h"

void handle_ww_error(const wchar_t* filename, uint32_t line);

namespace
{
constexpr uint64_t kInvalidKickId = std::numeric_limits<uint64_t>::max();
constexpr uint32_t kMaxGpuSampleCount = 0x100000;

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
    if (!device || !bytecode || bytecodeSize == 0)
        return nullptr;

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

uint32_t clampedSecondBatchMip(uint32_t mipExponent, uint32_t targetMip, uint32_t fallbackMip)
{
    return mipExponent >= targetMip ? targetMip : fallbackMip;
}

void zeroStats(GFSDK_WaveWorks_Local_Waves_Simulation_Stats& stats)
{
    stats.CPU_main_thread_wait_time = 0.0f;
    stats.CPU_threads_total_time = 0.0f;
    stats.GPU_simulation_time = 0.0f;
    stats.GPU_update_time = 0.0f;
    stats.GPU_total_time = 0.0f;
}

static inline WWRESULT mapCursorAdvanceResult(WWRESULT result, bool& wouldBlock) {
    if (result == WWE_OK)
    {
        wouldBlock = false;
        return WWE_OK;
    }
    if (result == WWE_NONE)
    {
        wouldBlock = false;
        return WWE_NONE;
    }
    if (result == WWE_WOULD_BLOCK)
    {
        wouldBlock = true;
        return WWE_WOULD_BLOCK;
    }
    if (result == WWE_FAIL)
        return WWE_FAIL;
    return WWE_INTERNAL_ERROR;
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
}

Local_Waves_Simulation::Local_Waves_Simulation()
    : m_deltaTime(0.0f)
    , m_params{}
    , m_simulation(nullptr)
    , m_displacementMapVersion(kInvalidKickId)
    , m_renderingResetPending(false)
    , m_simulationResetPending(false)
    , m_threadPool(nullptr)
    , m_disturbanceBuffers{}
    , m_disturbanceWriteIndex(0)
    , m_device(nullptr)
    , m_commandList(nullptr)
    , m_enableGraphics(false)
    , m_foamTexture(nullptr)
    , m_gradientTexture(nullptr)
    , m_gradientFoamShader(nullptr)
    , m_gradientConstantBuffer(nullptr)
    , m_gradientFoamPipeline(nullptr)
    , m_gradientFoamBindingLayout(nullptr)
    , m_gradientFoamBindingSet(nullptr)
    , m_foamInjectionShader(nullptr)
    , m_foamInjectionPipeline(nullptr)
    , m_foamInjectionBindingLayout(nullptr)
    , m_foamInjectionBindingSet(nullptr)
    , m_wrapSampler(nullptr)
    , m_borderSampler(nullptr)
    , m_gpuSampleShader(nullptr)
    , m_readbackPositionsBuffer(nullptr)
    , m_readbackResultsBuffer(nullptr)
    , m_readbackResultsBufferStaging(nullptr)
    , m_calculateReadbacksCB(nullptr)
    , m_gpuSamplePipeline(nullptr)
    , m_gpuSampleBindingLayout(nullptr)
    , m_gpuSampleBindingSet(nullptr)
    , m_mipmapConstantBuffer(nullptr)
    , m_mipmapShader(nullptr)
    , m_mipmapPipeline(nullptr)
    , m_displacementMipmapBindingSet0(nullptr)
    , m_displacementMipmapBindingSet1(nullptr)
    , m_gradientMipmapBindingSet0(nullptr)
    , m_gradientMipmapBindingSet1(nullptr)
    , m_mipmapBindingLayout(nullptr)
    , m_timerResultsMs{}
    , m_timerReadIndex(0)
    , m_timerWriteIndex(1)
    , m_timerQueries{}
{
}

Local_Waves_Simulation::~Local_Waves_Simulation()
{
    releaseAll();
    m_commandList = nullptr;
    m_device = nullptr;
}

void Local_Waves_Simulation::addDisturbances(
    gfsdk_float4* disturbanceData,
    uint32_t numSamples)
{
    // TODO: improve me using SIMD
    const uint32_t N = m_params.textureSize;
    float2* target = m_disturbanceBuffers[m_disturbanceWriteIndex];
    for (uint32_t i = 0; i < numSamples; ++i)
    {
        const uint32_t x = static_cast<uint32_t>(disturbanceData[i].x);
        const uint32_t y = static_cast<uint32_t>(disturbanceData[i].y);
        if (x >= N || y >= N)
            continue;

        float2& cell = target[static_cast<size_t>(y) * N + x];
        cell.x += disturbanceData[i].z;
        cell.y += disturbanceData[i].w;
    }
}

WWRESULT Local_Waves_Simulation::advanceReadbackCursor(bool block, bool& wouldBlock)
{
    return mapCursorAdvanceResult(m_simulation->advanceReadbackCursor(block), wouldBlock);
}

WWRESULT Local_Waves_Simulation::advanceStagingCursor(bool block, bool& wouldBlock)
{
    const WWRESULT result = m_simulation->advanceStagingCursor(block);
    WWRESULT mappedResult = mapCursorAdvanceResult(result, wouldBlock);
    if (mappedResult != WWE_OK)
        return mappedResult;

    m_commandList->open();
    updateGradientMaps();
    m_commandList->close();
    m_device->executeCommandList(m_commandList);

    return WWE_OK;
}

WWRESULT Local_Waves_Simulation::allocateAll()
{
    WWRESULT simulationResult = allocateSimulation();
    if (simulationResult < 0)
    {
        handle_ww_error(
            L"C:\\Work\\dev\\waveworks_example.package\\waveworks\\src\\Local_Waves_Simulation.cpp",
            0x107u);
        return simulationResult;
    }

    WWRESULT renderingResult = allocateRenderingResources();
    if (renderingResult < 0)
    {
        handle_ww_error(
            L"C:\\Work\\dev\\waveworks_example.package\\waveworks\\src\\Local_Waves_Simulation.cpp",
            0x108u);
        return renderingResult;
    }

    return WWE_OK;
}

WWRESULT Local_Waves_Simulation::allocateRenderingResources()
{
    if (m_enableGraphics)
    {
        m_renderingResetPending = true;

        const uint32_t N = m_params.textureSize;
        nvrhi::TextureHandle displacementTexture = m_simulation->getDisplacementMap();
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
        textureDesc.debugName = "Local waves gradients texture";
        textureDesc.isUAV = true;
        textureDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        textureDesc.keepInitialState = true;
        m_device->createTexture(textureDesc, &m_gradientTexture);
        if (m_gradientTexture && m_device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN && textureDesc.keepInitialState) {
            // Establish the native image layout before exposing it to the renderer.
            nvrhi::CommandListHandle initializeCommands;
            m_device->createCommandList(nvrhi::CommandListParameters(), &initializeCommands);
            if (!initializeCommands) return WWE_FAIL;
            initializeCommands->open();
            initializeCommands->beginTrackingTextureState(m_gradientTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);
            initializeCommands->setTextureState(m_gradientTexture, nvrhi::AllSubresources, textureDesc.initialState);
            initializeCommands->commitBarriers();
            initializeCommands->close();
            m_device->executeCommandList(initializeCommands);
        }

        textureDesc.format = nvrhi::Format::R16_FLOAT;
        textureDesc.debugName = "Local waves foam energy texture";
        m_device->createTexture(textureDesc, &m_foamTexture);
        if (m_foamTexture && m_device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN && textureDesc.keepInitialState) {
            // Establish the native image layout before exposing it to the renderer.
            nvrhi::CommandListHandle initializeCommands;
            m_device->createCommandList(nvrhi::CommandListParameters(), &initializeCommands);
            if (!initializeCommands) return WWE_FAIL;
            initializeCommands->open();
            initializeCommands->beginTrackingTextureState(m_foamTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);
            initializeCommands->setTextureState(m_foamTexture, nvrhi::AllSubresources, textureDesc.initialState);
            initializeCommands->commitBarriers();
            initializeCommands->close();
            m_device->executeCommandList(initializeCommands);
        }

        if (m_device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN)
        {
            m_gradientFoamShader = createComputeShader(
                m_device,
                BlurFoamAndCalculateGradientsForLocalWavesCS_bytecode_vk,
                BlurFoamAndCalculateGradientsForLocalWavesCS_bytecode_vk_size);
            m_foamInjectionShader = createComputeShader(
                m_device,
                InjectAndBlurFoamForLocalWavesCS_bytecode_vk,
                InjectAndBlurFoamForLocalWavesCS_bytecode_vk_size);
            m_mipmapShader = createComputeShader(
                m_device,
                GenerateMipmapsForLocalWavesCS_bytecode_vk,
                GenerateMipmapsForLocalWavesCS_bytecode_vk_size);
        }
        else
        {
            m_gradientFoamShader = createComputeShader(
                m_device,
                BlurFoamAndCalculateGradientsForLocalWavesCS_bytecode_dx,
                BlurFoamAndCalculateGradientsForLocalWavesCS_bytecode_dx_size);
            m_foamInjectionShader = createComputeShader(
                m_device,
                InjectAndBlurFoamForLocalWavesCS_bytecode_dx,
                InjectAndBlurFoamForLocalWavesCS_bytecode_dx_size);
            m_mipmapShader = createComputeShader(
                m_device,
                GenerateMipmapsForLocalWavesCS_bytecode_dx,
                GenerateMipmapsForLocalWavesCS_bytecode_dx_size);
        }

        nvrhi::BufferDesc constantsDesc;
        constantsDesc.byteSize = 32;
        constantsDesc.debugName = "Gradients Calc Pixel Shader CB";
        constantsDesc.isConstantBuffer = true;
        constantsDesc.isVolatile = true;
        constantsDesc.maxVersions = 4096;
        m_device->createBuffer(constantsDesc, &m_gradientConstantBuffer);

        constantsDesc.byteSize = 16;
        constantsDesc.debugName = "Generate Mipmaps CB";
        m_device->createBuffer(constantsDesc, &m_mipmapConstantBuffer);

        nvrhi::SamplerDesc wrapSamplerDesc;
        wrapSamplerDesc.addressU = nvrhi::SamplerAddressMode::Wrap;
        wrapSamplerDesc.addressV = nvrhi::SamplerAddressMode::Wrap;
        wrapSamplerDesc.addressW = nvrhi::SamplerAddressMode::Wrap;
        wrapSamplerDesc.minFilter = true;
        wrapSamplerDesc.magFilter = true;
        wrapSamplerDesc.mipFilter = false;
        wrapSamplerDesc.maxAnisotropy = 0.0f;
        wrapSamplerDesc.borderColor = nvrhi::Color(0.0f);
        m_device->createSampler(wrapSamplerDesc, &m_wrapSampler);

        nvrhi::SamplerDesc borderSamplerDesc;
        borderSamplerDesc.addressU = nvrhi::SamplerAddressMode::Border;
        borderSamplerDesc.addressV = nvrhi::SamplerAddressMode::Border;
        borderSamplerDesc.addressW = nvrhi::SamplerAddressMode::Border;
        borderSamplerDesc.minFilter = true;
        borderSamplerDesc.magFilter = true;
        borderSamplerDesc.mipFilter = false;
        borderSamplerDesc.maxAnisotropy = 0.0f;
        borderSamplerDesc.borderColor = nvrhi::Color(0.0f);
        m_device->createSampler(borderSamplerDesc, &m_borderSampler);

        m_gradientFoamBindingLayout = createComputeBindingLayout(
            m_device,
            {bindingLayoutItem(0, nvrhi::ResourceType::Texture_UAV),
             bindingLayoutItem(1, nvrhi::ResourceType::Texture_UAV),
             bindingLayoutItem(0, nvrhi::ResourceType::Texture_SRV),
             bindingLayoutItem(0, nvrhi::ResourceType::Sampler),
             bindingLayoutItem(0, nvrhi::ResourceType::VolatileConstantBuffer)});
        m_gradientFoamPipeline = createComputePipeline(
            m_device, m_gradientFoamShader, m_gradientFoamBindingLayout);

        m_foamInjectionBindingLayout = createComputeBindingLayout(
            m_device,
            {bindingLayoutItem(0, nvrhi::ResourceType::Texture_SRV),
             bindingLayoutItem(0, nvrhi::ResourceType::Texture_UAV),
             bindingLayoutItem(0, nvrhi::ResourceType::Sampler),
             bindingLayoutItem(0, nvrhi::ResourceType::VolatileConstantBuffer)});
        m_foamInjectionPipeline = createComputePipeline(
            m_device, m_foamInjectionShader, m_foamInjectionBindingLayout);

        m_gradientFoamBindingSet = createComputeBindingSet(
            m_device,
            m_gradientFoamBindingLayout,
            {nvrhi::BindingSetItem::Texture_UAV(
                 0, displacementTexture, nvrhi::Format::RGBA16_FLOAT),
             nvrhi::BindingSetItem::Texture_UAV(
                 1, m_gradientTexture, nvrhi::Format::RGBA16_FLOAT),
             nvrhi::BindingSetItem::Texture_SRV(
                 0, m_foamTexture, nvrhi::Format::R16_FLOAT),
             nvrhi::BindingSetItem::Sampler(0, m_wrapSampler),
             nvrhi::BindingSetItem::ConstantBuffer(0, m_gradientConstantBuffer)});
        m_foamInjectionBindingSet = createComputeBindingSet(
            m_device,
            m_foamInjectionBindingLayout,
            {nvrhi::BindingSetItem::Texture_SRV(
                 0, m_gradientTexture, nvrhi::Format::RGBA16_FLOAT),
             nvrhi::BindingSetItem::Texture_UAV(
                 0, m_foamTexture, nvrhi::Format::R16_FLOAT),
             nvrhi::BindingSetItem::Sampler(0, m_wrapSampler),
             nvrhi::BindingSetItem::ConstantBuffer(0, m_gradientConstantBuffer)});

        m_displacementMapVersion = kInvalidKickId;

        m_mipmapBindingLayout = createComputeBindingLayout(
            m_device,
            {bindingLayoutItem(0, nvrhi::ResourceType::Texture_SRV),
             bindingLayoutItem(0, nvrhi::ResourceType::Texture_UAV),
             bindingLayoutItem(1, nvrhi::ResourceType::Texture_UAV),
             bindingLayoutItem(2, nvrhi::ResourceType::Texture_UAV),
             bindingLayoutItem(3, nvrhi::ResourceType::Texture_UAV),
             bindingLayoutItem(4, nvrhi::ResourceType::Texture_UAV),
             bindingLayoutItem(0, nvrhi::ResourceType::Sampler),
             bindingLayoutItem(0, nvrhi::ResourceType::VolatileConstantBuffer)});
        m_mipmapPipeline = createComputePipeline(
            m_device, m_mipmapShader, m_mipmapBindingLayout);

        auto createMipmapBindingSet =
            [&](nvrhi::ITexture* texture,
                nvrhi::Format format,
                uint32_t sourceMip,
                uint32_t targetMip0,
                uint32_t targetMip1,
                uint32_t targetMip2,
                uint32_t targetMip3,
                uint32_t targetMip4)
        {
            return createComputeBindingSet(
                m_device,
                m_mipmapBindingLayout,
                {nvrhi::BindingSetItem::Texture_SRV(
                     0,
                     texture,
                     format,
                     nvrhi::TextureSubresourceSet(sourceMip, 1, 0, 1)),
                 nvrhi::BindingSetItem::Texture_UAV(
                     0,
                     texture,
                     format,
                     nvrhi::TextureSubresourceSet(targetMip0, 1, 0, 1)),
                 nvrhi::BindingSetItem::Texture_UAV(
                     1,
                     texture,
                     format,
                     nvrhi::TextureSubresourceSet(targetMip1, 1, 0, 1)),
                 nvrhi::BindingSetItem::Texture_UAV(
                     2,
                     texture,
                     format,
                     nvrhi::TextureSubresourceSet(targetMip2, 1, 0, 1)),
                 nvrhi::BindingSetItem::Texture_UAV(
                     3,
                     texture,
                     format,
                     nvrhi::TextureSubresourceSet(targetMip3, 1, 0, 1)),
                 nvrhi::BindingSetItem::Texture_UAV(
                     4,
                     texture,
                     format,
                     nvrhi::TextureSubresourceSet(targetMip4, 1, 0, 1)),
                 nvrhi::BindingSetItem::Sampler(0, m_wrapSampler),
                 nvrhi::BindingSetItem::ConstantBuffer(0, m_mipmapConstantBuffer)});
        };

        m_displacementMipmapBindingSet0 = createMipmapBindingSet(
            displacementTexture, nvrhi::Format::RGBA16_FLOAT, 0, 1, 2, 3, 4, 5);
        m_displacementMipmapBindingSet1 = createMipmapBindingSet(
            displacementTexture,
            nvrhi::Format::RGBA16_FLOAT,
            5,
            clampedSecondBatchMip(mipExponent, 6, 0),
            clampedSecondBatchMip(mipExponent, 7, 1),
            clampedSecondBatchMip(mipExponent, 8, 2),
            clampedSecondBatchMip(mipExponent, 9, 3),
            clampedSecondBatchMip(mipExponent, 10, 4));
        m_gradientMipmapBindingSet0 = createMipmapBindingSet(
            m_gradientTexture, nvrhi::Format::RGBA16_FLOAT, 0, 1, 2, 3, 4, 5);
        m_gradientMipmapBindingSet1 = createMipmapBindingSet(
            m_gradientTexture,
            nvrhi::Format::RGBA16_FLOAT,
            5,
            clampedSecondBatchMip(mipExponent, 6, 0),
            clampedSecondBatchMip(mipExponent, 7, 1),
            clampedSecondBatchMip(mipExponent, 8, 2),
            clampedSecondBatchMip(mipExponent, 9, 3),
            clampedSecondBatchMip(mipExponent, 10, 4));

        for (uint32_t i = 0; i < array_dim(m_timerQueries); ++i)
        {
            m_device->createTimerQuery(&m_timerQueries[i]);
            m_timerResultsMs[i] = 0.0f;
        }
        m_timerReadIndex = 0;
        m_timerWriteIndex = 1;
    }

    const bool allocateGpuSampling =
        m_params.enableGpuDrivenDisplacementCalculation &&
        (m_params.simulationAPI == GFSDK_WaveWorks_Simulation_API_Compute ||
         (m_params.simulationAPI == GFSDK_WaveWorks_Simulation_API_CPU && m_enableGraphics));
    if (allocateGpuSampling)
    {
        if (m_device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN)
        {
            m_gpuSampleShader = createComputeShader(
                m_device,
                CalculateDisplacementReadbacksForLocalWavesCS_bytecode_vk,
                CalculateDisplacementReadbacksForLocalWavesCS_bytecode_vk_size);
        }
        else
        {
            m_gpuSampleShader = createComputeShader(
                m_device,
                CalculateDisplacementReadbacksForLocalWavesCS_bytecode_dx,
                CalculateDisplacementReadbacksForLocalWavesCS_bytecode_dx_size);
        }
        if (!m_borderSampler)
        {
            nvrhi::SamplerDesc borderSamplerDesc;
            borderSamplerDesc.addressU = nvrhi::SamplerAddressMode::Border;
            borderSamplerDesc.addressV = nvrhi::SamplerAddressMode::Border;
            borderSamplerDesc.addressW = nvrhi::SamplerAddressMode::Border;
            borderSamplerDesc.minFilter = true;
            borderSamplerDesc.magFilter = true;
            borderSamplerDesc.mipFilter = false;
            borderSamplerDesc.maxAnisotropy = 0.0f;
            borderSamplerDesc.borderColor = nvrhi::Color(0.0f);
            m_device->createSampler(borderSamplerDesc, &m_borderSampler);
        }

        nvrhi::BufferDesc constantsDesc;
        constantsDesc.byteSize = 16;
        constantsDesc.isConstantBuffer = true;
        constantsDesc.isVolatile = true;
        constantsDesc.maxVersions = 4096;
        constantsDesc.debugName = "m_hCalculateReadbacksCB";
        m_device->createBuffer(constantsDesc, &m_calculateReadbacksCB);

        nvrhi::BufferDesc sampleDesc;
        sampleDesc.byteSize = kMaxGpuSampleCount * sizeof(float2);
        sampleDesc.structStride = sizeof(float2);
        sampleDesc.debugName = "m_hReadbackPositionsBuffer";
        sampleDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        sampleDesc.keepInitialState = true;
        sampleDesc.cpuAccess = nvrhi::CpuAccessMode::Write;
        m_device->createBuffer(sampleDesc, &m_readbackPositionsBuffer);

        sampleDesc.byteSize = kMaxGpuSampleCount * sizeof(float4);
        sampleDesc.structStride = sizeof(float4);
        sampleDesc.canHaveUAVs = true;
        sampleDesc.debugName = "m_hReadbackResultsBuffer";
        sampleDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
        sampleDesc.cpuAccess = nvrhi::CpuAccessMode::None;
        m_device->createBuffer(sampleDesc, &m_readbackResultsBuffer);

        sampleDesc.canHaveUAVs = false;
        sampleDesc.debugName = "m_hReadbackResultsBufferStaging";
        sampleDesc.cpuAccess = nvrhi::CpuAccessMode::Read;
        m_device->createBuffer(sampleDesc, &m_readbackResultsBufferStaging);

        m_gpuSampleBindingLayout = createComputeBindingLayout(
            m_device,
            {bindingLayoutItem(0, nvrhi::ResourceType::StructuredBuffer_SRV),
             bindingLayoutItem(1, nvrhi::ResourceType::Texture_SRV),
             bindingLayoutItem(0, nvrhi::ResourceType::StructuredBuffer_UAV),
             bindingLayoutItem(0, nvrhi::ResourceType::VolatileConstantBuffer),
             bindingLayoutItem(0, nvrhi::ResourceType::Sampler)});
        nvrhi::ITexture* displacementTexture = nullptr;
        {
            nvrhi::TextureHandle retainedDisplacementTexture =
                m_simulation->getDisplacementMap();
            displacementTexture = retainedDisplacementTexture.Get();
        }
        m_gpuSampleBindingSet = createComputeBindingSet(
            m_device,
            m_gpuSampleBindingLayout,
            {nvrhi::BindingSetItem::StructuredBuffer_SRV(
                 0, m_readbackPositionsBuffer, nvrhi::Format::RG32_FLOAT),
             nvrhi::BindingSetItem::Texture_SRV(
                 1,
                 displacementTexture,
                 nvrhi::Format::RGBA16_FLOAT,
                 nvrhi::TextureSubresourceSet(0, 1, 0, 1)),
             nvrhi::BindingSetItem::StructuredBuffer_UAV(
                 0, m_readbackResultsBuffer, nvrhi::Format::RGBA32_FLOAT),
             nvrhi::BindingSetItem::ConstantBuffer(0, m_calculateReadbacksCB),
             nvrhi::BindingSetItem::Sampler(0, m_borderSampler)});
        m_gpuSamplePipeline = createComputePipeline(
            m_device, m_gpuSampleShader, m_gpuSampleBindingLayout);
    }

    return WWE_OK;
}

WWRESULT Local_Waves_Simulation::allocateSimulation()
{
    if (!isValidSimulationAPI(m_params.simulationAPI))
        return WWE_FAIL;

    if (m_params.simulationAPI == GFSDK_WaveWorks_Simulation_API_Compute)
    {
        m_simulation = new Local_Waves_FFT_Simulation_Compute_Impl(m_params);
    }
    else
    {
        m_simulation = new Local_Waves_FFT_Simulation_CPU_Impl(m_params, m_threadPool);
    }

    m_simulation->init(m_device.Get(), m_commandList, m_enableGraphics);

    const size_t disturbanceCount =
        static_cast<size_t>(m_params.textureSize) * static_cast<size_t>(m_params.textureSize);
    const size_t disturbanceBytes = sizeof(float2) * disturbanceCount;
    m_disturbanceBuffers[0] =
        reinterpret_cast<float2*>(NVSDK_aligned_malloc(disturbanceBytes, 16));
    m_disturbanceBuffers[1] =
        reinterpret_cast<float2*>(NVSDK_aligned_malloc(disturbanceBytes, 16));
    memset(m_disturbanceBuffers[0], 0, disturbanceBytes);
    memset(m_disturbanceBuffers[1], 0, disturbanceBytes);
    return WWE_OK;
}

WWRESULT Local_Waves_Simulation::consumeAvailableTimerSlot(uint32_t& slot)
{
    if (m_enableGraphics)
    {
        if (m_timerReadIndex == m_timerWriteIndex)
        {
            const uint32_t next = (m_timerReadIndex + 1) % array_dim(m_timerQueries);
            m_timerResultsMs[next] =
                m_device->getTimerQueryTime(m_timerQueries[next]) * 1000.0f;
            m_timerReadIndex = next;
        }
        slot = m_timerWriteIndex;
        m_timerWriteIndex = (m_timerWriteIndex + 1) % array_dim(m_timerQueries);
    }

    return WWE_OK;
}

WWRESULT Local_Waves_Simulation::getDisplacements(
    const gfsdk_float2* samplePoints,
    gfsdk_float4* displacements,
    uint32_t numSamples,
    bool calculateOnGPU)
{
    if (!samplePoints || !displacements || !m_simulation)
        return WWE_FAIL;

    if (calculateOnGPU)
    {
        if (!m_params.enableGpuDrivenDisplacementCalculation || !m_device ||
            !m_commandList || !m_readbackPositionsBuffer || !m_readbackResultsBuffer ||
            !m_readbackResultsBufferStaging || !m_calculateReadbacksCB ||
            !m_gpuSamplePipeline || !m_gpuSampleBindingSet ||
            numSamples > kMaxGpuSampleCount ||
            (m_params.simulationAPI == GFSDK_WaveWorks_Simulation_API_CPU && !m_enableGraphics))
        {
            return WWE_FAIL;
        }

        nvrhi::TextureHandle displacementTexture = m_simulation->getDisplacementMap();
        if (!displacementTexture)
            return WWE_FAIL;

        void* inputData = m_device->mapBuffer(m_readbackPositionsBuffer, nvrhi::CpuAccessMode::Write);
        if (!inputData)
            return WWE_FAIL;
        memcpy(inputData, samplePoints, sizeof(gfsdk_float2) * numSamples);
        m_device->unmapBuffer(m_readbackPositionsBuffer);

        m_commandList->open();
        m_commandList->beginTrackingTextureState(
            displacementTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);

        struct GpuSampleConstants
        {
            gfsdk_float2 domainCenter; // offset 0
            float domainSize; // offset 8
            uint32_t sampleCount; // offset 12
        } constants = {
            make_float2(m_params.offsetX, m_params.offsetY),
            m_params.physicalSize,
            numSamples,
        };
        m_commandList->writeBuffer(
            m_calculateReadbacksCB, &constants, sizeof(constants));
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
        m_commandList->setTextureState(displacementTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
        m_commandList->commitBarriers();
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

    uint8_t* displacementData = nullptr;
    uint32_t rowPitch = 0;
    bool fp16 = false;
    WWRESULT mapResult =
        m_simulation->mapDisplacementData(&displacementData, &rowPitch, &fp16);
    if (mapResult < 0)
        return mapResult;
    if (mapResult != WWE_OK || !displacementData)
        return WWE_FAIL;

    if (numSamples < 512u || !m_threadPool)
    {
        Simulation_Util::addLocalDisplacements(
            m_params, displacementData, rowPitch, samplePoints, displacements,
            0, numSamples, fp16);
    }
    else
    {
        const uint32_t threadCount = std::max(1u, m_threadPool->getThreadCount());
        const uint32_t chunkSize = std::max(128u, numSamples / threadCount);
        std::vector<std::function<void()>> tasks;
        for (uint32_t start = 0; start < numSamples; start += chunkSize)
        {
            const uint32_t count = std::min(chunkSize, numSamples - start);
            tasks.emplace_back(
                [this, displacementData, rowPitch, samplePoints, displacements,
                 start, count, fp16]() {
                    Simulation_Util::addLocalDisplacements(
                        m_params, displacementData, rowPitch, samplePoints,
                        displacements, start, count, fp16);
                });
        }
        m_threadPool->prepareHighPrioritySubmission(static_cast<unsigned int>(tasks.size()));
        m_threadPool->submitHighPriorityVector(std::move(tasks));
        m_threadPool->waitForHighPriorityWorkDone();
    }

    m_simulation->unmapDisplacementData();
    return WWE_OK;
}

bool Local_Waves_Simulation::getReadbackCursor(uint64_t* cursor) const
{
    return m_simulation && m_simulation->getReadbackCursor(cursor);
}

WWRESULT Local_Waves_Simulation::getRenderingResources(
    GFSDK_WaveWorks_Local_Waves_Rendering_Data& data) const
{
    if (!m_enableGraphics || !m_simulation || !m_device)
        return WWE_FAIL;

    data.simulation_domain_worldspace_center = make_float2(m_params.offsetX, m_params.offsetY);
    data.simulation_domain_worldspace_size = m_params.physicalSize;
    data.size_of_texture = m_params.textureSize;

    nvrhi::TextureHandle displacementTexture = m_simulation->getDisplacementMap();
    if (!displacementTexture || !m_gradientTexture)
        return WWE_FAIL;

    nvrhi::ObjectType objectType = 0;
    switch (m_device->getGraphicsAPI())
    {
    case nvrhi::GraphicsAPI::D3D11:
        objectType = nvrhi::ObjectTypes::D3D11_Resource;
        break;
    case nvrhi::GraphicsAPI::D3D12:
        objectType = nvrhi::ObjectTypes::D3D12_Resource;
        break;
    case nvrhi::GraphicsAPI::VULKAN:
        objectType = nvrhi::ObjectTypes::VK_Image;
        break;
    default:
        return WWE_FAIL;
    }

    data.displacements_texture = displacementTexture->getNativeObject(objectType);
    data.gradients_texture = m_gradientTexture->getNativeObject(objectType);

    return data.displacements_texture && data.gradients_texture ? WWE_OK : WWE_FAIL;
}

bool Local_Waves_Simulation::getStagingCursor(uint64_t* cursor) const
{
    return m_simulation && m_simulation->getStagingCursor(cursor);
}

WWRESULT Local_Waves_Simulation::getStats(
    GFSDK_WaveWorks_Local_Waves_Simulation_Stats& stats) const
{
    if (!m_simulation)
        return WWE_FAIL;

    zeroStats(stats);
    const WWRESULT result = m_simulation->getTimings(stats);
    if (result < 0)
        return result;

    const float updateMs = m_timerResultsMs[m_timerReadIndex];
    stats.GPU_update_time = updateMs;
    stats.GPU_total_time = stats.GPU_simulation_time + stats.GPU_update_time;
    return WWE_OK;
}

WWRESULT Local_Waves_Simulation::init11(
    void* deviceContext,
    const Local_Waves_Detailed_Simulation_Parameters& params,
    bool enableGraphics)
{
    if ((enableGraphics || params.simulationAPI == GFSDK_WaveWorks_Simulation_API_Compute) &&
        !deviceContext)
    {
        return WWE_FAIL;
    }

    m_enableGraphics = enableGraphics;
    m_params = params;

#if defined(_WIN32)
    if (deviceContext)
    {
        nvrhi::d3d11::DeviceDesc deviceDesc;
        deviceDesc.messageCallback = m_messageCallback;
        deviceDesc.context = reinterpret_cast<ID3D11DeviceContext*>(deviceContext);
        nvrhi::d3d11::createGraphicsFactory(nvrhi::d3d11::GraphicsFactoryDesc())->createDevice(deviceDesc, &m_device);
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

    allocateThreadPool(params.cpuThreadingModel);
    return allocateAll();
}

WWRESULT Local_Waves_Simulation::init12(
    void* device,
    void* queue,
    const Local_Waves_Detailed_Simulation_Parameters& params,
    bool enableGraphics)
{
    if ((enableGraphics && (!device || !queue)) ||
        (params.simulationAPI == GFSDK_WaveWorks_Simulation_API_Compute && !device))
    {
        return WWE_FAIL;
    }

    m_enableGraphics = enableGraphics;
    m_params = params;

#if defined(_WIN32)
    nvrhi::d3d12::DeviceDesc deviceDesc;
    deviceDesc.errorCB = m_messageCallback;
    deviceDesc.pDevice = reinterpret_cast<ID3D12Device*>(device);
    deviceDesc.pGraphicsCommandQueue = reinterpret_cast<ID3D12CommandQueue*>(queue);
    nvrhi::d3d12::DeviceHandle d3d12Device;
    nvrhi::d3d12::createGraphicsFactory(nvrhi::d3d12::GraphicsFactoryDesc())->createDevice(deviceDesc, &d3d12Device);
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
    return allocateAll();
}

WWRESULT Local_Waves_Simulation::initVK(
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
    const Local_Waves_Detailed_Simulation_Parameters& params,
    bool enableGraphics)
{
    if ((enableGraphics || params.simulationAPI == GFSDK_WaveWorks_Simulation_API_Compute) &&
        (!instance || !physicalDevice || !device || !computeQueue))
    {
        return WWE_FAIL;
    }

    m_enableGraphics = enableGraphics;
    m_params = params;

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
    nvrhi::vulkan::DeviceHandle vulkanDevice;
    // A headless factory wraps the host's native objects (nvrhi ADR 0014); its own instance goes with it. nvrhi
    // initializes the Vulkan-Hpp dispatcher of this module for the host's instance and device.
    nvrhi::vulkan::GraphicsFactoryDesc factoryDesc;
    factoryDesc.headless = true;
    nvrhi::vulkan::createGraphicsFactory(factoryDesc)->createDevice(deviceDesc, &vulkanDevice);
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
    return allocateAll();
}

WWRESULT Local_Waves_Simulation::kick(uint64_t* kickId)
{
    m_device->runGarbageCollection();

    if (m_simulationResetPending)
    {
        m_simulation->reset();
        m_renderingResetPending = true;
        m_simulationResetPending = false;
    }

    const uint32_t sourceIndex = m_disturbanceWriteIndex;
    m_disturbanceWriteIndex = (m_disturbanceWriteIndex - 1u) & 1u;

    uint64_t localKickId = kInvalidKickId;
    m_commandList->open();

    const WWRESULT result =
        m_simulation->kick(m_deltaTime, m_disturbanceBuffers[sourceIndex], localKickId);
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

WWRESULT Local_Waves_Simulation::reinit(
    const Local_Waves_Detailed_Simulation_Parameters& params)
{
    if (!isValidSimulationAPI(params.simulationAPI))
        return WWE_FAIL;
    if (params.simulationAPI == GFSDK_WaveWorks_Simulation_API_Compute && !m_device)
        return WWE_FAIL;

    const bool recreateThreadPool = params.cpuThreadingModel != m_params.cpuThreadingModel;
    const bool textureSizeChanged = params.textureSize != m_params.textureSize;
    const bool cpuDisplacementChanged =
        params.enableCpuDrivenDisplacementCalculation !=
        m_params.enableCpuDrivenDisplacementCalculation;
    const bool gpuDisplacementChanged =
        params.enableGpuDrivenDisplacementCalculation !=
        m_params.enableGpuDrivenDisplacementCalculation;
    const bool simulationApiChanged = params.simulationAPI != m_params.simulationAPI;
    const bool recreateSimulation =
        recreateThreadPool || textureSizeChanged || cpuDisplacementChanged ||
        simulationApiChanged;

    const bool recreateRendering =
        textureSizeChanged || gpuDisplacementChanged || cpuDisplacementChanged ||
        simulationApiChanged;

    const bool reinitSimulation =
        params.amplitudeMultiplier != m_params.amplitudeMultiplier ||
        params.lateralMultiplier != m_params.lateralMultiplier ||
        params.physicalSize != m_params.physicalSize;

    if (recreateThreadPool)
    {
        m_threadPool->waitForLowPriorityWorkDone();
        allocateThreadPool(params.cpuThreadingModel);
    }

    m_params = params;

    if (recreateSimulation)
    {
        releaseSimulation();
        WWRESULT simulationResult = allocateSimulation();
        if (simulationResult < 0)
            return simulationResult;
    }

    if (reinitSimulation)
    {
        WWRESULT beforeResult = m_simulation->beforeReinit();
        if (beforeResult < 0)
            return beforeResult;
        WWRESULT reinitResult = m_simulation->reinit(m_params);
        if (reinitResult < 0)
            return reinitResult;
    }

    if (recreateRendering)
    {
        waitForAllInFlightTimers();
        releaseRenderingResources();
        WWRESULT renderingResult = allocateRenderingResources();
        if (renderingResult < 0)
            return renderingResult;
    }

    return WWE_OK;
}

void Local_Waves_Simulation::releaseAll()
{
    releaseSimulation();
    releaseRenderingResources();

    if (m_threadPool)
    {
        delete m_threadPool;
        m_threadPool = nullptr;
    }
}

void Local_Waves_Simulation::releaseRenderingResources()
{
    m_foamTexture = nullptr;
    m_gradientTexture = nullptr;
    m_gradientFoamShader = nullptr;
    m_gradientConstantBuffer = nullptr;
    m_gradientFoamPipeline = nullptr;
    m_gradientFoamBindingLayout = nullptr;
    m_gradientFoamBindingSet = nullptr;
    m_foamInjectionShader = nullptr;
    m_foamInjectionPipeline = nullptr;
    m_foamInjectionBindingLayout = nullptr;
    m_foamInjectionBindingSet = nullptr;
    m_wrapSampler = nullptr;
    m_borderSampler = nullptr;
    m_gpuSampleShader = nullptr;
    m_readbackPositionsBuffer = nullptr;
    m_readbackResultsBuffer = nullptr;
    m_readbackResultsBufferStaging = nullptr;
    m_calculateReadbacksCB = nullptr;
    m_gpuSamplePipeline = nullptr;
    m_gpuSampleBindingLayout = nullptr;
    m_gpuSampleBindingSet = nullptr;
    m_mipmapConstantBuffer = nullptr;
    m_mipmapShader = nullptr;
    m_mipmapPipeline = nullptr;
    m_displacementMipmapBindingSet0 = nullptr;
    m_displacementMipmapBindingSet1 = nullptr;
    m_gradientMipmapBindingSet0 = nullptr;
    m_gradientMipmapBindingSet1 = nullptr;
    m_mipmapBindingLayout = nullptr;

    for (nvrhi::TimerQueryHandle& query : m_timerQueries)
        query = nullptr;
}

void Local_Waves_Simulation::releaseSimulation()
{
    delete m_simulation;
    m_simulation = nullptr;
    freeDisturbanceBuffers();
}

void Local_Waves_Simulation::resetSimulation()
{
    m_simulationResetPending = true;
}

void Local_Waves_Simulation::setSimulationDeltaTime(float deltaTime)
{
    m_deltaTime = deltaTime;
}

bool Local_Waves_Simulation::updateGradientMaps()
{
    if (!m_enableGraphics)
        return true;

    nvrhi::TextureHandle displacementTexture = m_simulation->getDisplacementMap();

    if (m_displacementMapVersion == m_simulation->getDisplacementMapVersion())
        return true;

    m_commandList->beginTrackingTextureState(
        displacementTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);

    uint32_t mipExponent = 0;
    uint32_t mipPower = 0;
    Simulation_Util::powerOf2(m_params.textureSize, &mipExponent, &mipPower);

    uint32_t timerSlot = 0;
    if (m_params.enableGpuTimers)
    {
        consumeAvailableTimerSlot(timerSlot);
        if (m_timerQueries[timerSlot])
        {
            m_device->resetTimerQuery(m_timerQueries[timerSlot]);
            m_commandList->beginTimerQuery(m_timerQueries[timerSlot]);
        }
    }

    m_commandList->beginMarker("WaveWorks: Postprocessing FFT results for local waves");
    m_commandList->beginMarker("WaveWorks: Gradients / foam generation");

    if (m_renderingResetPending)
    {
        m_commandList->clearTextureFloat(
            m_gradientTexture, nvrhi::TextureSubresourceSet(0, 1, 0, 1), nvrhi::Color(0.0f, 1.f, 0.f, 0.0f));
        m_commandList->clearTextureFloat(
            m_foamTexture, nvrhi::TextureSubresourceSet(0, 1, 0, 1), nvrhi::Color(0.0f));
        m_commandList->clearTextureFloat(
            displacementTexture, nvrhi::TextureSubresourceSet(0, 1, 0, 1), nvrhi::Color(0.0f));
        m_renderingResetPending = false;
    }

    struct GradientConstants
    {
        uint32_t simulationGridSize; // offset 0
        float simulationWorldspaceSize; // offset 4
        float deltaTime; // offset 8
        float foamTemporalDissipation; // offset 12
        float foamSpatialDissipation; // offset 16
        float foamGenerationThreshold; // offset 20
        float foamGenerationAmount; // offset 24
        float pad0; // offset 28
    } gradientConstants = {
        m_params.textureSize,
        m_params.physicalSize,
        m_deltaTime,
        m_params.foamFalloffSpeed,
        m_params.foamDissipationSpeed,
        m_params.foamGenerationThreshold,
        m_params.foamGenerationAmount * 50.0f,
        0.0f,
    };
    m_commandList->writeBuffer(
        m_gradientConstantBuffer, &gradientConstants, sizeof(gradientConstants));

    const uint32_t groups = m_params.textureSize >> 4u;
    dispatchCompute(
        m_commandList,
        m_gradientFoamPipeline,
        m_gradientFoamBindingSet,
        groups,
        groups,
        1u);
    dispatchCompute(
        m_commandList,
        m_foamInjectionPipeline,
        m_foamInjectionBindingSet,
        groups,
        groups,
        1u);

    m_commandList->endMarker();

    struct MipmapConstants
    {
        uint32_t sourceMipSize; // offset 0
        float pad0; // offset 4
        float pad1; // offset 8
        float pad2; // offset 12
    } mipmapConstants = {
        m_params.textureSize,
        0.0f,
        0.0f,
        0.0f,
    };
    m_commandList->writeBuffer(
        m_mipmapConstantBuffer, &mipmapConstants, sizeof(mipmapConstants));

    m_commandList->beginMarker("WaveWorks: Generating mipmaps for textures");
    dispatchCompute(
        m_commandList,
        m_mipmapPipeline,
        m_displacementMipmapBindingSet0,
        groups,
        groups,
        4u);
    dispatchCompute(
        m_commandList,
        m_mipmapPipeline,
        m_gradientMipmapBindingSet0,
        groups,
        groups,
        4u);

    mipmapConstants.sourceMipSize = m_params.textureSize >> 5u;
    m_commandList->writeBuffer(
        m_mipmapConstantBuffer, &mipmapConstants, sizeof(mipmapConstants));

    uint32_t coarseGroups = m_params.textureSize >> 9u;
    if (coarseGroups == 0)
        coarseGroups = 1;

    dispatchCompute(
        m_commandList,
        m_mipmapPipeline,
        m_displacementMipmapBindingSet1,
        coarseGroups,
        coarseGroups,
        4u);
    dispatchCompute(
        m_commandList,
        m_mipmapPipeline,
        m_gradientMipmapBindingSet1,
        coarseGroups,
        coarseGroups,
        4u);

    m_commandList->endMarker();
    m_commandList->endMarker();

    m_displacementMapVersion = m_simulation->getDisplacementMapVersion();

    m_commandList->setTextureState(displacementTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    m_commandList->commitBarriers();

    if (m_params.enableGpuTimers && m_timerQueries[timerSlot])
        m_commandList->endTimerQuery(m_timerQueries[timerSlot]);

    return true;
}

WWRESULT Local_Waves_Simulation::waitForAllInFlightTimers()
{
    if (!m_enableGraphics)
        return WWE_OK;

    for (uint32_t next = (m_timerReadIndex + 1) % array_dim(m_timerQueries);
         next != m_timerWriteIndex;
         next = (next + 1) % array_dim(m_timerQueries))
    {
        m_device->getTimerQueryTime(m_timerQueries[next]);
    }

    return WWE_OK;
}

WWRESULT Local_Waves_Simulation::waitStagingCursor()
{
    if (!m_simulation)
        return WWE_FAIL;

    const WWRESULT result = m_simulation->waitStagingCursor();
    if (result == WWE_OK || result == WWE_NONE)
        return result;
    return WWE_FAIL;
}

void Local_Waves_Simulation::MessageCallback::message(
    nvrhi::MessageSeverity severity, const char* text) noexcept
{
    std::fprintf(stderr, "WaveWorks NVRHI [%d]: %s\n", int(severity), text);
}

void Local_Waves_Simulation::allocateThreadPool(
    GFSDK_WaveWorks_Simulation_CPU_Threading_Model threadingModel)
{
    if (m_threadPool)
    {
        delete m_threadPool;
        m_threadPool = nullptr;
    }

    m_threadPool = new ThreadPool(static_cast<unsigned int>(threadingModel));
}

void Local_Waves_Simulation::freeDisturbanceBuffers()
{
    for (float2*& buffer : m_disturbanceBuffers)
    {
        if (buffer)
        {
            NVSDK_aligned_free(buffer);
            buffer = nullptr;
        }
    }
}

void Local_Waves_Simulation::resetTimerRing()
{
    m_timerReadIndex = 0;
    m_timerWriteIndex = 1;
    for (float& result : m_timerResultsMs)
        result = 0.0f;
}
