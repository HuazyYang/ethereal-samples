#include "passes/BloomPass2018.h"
#include "app/GpuProfiler.h"

#include "passes/Framework2018Constants.h"

#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/FramebufferFactory.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/View.h>

#include <algorithm>
#include <cmath>

using namespace donut;
using namespace donut::math;
using namespace donut::engine;

BloomPass2018::BloomPass2018(
    nvrhi::IDevice* device,
    const std::shared_ptr<ShaderFactory>& shaderFactory,
    std::shared_ptr<CommonRenderPasses> commonPasses,
    std::shared_ptr<FramebufferFactory> framebufferFactory,
    const ICompositeView& compositeView)
    : m_CommonPasses(std::move(commonPasses))
    , m_FramebufferFactory(std::move(framebufferFactory))
    , m_Device(device)
    , m_BindingCache(device)
{
    m_BloomBlurPixelShader = shaderFactory->CreateShader("framework/passes/bloom_ps.hlsl", "main", nullptr,
        nvrhi::ShaderType::Pixel);

    nvrhi::BufferDesc constantBufferDesc;
    constantBufferDesc.byteSize = sizeof(framework2018::BloomConstants);
    constantBufferDesc.isConstantBuffer = true;
    constantBufferDesc.isVolatile = true;
    constantBufferDesc.maxVersions = c_MaxRenderPassConstantBufferVersions;
    constantBufferDesc.debugName = "BloomConstantsH";
    m_BloomHBlurCB = device->createBuffer(constantBufferDesc);
    constantBufferDesc.debugName = "BloomConstantsV";
    m_BloomVBlurCB = device->createBuffer(constantBufferDesc);

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Pixel;
    layoutDesc.bindings = {
        nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
        nvrhi::BindingLayoutItem::Sampler(0),
        nvrhi::BindingLayoutItem::Texture_SRV(0),
    };
    m_BloomBlurBindingLayout = device->createBindingLayout(layoutDesc);

    m_PerViewData.resize(compositeView.GetNumChildViews(ViewType::PLANAR));
    for (uint32_t viewIndex = 0; viewIndex < compositeView.GetNumChildViews(ViewType::PLANAR); viewIndex++)
    {
        const IView* view = compositeView.GetChildView(ViewType::PLANAR, viewIndex);
        nvrhi::IFramebuffer* sampleFramebuffer = m_FramebufferFactory->GetFramebuffer(*view);
        PerViewData& perView = m_PerViewData[viewIndex];

        const nvrhi::Rect extent = view->GetViewExtent();
        const int viewportWidth = extent.maxX - extent.minX;
        const int viewportHeight = extent.maxY - extent.minY;

        nvrhi::TextureDesc downscaleDesc;
        downscaleDesc.format = sampleFramebuffer->getFramebufferInfo().colorFormats[0];
        downscaleDesc.width = uint32_t(std::ceil(float(viewportWidth) * 0.5f));
        downscaleDesc.height = uint32_t(std::ceil(float(viewportHeight) * 0.5f));
        downscaleDesc.mipLevels = 1;
        downscaleDesc.isRenderTarget = true;
        downscaleDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        downscaleDesc.keepInitialState = true;
        downscaleDesc.debugName = "bloom src mip1";
        perView.textureDownscale1 = device->createTexture(downscaleDesc);
        perView.framebufferDownscale1 = device->createFramebuffer(
            nvrhi::FramebufferDesc().addColorAttachment(perView.textureDownscale1));

        downscaleDesc.debugName = "bloom src mip2";
        downscaleDesc.width = uint32_t(std::ceil(float(downscaleDesc.width) * 0.5f));
        downscaleDesc.height = uint32_t(std::ceil(float(downscaleDesc.height) * 0.5f));
        perView.textureDownscale2 = device->createTexture(downscaleDesc);
        perView.framebufferDownscale2 = device->createFramebuffer(
            nvrhi::FramebufferDesc().addColorAttachment(perView.textureDownscale2));

        nvrhi::TextureDesc blurDesc = downscaleDesc;
        blurDesc.debugName = "bloom accumulation pass1";
        perView.texturePass1Blur = device->createTexture(blurDesc);
        perView.framebufferPass1Blur = device->createFramebuffer(
            nvrhi::FramebufferDesc().addColorAttachment(perView.texturePass1Blur));

        blurDesc.debugName = "bloom accumulation pass2";
        perView.texturePass2Blur = device->createTexture(blurDesc);
        perView.framebufferPass2Blur = device->createFramebuffer(
            nvrhi::FramebufferDesc().addColorAttachment(perView.texturePass2Blur));

        nvrhi::GraphicsPipelineDesc pipelineDesc;
        pipelineDesc.primType = nvrhi::PrimitiveType::TriangleStrip;
        pipelineDesc.VS = m_CommonPasses->m_FullscreenVS;
        pipelineDesc.PS = m_BloomBlurPixelShader;
        pipelineDesc.bindingLayouts = { m_BloomBlurBindingLayout };
        pipelineDesc.renderState.rasterState.setCullNone();
        pipelineDesc.renderState.depthStencilState.depthTestEnable = false;
        pipelineDesc.renderState.depthStencilState.stencilEnable = false;
        perView.bloomBlurPso = device->createGraphicsPipeline(pipelineDesc, perView.framebufferPass1Blur);

        // CommonRenderPasses+296: linear clamp sampler.
        nvrhi::BindingSetDesc setDesc;
        setDesc.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, m_BloomHBlurCB),
            nvrhi::BindingSetItem::Sampler(0, m_CommonPasses->m_LinearClampSampler),
            nvrhi::BindingSetItem::Texture_SRV(0, perView.textureDownscale2),
        };
        perView.bloomBlurBindingSetPass1 = device->createBindingSet(setDesc, m_BloomBlurBindingLayout);

        setDesc.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, m_BloomVBlurCB),
            nvrhi::BindingSetItem::Sampler(0, m_CommonPasses->m_LinearClampSampler),
            nvrhi::BindingSetItem::Texture_SRV(0, perView.texturePass1Blur),
        };
        perView.bloomBlurBindingSetPass2 = device->createBindingSet(setDesc, m_BloomBlurBindingLayout);
    }
}

void BloomPass2018::Render(
    nvrhi::ICommandList* commandList,
    const std::shared_ptr<FramebufferFactory>& framebufferFactory,
    const ICompositeView& compositeView,
    nvrhi::ITexture* sourceDestTexture,
    float sigmaInPixels)
{
    // 0x140090420
    const float effectiveSigma = std::min(std::max(sigmaInPixels * 0.25f, 1.f), 100.f);

    demo::ProfBegin(commandList, "Bloom");

    nvrhi::DrawArguments fullscreenArgs;
    fullscreenArgs.vertexCount = 4;
    fullscreenArgs.instanceCount = 1;

    for (uint32_t viewIndex = 0; viewIndex < compositeView.GetNumChildViews(ViewType::PLANAR); viewIndex++)
    {
        const IView* view = compositeView.GetChildView(ViewType::PLANAR, viewIndex);
        nvrhi::IFramebuffer* framebuffer = framebufferFactory->GetFramebuffer(*view);
        PerViewData& perView = m_PerViewData[viewIndex];

        const nvrhi::ViewportState viewportState = view->GetViewportState();
        const nvrhi::Rect extent = view->GetViewExtent();
        const nvrhi::FramebufferInfoEx& fbinfo = framebuffer->getFramebufferInfo();

        {
            demo::ProfBegin(commandList, "Downscale");

            BlitParameters blit1;
            blit1.targetFramebuffer = perView.framebufferDownscale1;
            blit1.sourceTexture = sourceDestTexture;
            blit1.sourceBox = box2(
                float2(float(extent.minX) / float(fbinfo.width), float(extent.minY) / float(fbinfo.height)),
                float2(float(extent.maxX) / float(fbinfo.width), float(extent.maxY) / float(fbinfo.height)));
            m_CommonPasses->BlitTexture(commandList, blit1, &m_BindingCache);

            BlitParameters blit2;
            blit2.targetFramebuffer = perView.framebufferDownscale2;
            blit2.sourceTexture = perView.textureDownscale1;
            m_CommonPasses->BlitTexture(commandList, blit2, &m_BindingCache);

            demo::ProfEnd(commandList);
        }

        {
            demo::ProfBegin(commandList, "Blur");

            const nvrhi::TextureDesc& pass1Desc = perView.texturePass1Blur->getDesc();
            const nvrhi::TextureDesc& pass2Desc = perView.texturePass2Blur->getDesc();

            framework2018::BloomConstants horizontal = {};
            horizontal.pixstep = float2(1.f / float(pass1Desc.width), 0.f);
            horizontal.argumentScale = -1.f / (2.f * effectiveSigma * effectiveSigma);
            horizontal.normalizationScale = 1.f / (std::sqrt(2.f * PI_f) * effectiveSigma);
            horizontal.numSamples = std::round(effectiveSigma * 4.f);

            framework2018::BloomConstants vertical = horizontal;
            vertical.pixstep = float2(0.f, 1.f / float(pass1Desc.height));

            commandList->writeBuffer(m_BloomHBlurCB, &horizontal, sizeof(horizontal));
            commandList->writeBuffer(m_BloomVBlurCB, &vertical, sizeof(vertical));

            nvrhi::GraphicsState state;
            state.pipeline = perView.bloomBlurPso;
            state.framebuffer = perView.framebufferPass1Blur;
            state.bindings = { perView.bloomBlurBindingSetPass1 };
            nvrhi::Viewport viewport(float(pass1Desc.width), float(pass1Desc.height));
            state.viewport.addViewportAndScissorRect(viewport);
            commandList->setGraphicsState(state);
            commandList->draw(fullscreenArgs);

            state.framebuffer = perView.framebufferPass2Blur;
            state.bindings = { perView.bloomBlurBindingSetPass2 };
            viewport = nvrhi::Viewport(float(pass2Desc.width), float(pass2Desc.height));
            state.viewport = nvrhi::ViewportState().addViewportAndScissorRect(viewport);
            commandList->setGraphicsState(state);
            commandList->draw(fullscreenArgs);

            demo::ProfEnd(commandList);
        }

        {
            demo::ProfBegin(commandList, "Apply");

            BlitParameters blit3;
            blit3.targetFramebuffer = framebuffer;
            blit3.targetViewport = viewportState.viewports[0];
            blit3.sourceTexture = perView.texturePass2Blur;
            blit3.blendState.setBlendEnable(true)
                .setSrcBlend(nvrhi::BlendFactor::ConstantColor)
                .setDestBlend(nvrhi::BlendFactor::InvConstantColor)
                .setBlendOp(nvrhi::BlendOp::Add)
                .setSrcBlendAlpha(nvrhi::BlendFactor::Zero)
                .setDestBlendAlpha(nvrhi::BlendFactor::One)
                .setBlendOpAlpha(nvrhi::BlendOp::Add);
            blit3.blendConstantColor = nvrhi::Color(c_BlendFactor);
            m_CommonPasses->BlitTexture(commandList, blit3, &m_BindingCache);

            demo::ProfEnd(commandList);
        }
    }

    demo::ProfEnd(commandList);
}
