#pragma once

// Asteroids.exe: 2018 framework BloomPass (FeatureDemo+888; ctor 0x14008E320, Render 0x140090420, per-view data
// 0x78 bytes released by 0x140090370). FeatureDemo creates it in CreateRenderPasses (0x14002BD60) and calls
//   Render(commandList, framebufferFactory, compositeView, source, sigma = renderHeight / 1080 * UIData+116)
// from PostProcess. Markers "Bloom" > "Downscale" / "Blur" / "Apply".
//
// The 2018 pass differs from donut's in two places:
//  - the composite weight is not a Render argument: the "Apply" pipeline is built in the constructor with
//    src = BLEND_FACTOR, dest = INV_BLEND_FACTOR, srcAlpha = ZERO, destAlpha = ONE and a fixed blend factor of
//    0.05 (xmmword_14025DCB0 = {0.05, 0.05, 0.05, 0.05}), drawn with the CommonRenderPasses blit shaders;
//  - BloomConstants keeps numSamples at offset 16 (bloom_ps of 2018).
// The "Apply" draw is expressed through donut's BlitTexture (same blit shader, viewport = the view's viewport,
// full source rect), with the 2018 blend state and blend constant.

#include <donut/engine/BindingCache.h>
#include <nvrhi/nvrhi.h>

#include <memory>
#include <vector>

namespace donut::engine
{
    class ShaderFactory;
    class CommonRenderPasses;
    class FramebufferFactory;
    class ICompositeView;
}

class BloomPass2018
{
public:
    static constexpr float c_BlendFactor = 0.05f;   // xmmword_14025DCB0

    BloomPass2018(
        nvrhi::IDevice* device,
        const std::shared_ptr<donut::engine::ShaderFactory>& shaderFactory,
        std::shared_ptr<donut::engine::CommonRenderPasses> commonPasses,
        std::shared_ptr<donut::engine::FramebufferFactory> framebufferFactory,
        const donut::engine::ICompositeView& compositeView);

    void Render(
        nvrhi::ICommandList* commandList,
        const std::shared_ptr<donut::engine::FramebufferFactory>& framebufferFactory,
        const donut::engine::ICompositeView& compositeView,
        nvrhi::ITexture* sourceDestTexture,
        float sigmaInPixels);

private:
    struct PerViewData                                      // 0x78 bytes in the binary
    {
        nvrhi::GraphicsPipelineHandle bloomBlurPso;         // +0
        nvrhi::TextureHandle textureDownscale1;             // +16 "bloom src mip1"
        nvrhi::FramebufferHandle framebufferDownscale1;     // +24
        nvrhi::TextureHandle textureDownscale2;             // +32 "bloom src mip2"
        nvrhi::FramebufferHandle framebufferDownscale2;     // +40
        nvrhi::TextureHandle texturePass1Blur;              // +48 "bloom accumulation pass1"
        nvrhi::FramebufferHandle framebufferPass1Blur;      // +56
        nvrhi::TextureHandle texturePass2Blur;              // +64 "bloom accumulation pass2"
        nvrhi::FramebufferHandle framebufferPass2Blur;      // +72
        nvrhi::BindingSetHandle bloomBlurBindingSetPass1;   // +80 (H constants, downscale2)
        nvrhi::BindingSetHandle bloomBlurBindingSetPass2;   // +88 (V constants, pass1)
    };

    std::shared_ptr<donut::engine::CommonRenderPasses> m_CommonPasses;
    std::shared_ptr<donut::engine::FramebufferFactory> m_FramebufferFactory;
    nvrhi::DeviceHandle m_Device;
    std::vector<PerViewData> m_PerViewData;
    nvrhi::BufferHandle m_BloomHBlurCB;                     // "BloomConstantsH"
    nvrhi::BufferHandle m_BloomVBlurCB;                     // "BloomConstantsV"
    nvrhi::ShaderHandle m_BloomBlurPixelShader;             // framework/passes/bloom_ps.hlsl
    nvrhi::BindingLayoutHandle m_BloomBlurBindingLayout;    // b0 (volatile), s0, t0
    donut::engine::BindingCache m_BindingCache;
};
