#pragma once

#include <donut/core/math/math.h>
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
    // Asteroids.exe: ShieldPass (ctor 0x14006A2B0, Render 0x14006AB40; no vtable, 0x38-byte object)
    //
    // Full-screen pass that draws the player ship's energy shield: shield_ps.hlsl reconstructs the
    // camera-relative position of each depth sample, intersects the view ray with the shield sphere
    // and blends the bubble (premultiplied alpha) over the HDR target. Vertex shader:
    // CommonRenderPasses::m_FullscreenVS (4-vertex strip with SV_Position + UV).
    class ShieldPass
    {
    public:
        // depthBuffer: the scene depth (RenderTargets[0]), bound as t_Depth.
        ShieldPass(
            nvrhi::IDevice* device,
            const nvrhi::AutoPtr<donut::engine::ShaderFactory>& shaderFactory,
            const nvrhi::AutoPtr<donut::engine::CommonRenderPasses>& commonPasses,
            const nvrhi::AutoPtr<donut::engine::FramebufferFactory>& framebufferFactory,
            const donut::engine::ICompositeView& compositeView,
            nvrhi::ITexture* depthBuffer);

        // shipPosition: shield center relative to the camera (camera-relative world space).
        // shieldDirection: world direction the shield is concentrated towards (lit side of the bubble).
        // shieldRadius: sphere radius (FeatureDemo passes 30.0f).
        // shieldIntensity: shield strength; nothing is drawn when it is <= 0.
        // time: animation time in seconds.
        void Render(
            nvrhi::ICommandList* commandList,
            const donut::engine::ICompositeView& compositeView,
            const donut::math::float3& shipPosition,
            const donut::math::float3& shieldDirection,
            float shieldRadius,
            float shieldIntensity,
            float time) const;

    private:
        nvrhi::GraphicsPipelineHandle m_Pipeline;
        nvrhi::BufferHandle m_ShieldConstants;
        nvrhi::ShaderHandle m_PixelShader;
        nvrhi::BindingLayoutHandle m_BindingLayout;
        nvrhi::BindingSetHandle m_BindingSet;
        nvrhi::AutoPtr<donut::engine::FramebufferFactory> m_FramebufferFactory;
    };
}
