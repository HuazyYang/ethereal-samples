#pragma once

#include <donut/core/math/math.h>
#include <nvrhi/nvrhi.h>

#include <memory>

namespace donut::engine
{
    class ShaderFactory;
    class FramebufferFactory;
    class ICompositeView;
    class IView;
}

namespace fx
{
    // Asteroids.exe: RectPass (ctor 0x14000A500, Render 0x14000B330; no vtable)
    //
    // Draws a camera-facing quad that covers a given angular size around a world direction, at the
    // far plane, with a caller-supplied pixel shader. The vertex shader (demo/RectPass_vs.hlsl)
    // gets the four clip-space corners and their world directions in RectConstants (b0).
    // Used by the sun disk and the planets.
    //
    // deviation: the 2018 nvrhi used one binding layout with per-stage item lists (VS + PS) and one
    // binding set. nvrhi main layouts have a single visibility, and both stages use b0, so the
    // vertex-stage constants get their own layout/set owned by the RectPass, and the owner passes a
    // pixel-stage layout description and its own binding set.
    class RectPass
    {
    public:
        RectPass(
            nvrhi::IDevice* device,
            const nvrhi::AutoPtr<donut::engine::ShaderFactory>& shaderFactory,
            const char* pixelShaderFile,
            const nvrhi::BindingLayoutDesc& pixelBindingLayoutDesc,
            const nvrhi::BlendState::RenderTarget& blendState,
            const nvrhi::AutoPtr<donut::engine::FramebufferFactory>& framebufferFactory,
            const donut::engine::ICompositeView& compositeView);

        // direction: world-space direction of the quad center (need not be normalized).
        // angularSize: full angular size of the quad, degrees.
        // distance: distance of the quad from the camera (scaled by min(cos(angularSize/2), 0.9)).
        void Render(
            nvrhi::ICommandList* commandList,
            const donut::engine::IView& view,
            const donut::math::float3& direction,
            float angularSize,
            float distance,
            nvrhi::IBindingSet* pixelBindingSet) const;

        [[nodiscard]] nvrhi::IBindingLayout* GetPixelBindingLayout() const { return m_PixelBindingLayout; }

    private:
        nvrhi::ShaderHandle m_PixelShader;
        nvrhi::ShaderHandle m_VertexShader;
        nvrhi::BufferHandle m_RectConstants;
        nvrhi::GraphicsPipelineHandle m_Pipeline;
        nvrhi::BindingLayoutHandle m_VertexBindingLayout;
        nvrhi::BindingSetHandle m_VertexBindingSet;
        nvrhi::BindingLayoutHandle m_PixelBindingLayout;
        nvrhi::AutoPtr<donut::engine::FramebufferFactory> m_FramebufferFactory;
    };
}
