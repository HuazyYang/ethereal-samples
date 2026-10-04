#pragma once

#include <nvrhi/nvrhi.h>

#include <memory>
#include <unordered_map>

namespace donut::engine
{
    class ShaderFactory;
    class CommonRenderPasses;
}

namespace fx
{
    // Asteroids.exe: HQ blit / upscale pass (ctor 0x14006AF60, Render 0x14006B630 "Upscale"; no vtable).
    // FeatureDemo member +944, created in CreateRenderPasses (0x14002BD60) for the output framebuffer,
    // used by RenderScene (0x1400342A0) when UIData aaMode (+100) == 3: the scene is rendered at
    // 1/1.5 resolution and upscaled to the back buffer with a 13-tap Lanczos-3 filter
    // (demo/hq_blit_ps.hlsl) on a full-screen triangle strip.
    class HqBlitPass
    {
    public:
        HqBlitPass(
            nvrhi::IDevice* device,
            const std::shared_ptr<donut::engine::ShaderFactory>& shaderFactory,
            const std::shared_ptr<donut::engine::CommonRenderPasses>& commonPasses,
            nvrhi::IFramebuffer* framebuffer);

        // viewport: destination viewport in the framebuffer (the scissor is its floor/ceil).
        void Render(
            nvrhi::ICommandList* commandList,
            nvrhi::IFramebuffer* framebuffer,
            const nvrhi::Viewport& viewport,
            nvrhi::ITexture* sourceTexture);

    private:
        nvrhi::DeviceHandle m_Device;
        nvrhi::ShaderHandle m_PixelShader;
        nvrhi::BindingLayoutHandle m_BindingLayout;
        nvrhi::GraphicsPipelineHandle m_Pipeline;
        std::shared_ptr<donut::engine::CommonRenderPasses> m_CommonPasses;
        std::unordered_map<nvrhi::ITexture*, nvrhi::BindingSetHandle> m_BindingSets;   // keyed by source texture
    };
}
