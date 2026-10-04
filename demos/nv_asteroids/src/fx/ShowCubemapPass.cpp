#include "fx/ShowCubemapPass.h"
#include "fx/FxCommon.h"

#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/ShaderFactory.h>

#include <cmath>
#include <cstddef>

using namespace donut::math;
#include <space_cb.h>

using namespace donut::engine;

namespace fx
{
    ShowCubemapPass::ShowCubemapPass(
        nvrhi::IDevice* device,
        const nvrhi::AutoPtr<ShaderFactory>& shaderFactory,
        const nvrhi::AutoPtr<CommonRenderPasses>& commonPasses,
        nvrhi::IFramebuffer* framebuffer,
        nvrhi::ITexture* cubemap)
    {
        // 2018 sampler: wrap addressing, linear filters, no compare.
        auto samplerDesc = nvrhi::SamplerDesc()
            .setAllAddressModes(nvrhi::SamplerAddressMode::Wrap)
            .setAllFilters(true);
        device->createSampler(samplerDesc, &m_Sampler);

        m_PixelShader = shaderFactory->CreateShader("demo/show_cubemap_ps.hlsl", "main", nullptr, nvrhi::ShaderType::Pixel);

        // The 2018 buffer is 16 bytes although the shader's cbuffer CB is 8 bytes.
        device->createBuffer(ConstantBufferDesc(16, "ShowCubemapConstants"), &m_Constants);

        nvrhi::BindingSetDesc setDesc;
        setDesc.bindings = {
            nvrhi::BindingSetItem::Texture_SRV(0, cubemap, nvrhi::Format::UNKNOWN, nvrhi::AllSubresources,
                nvrhi::TextureDimension::TextureCubeArray),
            nvrhi::BindingSetItem::Sampler(0, m_Sampler),
            nvrhi::BindingSetItem::ConstantBuffer(0, m_Constants)
        };
        // 2018 helper 0x14000BE30 derived the layout from the set; nvrhi::utils::CreateBindingSetAndLayout
        // would declare the volatile buffer as a plain ConstantBuffer, so the layout is spelled out.
        nvrhi::BindingLayoutDesc layoutDesc;
        layoutDesc.visibility = nvrhi::ShaderType::Pixel;
        layoutDesc.bindings = {
            nvrhi::BindingLayoutItem::Texture_SRV(0),
            nvrhi::BindingLayoutItem::Sampler(0),
            nvrhi::BindingLayoutItem::VolatileConstantBuffer(0)
        };
        device->createBindingLayout(layoutDesc, &m_BindingLayout);
        device->createBindingSet(setDesc, m_BindingLayout, &m_BindingSet);

        nvrhi::GraphicsPipelineDesc pipelineDesc;
        pipelineDesc.primType = nvrhi::PrimitiveType::TriangleStrip;
        pipelineDesc.VS = commonPasses->m_FullscreenVS;
        pipelineDesc.PS = m_PixelShader;
        pipelineDesc.bindingLayouts = { m_BindingLayout };
        pipelineDesc.renderState.rasterState.setCullNone();
        pipelineDesc.renderState.depthStencilState.disableDepthTest().disableDepthWrite();
        device->createGraphicsPipeline2(pipelineDesc, framebuffer, &m_Pipeline);
    }

    void ShowCubemapPass::Render(
        nvrhi::ICommandList* commandList,
        nvrhi::IFramebuffer* framebuffer,
        const nvrhi::Viewport& viewport,
        uint32_t arrayIndex,
        uint32_t mipLevel)
    {
        ShowCubemapConstants constants{};
        constants.g_ArrayIndex = arrayIndex;
        constants.g_MipLevel = mipLevel;
        commandList->writeBuffer(m_Constants, &constants, sizeof(constants));

        nvrhi::GraphicsState state;
        state.pipeline = m_Pipeline;
        state.framebuffer = framebuffer;
        state.bindings = { m_BindingSet };
        state.viewport.addViewport(viewport);
        state.viewport.addScissorRect(nvrhi::Rect(
            int(floorf(viewport.minX)), int(ceilf(viewport.maxX)),
            int(floorf(viewport.minY)), int(ceilf(viewport.maxY))));
        commandList->setGraphicsState(state);

        nvrhi::DrawArguments args;
        args.vertexCount = 4;
        args.instanceCount = 1;
        commandList->draw(args);
    }
}
