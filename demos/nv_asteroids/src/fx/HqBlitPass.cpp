#include "fx/HqBlitPass.h"
#include "app/GpuProfiler.h"

#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/ShaderFactory.h>

using namespace donut::engine;

namespace fx
{
    HqBlitPass::HqBlitPass(
        nvrhi::IDevice* device,
        const std::shared_ptr<ShaderFactory>& shaderFactory,
        const std::shared_ptr<CommonRenderPasses>& commonPasses,
        nvrhi::IFramebuffer* framebuffer)
        : m_Device(device)
        , m_CommonPasses(commonPasses)
    {
        m_PixelShader = shaderFactory->CreateShader("demo/hq_blit_ps.hlsl", "main", nullptr, nvrhi::ShaderType::Pixel);

        // t0 tex, s0 samp
        nvrhi::BindingLayoutDesc layoutDesc;
        layoutDesc.visibility = nvrhi::ShaderType::Pixel;
        layoutDesc.bindings = {
            nvrhi::BindingLayoutItem::Texture_SRV(0),
            nvrhi::BindingLayoutItem::Sampler(0)
        };
        m_BindingLayout = device->createBindingLayout(layoutDesc);

        nvrhi::GraphicsPipelineDesc pipelineDesc;
        pipelineDesc.primType = nvrhi::PrimitiveType::TriangleStrip;
        // The 2018 CommonRenderPasses full-screen vertex shader (SV_Position + UV in [0,1]).
        pipelineDesc.VS = commonPasses->m_FullscreenVS;
        pipelineDesc.PS = m_PixelShader;
        pipelineDesc.bindingLayouts = { m_BindingLayout };
        pipelineDesc.renderState.rasterState.setCullNone();
        pipelineDesc.renderState.depthStencilState.disableDepthTest().disableDepthWrite().disableStencil();

        m_Pipeline = device->createGraphicsPipeline(pipelineDesc, framebuffer);
    }

    void HqBlitPass::Render(
        nvrhi::ICommandList* commandList,
        nvrhi::IFramebuffer* framebuffer,
        const nvrhi::Viewport& viewport,
        nvrhi::ITexture* sourceTexture)
    {
        // One binding set per source texture (2018: hash map keyed by the texture pointer).
        nvrhi::BindingSetHandle& bindingSet = m_BindingSets[sourceTexture];
        if (!bindingSet)
        {
            nvrhi::BindingSetDesc setDesc;
            setDesc.bindings = {
                nvrhi::BindingSetItem::Texture_SRV(0, sourceTexture),
                // unresolved: the 2018 CommonRenderPasses sampler at +296; the filter does its own
                // bilinear-tap math, so linear clamp is the natural choice.
                nvrhi::BindingSetItem::Sampler(0, m_CommonPasses->m_LinearClampSampler)
            };
            bindingSet = m_Device->createBindingSet(setDesc, m_BindingLayout);
        }

        demo::ProfBegin(commandList, "Upscale");

        nvrhi::GraphicsState state;
        state.pipeline = m_Pipeline;
        state.framebuffer = framebuffer;
        state.bindings = { bindingSet };
        state.viewport.addViewportAndScissorRect(viewport);

        commandList->setGraphicsState(state);

        nvrhi::DrawArguments args;
        args.vertexCount = 4;
        args.instanceCount = 1;
        commandList->draw(args);

        demo::ProfEnd(commandList);
    }
}
