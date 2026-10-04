#pragma once

#include <nvrhi/nvrhi.h>

#include <memory>

namespace donut::engine
{
    class ShaderFactory;
    class CommonRenderPasses;
    class FramebufferFactory;
    class ICompositeView;
}

namespace fx
{
    // Asteroids.exe: EnvironmentMapPass, the demo's variant of donut's EnvironmentMapPass
    // (ctor 0x140064D10, Render 0x1400658B0 "Environment Map"; no vtable). FeatureDemo member +800.
    //
    // Full-screen sky pass at the far plane with demo/texturedstarfield_ps.hlsl: the space-sky cube
    // map (nebula) plus an optional tiling star texture. The 2018 constant buffer is still named
    // "DeferredLightingConstants" (copied from the deferred-lighting pass); its content is
    // StarFieldConstants. FeatureDemo creates the pass only when the sky texture
    // (SkyAndStars/loc00180_8_Space_Sky_2k-ExposureCorrected-16bit.dds, FeatureDemo +1040) has loaded,
    // and always passes starMap = nullptr, so the star layer is disabled in the shipped demo.
    class EnvironmentMapPass
    {
    public:
        EnvironmentMapPass(
            nvrhi::IDevice* device,
            const nvrhi::AutoPtr<donut::engine::ShaderFactory>& shaderFactory,
            const nvrhi::AutoPtr<donut::engine::CommonRenderPasses>& commonPasses,
            const nvrhi::AutoPtr<donut::engine::FramebufferFactory>& framebufferFactory,
            const donut::engine::ICompositeView& compositeView,
            nvrhi::ITexture* environmentMap,
            nvrhi::ITexture* starMap);

        // FeatureDemo::RenderLightingAndEffects: Render(cmd, view, UIData+148, 0, 0).
        // The star brightness values only take effect when a star map was given to the constructor.
        void Render(
            nvrhi::ICommandList* commandList,
            const donut::engine::ICompositeView& compositeView,
            float nebulaLinearBrightness,
            float starLinearBrightness,
            float starSquareBrightness) const;

    private:
        nvrhi::ShaderHandle m_PixelShader;
        nvrhi::BufferHandle m_StarFieldCB;
        nvrhi::BindingLayoutHandle m_BindingLayout;
        nvrhi::BindingSetHandle m_BindingSet;
        nvrhi::GraphicsPipelineHandle m_Pipeline;
        nvrhi::AutoPtr<donut::engine::CommonRenderPasses> m_CommonPasses;
        nvrhi::AutoPtr<donut::engine::FramebufferFactory> m_FramebufferFactory;
        bool m_HasStarMap = false;
    };
}
