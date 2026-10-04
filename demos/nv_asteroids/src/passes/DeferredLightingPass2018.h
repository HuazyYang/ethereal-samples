#pragma once

// Asteroids.exe: 2018 framework DeferredLightingPass (string "DeferredLightingConstants"; ctor 0x14007BDC0,
// Render 0x14007CA30, 0xE8 bytes). FeatureDemo creates it in CreateRenderPasses (0x14002BD60, +840) with
// deferred_lighting_ps (+672) and the G-buffer binding layout (+680), and runs it first in
// RenderLightingAndEffects (0x140033CA0).
//
// deviation: donut main's DeferredLightingPass is a compute pass with donut's G-buffer layout; the 2018 pass is a
// full-screen pixel pass (donut CommonRenderPasses fullscreen VS, 4-vertex strip) that fills the 2018
// c_Deferred (5648 bytes) read by the demo deferred_lighting_ps, so it is a demo-side class.

#include <donut/core/math/math.h>
#include <nvrhi/nvrhi.h>

#include <memory>
#include <vector>

namespace donut::engine
{
    class ShaderFactory;
    class CommonRenderPasses;
    class FramebufferFactory;
    class ICompositeView;
    class BindingCache;
    struct LightProbe;
}

class SceneLight;

class DeferredLightingPass2018
{
public:
    // FeatureDemo-side G-buffer binding layout (+680) and set (+688): t_GBufferDepth t8 = depth, t_GBuffer0 t9,
    // t_GBuffer1 t10, t_GBuffer2 t11 (render targets 0, 2, 3, 4), pixel visibility.
    static nvrhi::BindingLayoutHandle CreateGBufferBindingLayout(nvrhi::IDevice* device);
    static nvrhi::BindingSetHandle CreateGBufferBindingSet(nvrhi::IDevice* device, nvrhi::IBindingLayout* layout,
        nvrhi::ITexture* depth, nvrhi::ITexture* gbuffer0, nvrhi::ITexture* gbuffer1, nvrhi::ITexture* gbuffer2);

    // 0x14007BDC0(device, shaderFactory, commonPasses, framebufferFactory, view, pixelShader, gbufferBindingLayout).
    // FeatureDemo: (..., RenderTargets::HdrFramebufferNoDepth, main view, deferred_lighting_ps, +680).
    DeferredLightingPass2018(
        nvrhi::IDevice* device,
        nvrhi::AutoPtr<donut::engine::ShaderFactory> shaderFactory,
        nvrhi::AutoPtr<donut::engine::CommonRenderPasses> commonPasses,
        nvrhi::AutoPtr<donut::engine::FramebufferFactory> framebufferFactory,
        const donut::engine::ICompositeView& compositeView,
        nvrhi::IShader* pixelShader,
        nvrhi::IBindingLayout* gbufferBindingLayout);
    ~DeferredLightingPass2018();

    // 0x14007CA30(commandList, compositeView, lights, gbufferBindingSet, randomOffset, ambientColorTop,
    // ambientColorBottom, lightProbes, indirectDiffuse). 'indirectDiffuse' may be null (indirectDiffuseScale = 0).
    void Render(
        nvrhi::ICommandList* commandList,
        const donut::engine::ICompositeView& compositeView,
        const std::vector<std::shared_ptr<SceneLight>>& lights,
        nvrhi::IBindingSet* gbufferBindingSet,
        dm::float2 randomOffset,
        const dm::float3& ambientColorTop,
        const dm::float3& ambientColorBottom,
        const std::vector<nvrhi::AutoPtr<donut::engine::LightProbe>>& lightProbes,
        nvrhi::ITexture* indirectDiffuse = nullptr);

    void ResetBindingCache();

private:
    nvrhi::DeviceHandle m_Device;                                               // +0
    nvrhi::ShaderHandle m_PixelShader;                                          // +8
    nvrhi::SamplerHandle m_ShadowSampler;                                       // +16
    nvrhi::SamplerHandle m_ShadowSamplerComparison;                             // +24
    nvrhi::BufferHandle m_DeferredCB;                                           // +32 "DeferredLightingConstants"
    nvrhi::GraphicsPipelineHandle m_Pipeline;                                   // +40
    nvrhi::BindingLayoutHandle m_GBufferBindingLayout;                          // +48
    nvrhi::BindingLayoutHandle m_ShadowBindingLayout;                           // +56 b0, t0, s0, s1
    nvrhi::BindingLayoutHandle m_LightProbeBindingLayout;                       // +64 t1..t4, s2, s3
    std::unique_ptr<donut::engine::BindingCache> m_BindingSets;                 // +72 / +136 (per shadow map / per probe textures)
    nvrhi::AutoPtr<donut::engine::CommonRenderPasses> m_CommonPasses;          // +200
    nvrhi::AutoPtr<donut::engine::FramebufferFactory> m_FramebufferFactory;    // +216
};
