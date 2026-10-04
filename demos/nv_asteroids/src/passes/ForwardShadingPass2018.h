#pragma once

// Asteroids.exe: 2018 framework ForwardShadingPass (strings "ForwardShadingConstants", "ForwardShading";
// ctor 0x1400840B0, Render 0x140085A60, 0x110 bytes). FeatureDemo creates it in CreateRenderPasses (0x14002BD60,
// +776) and in the light probe capture (0x1400327F0) and renders the forward meshlet strategies through it in
// RenderLightingAndEffects (0x140033CA0): the asteroid forward renderer (+296) and the ship overlay (+312).
//
// deviation: donut main's ForwardShadingPass splits the 2018 monolithic c_Forward (5552 bytes, lights, shadows and
// probes in one cbuffer) into view/light buffers with a different layout, so the 2018 pass is a demo-side class
// that fills surface2018::ForwardShadingConstants for the demo forward_ps (b1, t4..t7, s1..s3).
// deviation: the 2018 constructor also built input-assembler pipelines from passes/forward_vs, forward_gs and the
// framework passes/forward_ps for regular IDrawStrategy geometry. The demo submits only meshlet strategies, so
// those pipelines (and the framework forward_ps they need) are not reconstructed; see src/passes/NOTES.md.

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

class MeshletDrawStrategy;
class SceneLight;
struct MeshletPassState;
struct SceneMaterial;

class ForwardShadingPass2018
{
public:
    // 0x1400840B0(device, shaderFactory, commonPasses, framebufferFactory, view, singlePassStereo, singlePassCubemap,
    // trackLiveness). FeatureDemo: (..., RenderTargets::HdrFramebuffer, main view, false, false, true).
    ForwardShadingPass2018(
        nvrhi::IDevice* device,
        nvrhi::AutoPtr<donut::engine::ShaderFactory> shaderFactory,
        nvrhi::AutoPtr<donut::engine::CommonRenderPasses> commonPasses,
        nvrhi::AutoPtr<donut::engine::FramebufferFactory> framebufferFactory,
        const donut::engine::ICompositeView& compositeView,
        nvrhi::IBindingLayout* materialBindingLayout,
        bool singlePassStereo = false,
        bool singlePassCubemap = false,
        bool trackLiveness = true);
    ~ForwardShadingPass2018();

    // 0x140085A60(commandList, drawStrategy, compositeView, lights, ambientColorTop, ambientColorBottom, lightProbes).
    void Render(
        nvrhi::ICommandList* commandList,
        MeshletDrawStrategy& drawStrategy,
        const donut::engine::ICompositeView& compositeView,
        const std::vector<std::shared_ptr<SceneLight>>& lights,
        const dm::float3& ambientColorTop,
        const dm::float3& ambientColorBottom,
        const std::vector<nvrhi::AutoPtr<donut::engine::LightProbe>>& lightProbes);

    void ResetBindingCache();

private:
    // The 2018 material callback (lambda 0x140086E50): opaque / alpha-tested / transparent state by material domain,
    // binds { material, forward, light probe } sets; skips materials without a binding set.
    bool SetupMaterial(SceneMaterial* material, MeshletPassState& state) const;

    nvrhi::DeviceHandle m_Device;                                               // +0
    nvrhi::SamplerHandle m_ShadowSampler;                                       // +40
    nvrhi::BindingLayoutHandle m_MaterialBindingLayout;                         // CommonRenderPasses+328 in 2018
    nvrhi::BindingLayoutHandle m_ForwardBindingLayout;                          // +48 PS section: b1 c_Forward, t4, s1
    nvrhi::BindingLayoutHandle m_LightProbeBindingLayout;                       // +56 PS: t5, t6, t7, s2, s3
    uint32_t m_SupportedViewTypes = 1;                                          // +64
    nvrhi::BufferHandle m_ForwardCB;                                            // +72 "ForwardShadingConstants"
    bool m_TrackLiveness = true;                                                // +104
    std::unique_ptr<donut::engine::BindingCache> m_BindingSets;                 // +112 / +176 (per shadow map / per probe textures)
    nvrhi::AutoPtr<donut::engine::CommonRenderPasses> m_CommonPasses;          // +240
    nvrhi::AutoPtr<donut::engine::FramebufferFactory> m_FramebufferFactory;    // +256

    nvrhi::RenderState m_OpaqueRenderState;                                     // pipeline +80
    nvrhi::RenderState m_AlphaTestedRenderState;                                // pipeline +88
    nvrhi::RenderState m_TransparentRenderState;                                // pipeline +96

    // Sets of the current Render() call.
    nvrhi::BindingSetHandle m_CurrentForwardSet;
    nvrhi::BindingSetHandle m_CurrentLightProbeSet;
};
