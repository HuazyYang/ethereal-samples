#pragma once

// Asteroids.exe: 2018 framework GBufferFillPass (no RTTI; ctor 0x140087630, Render 0x1400890D0, 0x80 bytes).
// FeatureDemo creates one instance (+848) in CreateRenderPasses (0x14002BD60) with gbuffer_ps and draws the
// G-buffer asteroid renderer (+248) through it in RenderGBuffer (0x140032630). The same class and Render function
// serve the picking "material ID" pass (+904) in RenderOverlay (0x1400321F0), but the shipped binary never creates
// that instance (+904 stays null), see src/passes/NOTES.md.
//
// deviation: donut main's GBufferFillPass uses donut's own gbuffer shaders and cbuffers; the 2018 pass takes the
// demo pixel shader and fills the 2018 c_GBuffer (432 bytes) that gbuffer_ps reads at b1, so it is a demo-side class.
// The 2018 pass handed an nvrhi GraphicsState (pass pipeline + binding sets) to the meshlet strategy; here it hands
// a MeshletPassState (render state, binding layouts and sets) to MeshletDrawStrategy::Render.

#include <donut/core/math/math.h>
#include <nvrhi/nvrhi.h>

#include <memory>

namespace donut::engine
{
    class ShaderFactory;
    class CommonRenderPasses;
    class FramebufferFactory;
    class ICompositeView;
    class IView;
}

class MeshletDrawStrategy;
struct MeshletPassState;
struct SceneMaterial;

class GBufferFillPass2018
{
public:
    // 2018 creation parameters (0x1C bytes). FeatureDemo passes
    // { gbuffer_ps (IS_SHIP=0, _ASTEROIDS=0), nullptr, false, false, true, true, false, 1 }.
    struct CreateParameters
    {
        nvrhi::ShaderHandle materialPixelShader;    // +0
        nvrhi::ShaderHandle alphaTestedPixelShader; // +8  null: alpha-tested materials use the main PS with alpha-to-coverage
        bool enableSinglePassStereo = false;        // +16 SINGLE_PASS_STEREO=1 gbuffer_vs + forward_gs
        bool enableSinglePassCubemap = false;       // +17 cubemap_gs
        bool enableDepthWrite = true;               // +18
        bool enableMotionVectors = false;           // +19 MOTION_VECTORS=1 gbuffer_vs, PREV_TRANSFORM instance stream
        bool trackLiveness = false;                 // +20 2018 binding-set flag; nvrhi main tracks resource lifetime itself
        uint32_t stencilWriteMask = 0;              // +24 non-zero: stencil = mask (replace) where geometry is drawn
    };

    // 0x140087630. 'framebufferFactory' is RenderTargets::GBufferFramebuffer for the G-buffer pass; 'compositeView'
    // only selects the view types (planar / stereo / cubemap) and the framebuffer used to create the pipelines.
    // 'materialBindingLayout' is the layout of SceneMaterial::bindingSet (2018: CommonRenderPasses+328).
    GBufferFillPass2018(
        nvrhi::IDevice* device,
        std::shared_ptr<donut::engine::ShaderFactory> shaderFactory,
        std::shared_ptr<donut::engine::CommonRenderPasses> commonPasses,
        std::shared_ptr<donut::engine::FramebufferFactory> framebufferFactory,
        const donut::engine::ICompositeView& compositeView,
        nvrhi::IBindingLayout* materialBindingLayout,
        const CreateParameters& params);

    // 0x1400890D0: for every child view, fills c_GBuffer (current view matrices, previous view-projection and
    // viewport for the motion vectors, pixel offset) and lets the strategy draw into the framebuffer of that view.
    // 'compositeViewPrevious' may be null (then the current view is used, i.e. no camera motion).
    void Render(
        nvrhi::ICommandList* commandList,
        MeshletDrawStrategy& drawStrategy,
        const donut::engine::ICompositeView& compositeView,
        const donut::engine::ICompositeView* compositeViewPrevious);

    // Pipelines for regular (input-assembler) geometry, created like in 2018 from framework/passes/gbuffer_vs.hlsl
    // (+ forward_gs / cubemap_gs) and the material pixel shader. The demo draws everything through meshlet
    // strategies, so FeatureDemo never uses them.
    nvrhi::IGraphicsPipeline* GetOpaquePipeline() const { return m_OpaquePipeline; }
    nvrhi::IGraphicsPipeline* GetAlphaTestedPipeline() const { return m_AlphaTestedPipeline; }
    nvrhi::IBindingSet* GetViewBindingSet() const { return m_VertexViewBindingSet; }

private:
    void CreateShaders(donut::engine::ShaderFactory& shaderFactory, const CreateParameters& params);
    void CreatePipelines(nvrhi::IFramebuffer* framebuffer);

    // The 2018 material filter (lambda in 0x1400890D0): skips materials without a binding set and transparent
    // materials, binds the material set and selects the opaque / alpha-tested state.
    bool SetupMaterial(SceneMaterial* material, MeshletPassState& state) const;

    nvrhi::DeviceHandle m_Device;                                               // +0
    nvrhi::InputLayoutHandle m_InputLayout;                                     // +8
    nvrhi::ShaderHandle m_VertexShader;                                         // +16
    nvrhi::ShaderHandle m_PixelShader;                                          // +24
    nvrhi::ShaderHandle m_PixelShaderAlphaTested;                               // +32
    nvrhi::ShaderHandle m_GeometryShader;                                       // +40
    nvrhi::BufferHandle m_GBufferCB;                                            // +56 "GBufferFillConstants"
    nvrhi::GraphicsPipelineHandle m_OpaquePipeline;                             // +64
    nvrhi::GraphicsPipelineHandle m_AlphaTestedPipeline;                        // +72
    bool m_UseAlphaToCoverage = true;                                           // +88 (no alpha-tested PS)
    uint32_t m_SupportedViewTypes = 1;                                          // +92
    std::shared_ptr<donut::engine::CommonRenderPasses> m_CommonPasses;          // +96
    std::shared_ptr<donut::engine::FramebufferFactory> m_FramebufferFactory;    // +112

    // deviation: the 2018 binding layout (+48) had a VS section (b0) and a PS section (b1) pointing at the same
    // buffer; nvrhi main layouts have one visibility, so there are two layouts and two sets.
    nvrhi::BindingLayoutHandle m_MaterialBindingLayout;
    nvrhi::BindingLayoutHandle m_VertexViewBindingLayout;   // VS/GS b0 c_GBuffer (regular geometry only)
    nvrhi::BindingLayoutHandle m_PixelViewBindingLayout;    // PS b1 c_GBuffer
    nvrhi::BindingSetHandle m_VertexViewBindingSet;
    nvrhi::BindingSetHandle m_PixelViewBindingSet;          // +80

    nvrhi::RenderState m_OpaqueRenderState;
    nvrhi::RenderState m_AlphaTestedRenderState;
};
