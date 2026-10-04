#include "fx/EnvironmentMapPass.h"
#include "app/GpuProfiler.h"
#include "fx/FxCommon.h"

#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/FramebufferFactory.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/View.h>

#include <cstddef>

using namespace donut::math;
#include <space_cb.h>

using namespace donut::engine;

namespace fx
{
    EnvironmentMapPass::EnvironmentMapPass(
        nvrhi::IDevice* device,
        const std::shared_ptr<ShaderFactory>& shaderFactory,
        const std::shared_ptr<CommonRenderPasses>& commonPasses,
        const std::shared_ptr<FramebufferFactory>& framebufferFactory,
        const ICompositeView& compositeView,
        nvrhi::ITexture* environmentMap,
        nvrhi::ITexture* starMap)
        : m_CommonPasses(commonPasses)
        , m_FramebufferFactory(framebufferFactory)
    {
        m_PixelShader = shaderFactory->CreateShader("demo/texturedstarfield_ps.hlsl", "main", nullptr, nvrhi::ShaderType::Pixel);

        // 2018: 96-byte volatile buffer named "DeferredLightingConstants" (sizeof(StarFieldConstants) = 92).
        m_StarFieldCB = device->createBuffer(ConstantBufferDesc(96, "DeferredLightingConstants"));

        m_HasStarMap = starMap != nullptr;

        const IView* sampleView = compositeView.GetChildView(ViewType::PLANAR, 0);

        nvrhi::BindingLayoutDesc layoutDesc;
        layoutDesc.visibility = nvrhi::ShaderType::Pixel;
        layoutDesc.bindings = {
            nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
            nvrhi::BindingLayoutItem::Texture_SRV(0),
            nvrhi::BindingLayoutItem::Texture_SRV(1),
            nvrhi::BindingLayoutItem::Sampler(0)
        };
        m_BindingLayout = device->createBindingLayout(layoutDesc);

        nvrhi::BindingSetDesc setDesc;
        setDesc.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, m_StarFieldCB),
            nvrhi::BindingSetItem::Texture_SRV(0, environmentMap),                                      // t_EnvironmentMap
            nvrhi::BindingSetItem::Texture_SRV(1, starMap ? starMap : commonPasses->m_BlackTexture.Get()), // t_StarMap
            nvrhi::BindingSetItem::Sampler(0, commonPasses->m_LinearWrapSampler)                       // s_Sampler (the star map tiles)
        };
        m_BindingSet = device->createBindingSet(setDesc, m_BindingLayout);

        nvrhi::GraphicsPipelineDesc pipelineDesc;
        pipelineDesc.primType = nvrhi::PrimitiveType::TriangleStrip;
        pipelineDesc.VS = sampleView->IsReverseDepth() ? commonPasses->m_FullscreenVS : commonPasses->m_FullscreenAtOneVS;
        pipelineDesc.PS = m_PixelShader;
        pipelineDesc.bindingLayouts = { m_BindingLayout };
        pipelineDesc.renderState.rasterState.setCullNone();

        // Depth test only when the target framebuffer has a depth attachment.
        const nvrhi::FramebufferInfo& framebufferInfo = m_FramebufferFactory->GetFramebuffer(*sampleView)->getFramebufferInfo();
        if (framebufferInfo.depthFormat != nvrhi::Format::UNKNOWN)
        {
            pipelineDesc.renderState.depthStencilState
                .enableDepthTest()
                .disableDepthWrite()
                .setDepthFunc(sampleView->IsReverseDepth()
                    ? nvrhi::ComparisonFunc::GreaterOrEqual
                    : nvrhi::ComparisonFunc::LessOrEqual);
        }
        else
        {
            pipelineDesc.renderState.depthStencilState.disableDepthTest().disableDepthWrite();
        }
        pipelineDesc.renderState.depthStencilState.disableStencil();

        m_Pipeline = device->createGraphicsPipeline(pipelineDesc, m_FramebufferFactory->GetFramebuffer(*sampleView));
    }

    void EnvironmentMapPass::Render(
        nvrhi::ICommandList* commandList,
        const ICompositeView& compositeView,
        float nebulaLinearBrightness,
        float starLinearBrightness,
        float starSquareBrightness) const
    {
        demo::ProfBegin(commandList, "Environment Map");

        for (uint32_t viewIndex = 0; viewIndex < compositeView.GetNumChildViews(ViewType::PLANAR); viewIndex++)
        {
            const IView* view = compositeView.GetChildView(ViewType::PLANAR, viewIndex);

            nvrhi::GraphicsState state;
            state.pipeline = m_Pipeline;
            state.framebuffer = m_FramebufferFactory->GetFramebuffer(*view);
            state.bindings = { m_BindingSet };
            state.viewport = view->GetViewportState();

            // Clip space -> world space with the camera at the origin.
            const float4x4 clipToTranslatedWorld = view->GetInverseViewProjectionMatrix(true)
                * affineToHomogeneous(translation(-view->GetViewOrigin()));

            StarFieldConstants constants{};
            constants.matClipToTranslatedWorld = clipToTranslatedWorld;
            constants.directionToSun = float4(0.f);   // not set by the 2018 code (zeroed)
            constants.nebulaLinearBrightness = nebulaLinearBrightness;
            constants.starSquareBrightness = m_HasStarMap ? starSquareBrightness : 0.f;
            constants.starLinearBrightness = m_HasStarMap ? starLinearBrightness : 0.f;
            commandList->writeBuffer(m_StarFieldCB, &constants, sizeof(constants));

            commandList->setGraphicsState(state);

            nvrhi::DrawArguments args;
            args.vertexCount = 4;
            args.instanceCount = 1;
            commandList->draw(args);
        }

        demo::ProfEnd(commandList);
    }
}
