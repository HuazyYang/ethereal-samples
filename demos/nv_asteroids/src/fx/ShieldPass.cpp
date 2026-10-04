#include "fx/ShieldPass.h"
#include "fx/FxCommon.h"

#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/FramebufferFactory.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/View.h>
#include <nvrhi/utils.h>

#include <cstddef>

using namespace donut::math;
#include <surface_cb.h>     // surface2018::ShieldConstants (112 bytes, cbuffer g_Shield : b0)

using namespace donut::engine;

namespace fx
{
    ShieldPass::ShieldPass(
        nvrhi::IDevice* device,
        const std::shared_ptr<ShaderFactory>& shaderFactory,
        const std::shared_ptr<CommonRenderPasses>& commonPasses,
        const std::shared_ptr<FramebufferFactory>& framebufferFactory,
        const ICompositeView& compositeView,
        nvrhi::ITexture* depthBuffer)
        : m_FramebufferFactory(framebufferFactory)
    {
        m_ShieldConstants = device->createBuffer(ConstantBufferDesc(sizeof(surface2018::ShieldConstants), "ShieldConstants"));

        nvrhi::BindingSetDesc bindingSetDesc;
        bindingSetDesc.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, m_ShieldConstants),
            nvrhi::BindingSetItem::Texture_SRV(0, depthBuffer)
        };
        // 2018: one helper (0x14000BE30) creates the layout from the set description.
        nvrhi::utils::CreateBindingSetAndLayout(device, nvrhi::ShaderType::Pixel, 0, bindingSetDesc,
            m_BindingLayout, m_BindingSet);

        m_PixelShader = shaderFactory->CreateShader("demo/shield_ps.hlsl", "main", nullptr, nvrhi::ShaderType::Pixel);

        const IView* sampleView = compositeView.GetChildView(ViewType::PLANAR, 0);

        nvrhi::GraphicsPipelineDesc pipelineDesc;
        pipelineDesc.primType = nvrhi::PrimitiveType::TriangleStrip;
        pipelineDesc.VS = commonPasses->m_FullscreenVS;
        pipelineDesc.PS = m_PixelShader;
        pipelineDesc.bindingLayouts = { m_BindingLayout };
        pipelineDesc.renderState.blendState.targets[0] = BlendStateRT(nvrhi::BlendFactor::One, nvrhi::BlendFactor::InvSrcAlpha);
        pipelineDesc.renderState.rasterState.setCullNone();
        pipelineDesc.renderState.depthStencilState
            .disableDepthTest()
            .disableDepthWrite()
            .disableStencil();

        m_Pipeline = device->createGraphicsPipeline(pipelineDesc, m_FramebufferFactory->GetFramebuffer(*sampleView));
    }

    void ShieldPass::Render(
        nvrhi::ICommandList* commandList,
        const ICompositeView& compositeView,
        const float3& shipPosition,
        const float3& shieldDirection,
        float shieldRadius,
        float shieldIntensity,
        float time) const
    {
        if (shieldIntensity <= 0.f)
            return;

        const float distanceToShip = length(shipPosition);

        for (uint32_t viewIndex = 0; viewIndex < compositeView.GetNumChildViews(ViewType::PLANAR); viewIndex++)
        {
            const IView* view = compositeView.GetChildView(ViewType::PLANAR, viewIndex);

            surface2018::ShieldConstants constants{};
            // The views are camera relative, so the clip-to-world matrix maps to the translated world.
            constants.matClipToTranslatedWorld = view->GetInverseViewProjectionMatrix(true);
            constants.shipPosition = shipPosition;
            constants.distanceToShip = distanceToShip;
            constants.shieldRadius = shieldRadius;
            constants.shieldIntensity = shieldIntensity;
            constants.time = time;
            constants.shieldDirection = shieldDirection;
            commandList->writeBuffer(m_ShieldConstants, &constants, sizeof(constants));

            nvrhi::GraphicsState state;
            state.pipeline = m_Pipeline;
            state.framebuffer = m_FramebufferFactory->GetFramebuffer(*view);
            state.bindings = { m_BindingSet };
            state.viewport = view->GetViewportState();
            commandList->setGraphicsState(state);

            nvrhi::DrawArguments args;
            args.vertexCount = 4;
            args.instanceCount = 1;
            commandList->draw(args);
        }
    }
}
