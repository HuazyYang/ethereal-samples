#pragma once

#include <donut/core/math/math.h>
#include <nvrhi/nvrhi.h>

#include <memory>
#include <unordered_map>

namespace donut::engine
{
    class ShaderFactory;
    class CommonRenderPasses;
}

// Asteroids.exe: 2018 CommonRenderPasses::BlitWithSharpening (0x14007AAB0 -> binding-set cache 0x14007B480,
// sharpen blit 0x14007A5B0). Used for the final blit with UIData::sharpness.
// deviation: donut main's CommonRenderPasses has a sharpen shader but no way to set its factor, and its
// BlitConstants layout differs, so the 2018 pass is reconstructed here. It reuses donut's rect_vs, whose
// constants match the first 32 bytes of the 2018 layout, together with the reconstructed 2018 sharpen_ps.
class SharpenBlitPass2018
{
public:
    SharpenBlitPass2018(nvrhi::IDevice* device, nvrhi::AutoPtr<donut::engine::ShaderFactory> shaderFactory,
        nvrhi::AutoPtr<donut::engine::CommonRenderPasses> commonPasses);

    // Draws 'source' over the whole viewport of 'framebuffer' (source and target boxes are [0,1]^2).
    void Render(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer, const nvrhi::Viewport& viewport,
        nvrhi::ITexture* source, float sharpenFactor);

    // Asteroids.exe: part of CommonRenderPasses::ResetBindingCache (0x14007B6A0), called when the render
    // targets are recreated.
    void ResetCaches() { m_BindingSets.clear(); }

private:
    nvrhi::DeviceHandle m_Device;
    nvrhi::AutoPtr<donut::engine::CommonRenderPasses> m_CommonPasses;
    nvrhi::ShaderHandle m_PixelShader;
    nvrhi::BufferHandle m_Constants;
    nvrhi::BindingLayoutHandle m_BindingLayout;
    // The 2018 pipeline cache was keyed by framebuffer; the pipeline only depends on its formats, so it is
    // keyed by FramebufferInfo here (no framebuffer references are held across swap-chain resizes).
    nvrhi::GraphicsPipelineHandle m_Pipeline;
    nvrhi::FramebufferInfo m_PipelineFramebufferInfo;
    // 2018: binding sets keyed by (texture, slice, mip); cleared with the render targets (ResetCaches).
    struct BindingEntry { nvrhi::TextureHandle texture; nvrhi::BindingSetHandle bindingSet; };
    std::unordered_map<nvrhi::ITexture*, BindingEntry> m_BindingSets;
};
