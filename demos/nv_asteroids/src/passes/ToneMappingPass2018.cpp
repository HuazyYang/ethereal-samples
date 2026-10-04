#include "passes/ToneMappingPass2018.h"
#include "app/GpuProfiler.h"

#include "passes/Framework2018Constants.h"

#include <donut/core/log.h>
#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/FramebufferFactory.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/View.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <string>

using namespace donut;
using namespace donut::math;
using namespace donut::engine;

namespace
{
    // 0x140093DC0 / 0x140094A50: scale 1/14 and bias 10/14 for the histogram, scale 14 and bias -10 for the
    // exposure (log2 luminance range [-10, 4]).
    constexpr float c_MinLogLuminance = -10.f;
    constexpr float c_MaxLogLuminance = 4.f;
}

ToneMappingPass2018::ToneMappingPass2018(
    nvrhi::IDevice* device,
    std::shared_ptr<ShaderFactory> shaderFactory,
    std::shared_ptr<CommonRenderPasses> commonPasses,
    std::shared_ptr<FramebufferFactory> framebufferFactory,
    const ICompositeView& compositeView,
    const CreateParameters& params)
    : m_Device(device)
    , m_HistogramBins(params.histogramBins)
    , m_CommonPasses(std::move(commonPasses))
    , m_FramebufferFactory(std::move(framebufferFactory))
{
    assert(params.histogramBins <= 256);

    const IView* sampleView = compositeView.GetChildView(ViewType::PLANAR, 0);
    nvrhi::IFramebuffer* sampleFramebuffer = m_FramebufferFactory->GetFramebuffer(*sampleView);

    {
        const std::vector<ShaderMacro> macros = {
            ShaderMacro("HISTOGRAM_BINS", std::to_string(params.histogramBins)),
            ShaderMacro("SOURCE_ARRAY", params.isTextureArray ? "1" : "0"),
        };

        m_HistogramComputeShader = shaderFactory->CreateShader("framework/passes/histogram_cs.hlsl", "main", &macros,
            nvrhi::ShaderType::Compute);
        m_ExposureComputeShader = shaderFactory->CreateShader("framework/passes/exposure_cs.hlsl", "main", &macros,
            nvrhi::ShaderType::Compute);
        m_PixelShader = shaderFactory->CreateShader("framework/passes/tonemapping_ps.hlsl", "main", &macros,
            nvrhi::ShaderType::Pixel);
    }

    nvrhi::BufferDesc constantBufferDesc;
    constantBufferDesc.byteSize = sizeof(framework2018::ToneMappingConstants);
    constantBufferDesc.debugName = "ToneMappingConstants";
    constantBufferDesc.isConstantBuffer = true;
    constantBufferDesc.isVolatile = true;
    constantBufferDesc.maxVersions = std::max(params.numConstantBufferVersions, 16u);
    m_ToneMappingCB = device->createBuffer(constantBufferDesc);

    nvrhi::BufferDesc storageBufferDesc;
    storageBufferDesc.byteSize = sizeof(uint32_t) * m_HistogramBins;
    storageBufferDesc.format = nvrhi::Format::R32_UINT;
    storageBufferDesc.canHaveUAVs = true;
    storageBufferDesc.canHaveTypedViews = true;
    storageBufferDesc.debugName = "HistogramBuffer";
    storageBufferDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    storageBufferDesc.keepInitialState = true;
    m_HistogramBuffer = device->createBuffer(storageBufferDesc);

    if (params.exposureBufferOverride)
    {
        m_ExposureBuffer = params.exposureBufferOverride;
    }
    else
    {
        storageBufferDesc.byteSize = sizeof(uint32_t);
        storageBufferDesc.debugName = "ExposureBuffer";
        m_ExposureBuffer = device->createBuffer(storageBufferDesc);
    }

    m_ColorLUT = m_CommonPasses->m_BlackTexture;
    if (params.colorLUT)
    {
        const nvrhi::TextureDesc& desc = params.colorLUT->getDesc();
        m_ColorLUTSize = float(desc.height);

        if (desc.width != desc.height * desc.height)
        {
            log::error("Color LUT texture size must be: width = (n*n), height = (n)");
            m_ColorLUTSize = 0.f;
        }
        else
        {
            m_ColorLUT = params.colorLUT;
        }
    }

    {
        nvrhi::BindingLayoutDesc layoutDesc;
        layoutDesc.visibility = nvrhi::ShaderType::Compute;
        layoutDesc.bindings = {
            nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
            nvrhi::BindingLayoutItem::Texture_SRV(0),
            nvrhi::BindingLayoutItem::TypedBuffer_UAV(0),
        };
        m_HistogramBindingLayout = device->createBindingLayout(layoutDesc);

        nvrhi::ComputePipelineDesc pipelineDesc;
        pipelineDesc.CS = m_HistogramComputeShader;
        pipelineDesc.bindingLayouts = { m_HistogramBindingLayout };
        m_HistogramPso = device->createComputePipeline(pipelineDesc);
    }

    {
        nvrhi::BindingLayoutDesc layoutDesc;
        layoutDesc.visibility = nvrhi::ShaderType::Compute;
        layoutDesc.bindings = {
            nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
            nvrhi::BindingLayoutItem::TypedBuffer_SRV(0),
            nvrhi::BindingLayoutItem::TypedBuffer_UAV(0),
        };
        m_ExposureBindingLayout = device->createBindingLayout(layoutDesc);

        nvrhi::BindingSetDesc setDesc;
        setDesc.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, m_ToneMappingCB),
            nvrhi::BindingSetItem::TypedBuffer_SRV(0, m_HistogramBuffer),
            nvrhi::BindingSetItem::TypedBuffer_UAV(0, m_ExposureBuffer),
        };
        m_ExposureBindingSet = device->createBindingSet(setDesc, m_ExposureBindingLayout);

        nvrhi::ComputePipelineDesc pipelineDesc;
        pipelineDesc.CS = m_ExposureComputeShader;
        pipelineDesc.bindingLayouts = { m_ExposureBindingLayout };
        m_ExposurePso = device->createComputePipeline(pipelineDesc);
    }

    {
        nvrhi::BindingLayoutDesc layoutDesc;
        layoutDesc.visibility = nvrhi::ShaderType::Pixel;
        layoutDesc.bindings = {
            nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
            nvrhi::BindingLayoutItem::Texture_SRV(0),
            nvrhi::BindingLayoutItem::TypedBuffer_SRV(1),
            nvrhi::BindingLayoutItem::Texture_SRV(2),
            nvrhi::BindingLayoutItem::Sampler(0),
        };
        m_RenderBindingLayout = device->createBindingLayout(layoutDesc);

        nvrhi::GraphicsPipelineDesc pipelineDesc;
        pipelineDesc.primType = nvrhi::PrimitiveType::TriangleStrip;
        pipelineDesc.VS = m_CommonPasses->m_FullscreenVS;
        pipelineDesc.PS = m_PixelShader;
        pipelineDesc.bindingLayouts = { m_RenderBindingLayout };
        pipelineDesc.renderState.rasterState.setCullNone();
        pipelineDesc.renderState.depthStencilState.depthTestEnable = false;
        pipelineDesc.renderState.depthStencilState.stencilEnable = false;
        m_RenderPso = device->createGraphicsPipeline(pipelineDesc, sampleFramebuffer);
    }
}

void ToneMappingPass2018::Render(nvrhi::ICommandList* commandList, const render::ToneMappingParameters& params,
    const ICompositeView& compositeView, nvrhi::ITexture* sourceTexture)
{
    // 0x1400942C0
    nvrhi::BindingSetHandle& bindingSet = m_RenderBindingSets[sourceTexture];
    if (!bindingSet)
    {
        nvrhi::BindingSetDesc setDesc;
        setDesc.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, m_ToneMappingCB),
            nvrhi::BindingSetItem::Texture_SRV(0, sourceTexture),
            nvrhi::BindingSetItem::TypedBuffer_SRV(1, m_ExposureBuffer),
            nvrhi::BindingSetItem::Texture_SRV(2, m_ColorLUT),
            nvrhi::BindingSetItem::Sampler(0, m_CommonPasses->m_LinearClampSampler),
        };
        bindingSet = m_Device->createBindingSet(setDesc, m_RenderBindingLayout);
    }

    for (uint32_t viewIndex = 0; viewIndex < compositeView.GetNumChildViews(ViewType::PLANAR); viewIndex++)
    {
        const IView* view = compositeView.GetChildView(ViewType::PLANAR, viewIndex);

        nvrhi::GraphicsState state;
        state.pipeline = m_RenderPso;
        state.framebuffer = m_FramebufferFactory->GetFramebuffer(*view);
        state.bindings = { bindingSet };
        state.viewport = view->GetViewportState();

        const bool enableColorLUT = params.enableColorLUT && m_ColorLUTSize > 0.f;

        framework2018::ToneMappingConstants constants = {};
        constants.exposureScale = std::exp2(params.exposureBias);
        constants.whitePointInvSquared = 1.f / std::pow(params.whitePoint, 2.f);
        constants.minAdaptedLuminance = params.minAdaptedLuminance;
        constants.maxAdaptedLuminance = params.maxAdaptedLuminance;
        constants.sourceSlice = view->GetSubresources().baseArraySlice;
        constants.colorLUTTextureSize = enableColorLUT
            ? float2(m_ColorLUTSize * m_ColorLUTSize, m_ColorLUTSize) : float2(0.f);
        constants.colorLUTTextureSizeInv = enableColorLUT ? 1.f / constants.colorLUTTextureSize : float2(0.f);
        commandList->writeBuffer(m_ToneMappingCB, &constants, sizeof(constants));

        commandList->setGraphicsState(state);

        nvrhi::DrawArguments args;
        args.vertexCount = 4;
        args.instanceCount = 1;
        commandList->draw(args);
    }
}

void ToneMappingPass2018::SimpleRender(nvrhi::ICommandList* commandList, const render::ToneMappingParameters& params,
    const ICompositeView& compositeView, nvrhi::ITexture* sourceTexture)
{
    // 0x140094A50
    demo::ProfBegin(commandList, "ToneMapping");
    ResetHistogram(commandList);
    AddFrameToHistogram(commandList, compositeView, sourceTexture);
    ComputeExposure(commandList, params);
    Render(commandList, params, compositeView, sourceTexture);
    demo::ProfEnd(commandList);
}

nvrhi::BufferHandle ToneMappingPass2018::GetExposureBuffer()
{
    return m_ExposureBuffer;
}

void ToneMappingPass2018::AdvanceFrame(float frameTime)
{
    m_FrameTime = frameTime;
}

void ToneMappingPass2018::ResetExposure(nvrhi::ICommandList* commandList, float initialExposure)
{
    // 0x1400949D0: the float's bits as a UINT clear.
    uint32_t bits = 0;
    std::memcpy(&bits, &initialExposure, sizeof(bits));
    commandList->clearBufferUInt(m_ExposureBuffer, bits);
}

void ToneMappingPass2018::ResetHistogram(nvrhi::ICommandList* commandList)
{
    commandList->clearBufferUInt(m_HistogramBuffer, 0);
}

void ToneMappingPass2018::AddFrameToHistogram(nvrhi::ICommandList* commandList, const ICompositeView& compositeView,
    nvrhi::ITexture* sourceTexture)
{
    // 0x140093DC0
    nvrhi::BindingSetHandle& bindingSet = m_HistogramBindingSets[sourceTexture];
    if (!bindingSet)
    {
        nvrhi::BindingSetDesc setDesc;
        setDesc.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, m_ToneMappingCB),
            nvrhi::BindingSetItem::Texture_SRV(0, sourceTexture),
            nvrhi::BindingSetItem::TypedBuffer_UAV(0, m_HistogramBuffer),
        };
        bindingSet = m_Device->createBindingSet(setDesc, m_HistogramBindingLayout);
    }

    for (uint32_t viewIndex = 0; viewIndex < compositeView.GetNumChildViews(ViewType::PLANAR); viewIndex++)
    {
        const IView* view = compositeView.GetChildView(ViewType::PLANAR, viewIndex);
        const nvrhi::ViewportState viewportState = view->GetViewportState();

        for (uint32_t viewportIndex = 0; viewportIndex < viewportState.scissorRects.size(); viewportIndex++)
        {
            framework2018::ToneMappingConstants constants = {};
            constants.logLuminanceScale = 1.f / (c_MaxLogLuminance - c_MinLogLuminance);
            constants.logLuminanceBias = -c_MinLogLuminance * constants.logLuminanceScale;

            const nvrhi::Rect& scissor = viewportState.scissorRects[viewportIndex];
            constants.viewOrigin = uint2(uint32_t(scissor.minX), uint32_t(scissor.minY));
            constants.viewSize = uint2(uint32_t(scissor.maxX - scissor.minX), uint32_t(scissor.maxY - scissor.minY));
            constants.sourceSlice = view->GetSubresources().baseArraySlice;
            commandList->writeBuffer(m_ToneMappingCB, &constants, sizeof(constants));

            nvrhi::ComputeState state;
            state.pipeline = m_HistogramPso;
            state.bindings = { bindingSet };
            commandList->setComputeState(state);

            const uint2 numGroups = (constants.viewSize + uint2(15)) / uint2(16);
            commandList->dispatch(numGroups.x, numGroups.y, 1);
        }
    }
}

void ToneMappingPass2018::ComputeExposure(nvrhi::ICommandList* commandList, const render::ToneMappingParameters& params)
{
    // inlined into 0x140094A50
    framework2018::ToneMappingConstants constants = {};
    constants.logLuminanceScale = c_MaxLogLuminance - c_MinLogLuminance;
    constants.logLuminanceBias = c_MinLogLuminance;
    constants.histogramLowPercentile = std::min(0.99f, std::max(0.f, params.histogramLowPercentile));
    constants.histogramHighPercentile = std::min(1.f,
        std::max(constants.histogramLowPercentile, params.histogramHighPercentile));
    constants.eyeAdaptationSpeedUp = params.eyeAdaptationSpeedUp;
    constants.eyeAdaptationSpeedDown = params.eyeAdaptationSpeedDown;
    constants.minAdaptedLuminance = params.minAdaptedLuminance;
    constants.maxAdaptedLuminance = params.maxAdaptedLuminance;
    constants.frameTime = m_FrameTime;
    commandList->writeBuffer(m_ToneMappingCB, &constants, sizeof(constants));

    nvrhi::ComputeState state;
    state.pipeline = m_ExposurePso;
    state.bindings = { m_ExposureBindingSet };
    commandList->setComputeState(state);
    commandList->dispatch(1, 1, 1);
}
