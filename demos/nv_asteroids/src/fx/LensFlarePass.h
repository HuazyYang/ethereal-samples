#pragma once

#include <donut/core/math/math.h>
#include <nvrhi/nvrhi.h>

#include <map>
#include <memory>

class SceneDirectionalLight;

namespace donut::engine
{
    class ShaderFactory;
    class CommonRenderPasses;
    class FramebufferFactory;
    class ICompositeView;
}

namespace fx
{
    // Asteroids.exe: LensFlare pass (ctor 0x1400637E0, Render 0x140064170 "LensFlare"; no vtable).
    // FeatureDemo member +936, created in CreateRenderPasses (0x14002BD60), rendered in PostProcess
    // (0x140033960) when UIData+233 is set.
    //
    // Full-screen additive pass (demo/lensflare.hlsl): the vertex shader samples the sun shadow at
    // the camera position (cascades + per-object shadows) and collapses the quad when the sun is
    // occluded; the pixel shader draws the procedural flare around the projected sun position.
    class LensFlarePass
    {
    public:
        LensFlarePass(
            nvrhi::IDevice* device,
            const std::shared_ptr<donut::engine::ShaderFactory>& shaderFactory,
            const std::shared_ptr<donut::engine::CommonRenderPasses>& commonPasses,
            const std::shared_ptr<donut::engine::FramebufferFactory>& framebufferFactory,
            const donut::engine::ICompositeView& compositeView);

        // cameraPosition: FeatureDemo +640 (camera-relative rendering offset, = -view translation).
        void Render(
            nvrhi::ICommandList* commandList,
            const donut::engine::ICompositeView& compositeView,
            const std::shared_ptr<donut::engine::FramebufferFactory>& framebufferFactory,
            const SceneDirectionalLight& sun,
            const donut::math::float3& cameraPosition);

    private:
        nvrhi::DeviceHandle m_Device;
        nvrhi::ShaderHandle m_VertexShader;
        nvrhi::ShaderHandle m_PixelShader;
        nvrhi::BufferHandle m_LensFlareConstants;
        nvrhi::SamplerHandle m_ShadowSampler;
        nvrhi::BindingLayoutHandle m_BindingLayout;
        nvrhi::GraphicsPipelineHandle m_Pipeline;
        std::map<nvrhi::ITexture*, nvrhi::BindingSetHandle> m_BindingSets;   // keyed by shadow map texture
        std::shared_ptr<donut::engine::CommonRenderPasses> m_CommonPasses;
    };
}
