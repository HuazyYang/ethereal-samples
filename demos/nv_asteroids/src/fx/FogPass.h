#pragma once

#include <donut/core/math/math.h>
#include <donut/engine/BindingCache.h>
#include <nvrhi/nvrhi.h>

#include <memory>

namespace donut::engine
{
    class ShaderFactory;
    class CommonRenderPasses;
    class FramebufferFactory;
    class ICompositeView;
}

class SceneDirectionalLight;

namespace fx
{
    // UIData+236 (36 bytes), consumed by FogPass::Render. Defaults from the UIData constructor (0x1400246A0).
    // Names follow the FogConstants fields they feed (no UI labels exist for them).
    struct FogParameters
    {
        float meanHeight = -3000.f;             // world height of the fog layer center (camera offset is added)
        float thickness = 4000.f;               // gaussian sigma of the layer
        float densityScale = 0.0005f;
        float lightIntensityScale = 1.f;        // multiplies light.radiance
        float visibilityThreshold = 0.0001f;    // ray marching stops below this transparency
        donut::math::float3 color = donut::math::float3(0.667f, 0.742f, 1.f);
        float intensity = 1.5f;
    };
    static_assert(sizeof(FogParameters) == 36, "UIData fog parameter block");

    // Asteroids.exe: Fog pass (ctor 0x140019CE0, Render 0x14001AE60; object size 0xC8, no vtable)
    //
    // Half-resolution volumetric height fog: demo/fog.hlsl ps_trace renders into an internal
    // half-size RGBA16F target, ps_filter upsamples it depth-aware and blends it (premultiplied)
    // over the framebuffer that the framebuffer factory gives for the view.
    class FogPass
    {
    public:
        // depthBuffer: the full-resolution scene depth (RenderTargets[0]); the trace target is
        // ((width+1)/2, (height+1)/2) of it. The pass is recreated with the render targets.
        FogPass(
            nvrhi::IDevice* device,
            const nvrhi::AutoPtr<donut::engine::ShaderFactory>& shaderFactory,
            const nvrhi::AutoPtr<donut::engine::CommonRenderPasses>& commonPasses,
            const nvrhi::AutoPtr<donut::engine::FramebufferFactory>& framebufferFactory,
            nvrhi::ITexture* depthBuffer,
            const donut::engine::ICompositeView& compositeView);

        // cameraOffset: FeatureDemo +640 (camera-relative rendering offset, = -camera position).
        // randomOffset: three MT random numbers * 32767 (dither pattern offset and temporal jitter).
        // maxShadowDistance: UIData+196.
        void Render(
            nvrhi::ICommandList* commandList,
            const donut::engine::ICompositeView& compositeView,
            const donut::math::float3& cameraOffset,
            const SceneDirectionalLight& light,
            const FogParameters& params,
            const donut::math::float3& randomOffset,
            float maxShadowDistance);

    private:
        nvrhi::DeviceHandle m_Device;
        nvrhi::ShaderHandle m_TracePixelShader;
        nvrhi::ShaderHandle m_FilterPixelShader;
        nvrhi::BufferHandle m_FogConstants;
        nvrhi::TextureHandle m_DepthBuffer;
        nvrhi::TextureHandle m_UnfilteredFog;
        nvrhi::SamplerHandle m_ShadowSampler;
        nvrhi::BindingLayoutHandle m_TraceBindingLayout;
        nvrhi::BindingLayoutHandle m_FilterBindingLayout;
        nvrhi::BindingSetHandle m_FilterBindingSet;
        nvrhi::FramebufferHandle m_TraceFramebuffer;
        nvrhi::GraphicsPipelineHandle m_TracePipeline;
        nvrhi::GraphicsPipelineHandle m_FilterPipeline;
        donut::engine::BindingCache m_TraceBindingSets;     // 2018: unordered_map<ITexture* shadowMap, BindingSet>
        nvrhi::AutoPtr<donut::engine::CommonRenderPasses> m_CommonPasses;
        nvrhi::AutoPtr<donut::engine::FramebufferFactory> m_FramebufferFactory;
    };
}
