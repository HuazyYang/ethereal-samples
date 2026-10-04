#pragma once

#include <nvrhi/nvrhi.h>

#include <memory>

namespace donut::engine
{
    class ShaderFactory;
    class CommonRenderPasses;
}

namespace fx
{
    // Asteroids.exe: ShowCubemap pass (ctor 0x14004C750, Render 0x14004CFC0; object size 0x30, no vtable)
    //
    // Debug overlay: draws one cube map array (a light probe's specular cubemap) as a lat-long image
    // into a viewport rectangle (demo/show_cubemap_ps.hlsl with donut's full-screen VS).
    // FeatureDemo::RenderOverlay (0x1400321F0) creates it lazily on first use.
    class ShowCubemapPass
    {
    public:
        // framebuffer: the overlay framebuffer the pipeline is created for (and rendered into).
        // cubemap: TextureCubeArray to display (bound once; the 2018 code never rebinds it).
        ShowCubemapPass(
            nvrhi::IDevice* device,
            const std::shared_ptr<donut::engine::ShaderFactory>& shaderFactory,
            const std::shared_ptr<donut::engine::CommonRenderPasses>& commonPasses,
            nvrhi::IFramebuffer* framebuffer,
            nvrhi::ITexture* cubemap);

        // viewport: FeatureDemo passes (10, 266, height - 266, height - 10, 0, 1).
        void Render(
            nvrhi::ICommandList* commandList,
            nvrhi::IFramebuffer* framebuffer,
            const nvrhi::Viewport& viewport,
            uint32_t arrayIndex,
            uint32_t mipLevel);

    private:
        nvrhi::ShaderHandle m_PixelShader;
        nvrhi::SamplerHandle m_Sampler;
        nvrhi::GraphicsPipelineHandle m_Pipeline;
        nvrhi::BindingLayoutHandle m_BindingLayout;
        nvrhi::BindingSetHandle m_BindingSet;
        nvrhi::BufferHandle m_Constants;
    };
}
