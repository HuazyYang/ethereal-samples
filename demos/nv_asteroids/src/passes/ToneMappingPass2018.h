#pragma once

// Asteroids.exe: 2018 framework ToneMappingPass (FeatureDemo+832; ctor 0x1400925B0, created in CreateRenderPasses
// 0x14002BD60 with 256 bins, no texture array, the previous pass's exposure buffer and the color LUT).
//
//   0x1400925B0  ctor(device, shaderFactory, commonPasses, framebufferFactory, compositeView, CreateParameters)
//   0x140094290  GetExposureBuffer                     (FeatureDemo, before re-creating the pass)
//   0x140094240  BeginTrackingState(commandList)       (FeatureDemo frame start, after commandList->open)
//   0x1400949D0  ResetExposure(commandList, 0.05f)     (FeatureDemo frame start, when the exposure must be reset)
//   0x140094A50  SimpleRender(commandList, UIData+52, compositeView, source)   marker "ToneMapping":
//                  ResetHistogram, AddFrameToHistogram (0x140093DC0), ComputeExposure (inlined), Render (0x1400942C0)
//   0x140094A00  SaveTrackedState(commandList)          (FeatureDemo frame end, before commandList->close)
//   AdvanceFrame (inlined into FeatureDemo::Animate: +72 = elapsed time)
//
// BeginTrackingState / SaveTrackedState carried the histogram and exposure buffer states across command lists
// (the 2018 nvrhi beginTrackingBufferState / getBufferState pair). With nvrhi main the buffers use
// keepInitialState (UnorderedAccess), which gives the same behaviour, so both are kept only as no-ops.
//
// The C++ is the same as the first public donut (2021) ToneMappingPass; the shaders are the 2018 ones
// (framework/passes/{histogram_cs,exposure_cs,tonemapping_ps}.hlsl). Donut main's ToneMappingPass::Render does not
// run the histogram / exposure passes; the 2018 FeatureDemo calls SimpleRender, which does.

#include <donut/render/ToneMappingPasses.h> // donut::render::ToneMappingParameters (UIData+52, same layout as 2018)
#include <nvrhi/nvrhi.h>

#include <memory>
#include <unordered_map>

namespace donut::engine
{
    class ShaderFactory;
    class CommonRenderPasses;
    class FramebufferFactory;
    class ICompositeView;
}

class ToneMappingPass2018
{
public:
    struct CreateParameters
    {
        bool isTextureArray = false;
        uint32_t histogramBins = 256;
        uint32_t numConstantBufferVersions = 16;
        nvrhi::IBuffer* exposureBufferOverride = nullptr;
        nvrhi::ITexture* colorLUT = nullptr;
    };

    ToneMappingPass2018(
        nvrhi::IDevice* device,
        nvrhi::AutoPtr<donut::engine::ShaderFactory> shaderFactory,
        nvrhi::AutoPtr<donut::engine::CommonRenderPasses> commonPasses,
        nvrhi::AutoPtr<donut::engine::FramebufferFactory> framebufferFactory,
        const donut::engine::ICompositeView& compositeView,
        const CreateParameters& params);

    void Render(nvrhi::ICommandList* commandList, const donut::render::ToneMappingParameters& params,
        const donut::engine::ICompositeView& compositeView, nvrhi::ITexture* sourceTexture);

    void SimpleRender(nvrhi::ICommandList* commandList, const donut::render::ToneMappingParameters& params,
        const donut::engine::ICompositeView& compositeView, nvrhi::ITexture* sourceTexture);

    nvrhi::BufferHandle GetExposureBuffer();
    void AdvanceFrame(float frameTime);
    void ResetExposure(nvrhi::ICommandList* commandList, float initialExposure = 0.f);
    void ResetHistogram(nvrhi::ICommandList* commandList);
    void AddFrameToHistogram(nvrhi::ICommandList* commandList, const donut::engine::ICompositeView& compositeView,
        nvrhi::ITexture* sourceTexture);
    void ComputeExposure(nvrhi::ICommandList* commandList, const donut::render::ToneMappingParameters& params);

    void BeginTrackingState(nvrhi::ICommandList*) { }
    void SaveTrackedState(nvrhi::ICommandList*) { }

private:
    nvrhi::DeviceHandle m_Device;
    nvrhi::ShaderHandle m_PixelShader;
    nvrhi::ShaderHandle m_HistogramComputeShader;
    nvrhi::ShaderHandle m_ExposureComputeShader;
    uint32_t m_HistogramBins;
    nvrhi::BufferHandle m_ToneMappingCB;        // "ToneMappingConstants"
    nvrhi::BufferHandle m_HistogramBuffer;      // "HistogramBuffer"
    nvrhi::BufferHandle m_ExposureBuffer;       // "ExposureBuffer"
    float m_FrameTime = 0.f;
    nvrhi::TextureHandle m_ColorLUT;
    float m_ColorLUTSize = 0.f;

    nvrhi::BindingLayoutHandle m_HistogramBindingLayout;
    nvrhi::ComputePipelineHandle m_HistogramPso;
    nvrhi::BindingLayoutHandle m_ExposureBindingLayout;
    nvrhi::BindingSetHandle m_ExposureBindingSet;
    nvrhi::ComputePipelineHandle m_ExposurePso;
    nvrhi::BindingLayoutHandle m_RenderBindingLayout;
    nvrhi::GraphicsPipelineHandle m_RenderPso;

    nvrhi::AutoPtr<donut::engine::CommonRenderPasses> m_CommonPasses;
    nvrhi::AutoPtr<donut::engine::FramebufferFactory> m_FramebufferFactory;

    std::unordered_map<nvrhi::ITexture*, nvrhi::BindingSetHandle> m_HistogramBindingSets;
    std::unordered_map<nvrhi::ITexture*, nvrhi::BindingSetHandle> m_RenderBindingSets;
};
