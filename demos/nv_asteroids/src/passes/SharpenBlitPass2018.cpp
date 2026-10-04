#include "passes/SharpenBlitPass2018.h"

#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/ShaderFactory.h>
#include <nvrhi/utils.h>

using namespace donut;
using namespace donut::math;
#include "../../shaders/framework/blit_cb_2018.h"

SharpenBlitPass2018::SharpenBlitPass2018(nvrhi::IDevice* device, nvrhi::AutoPtr<engine::ShaderFactory> shaderFactory,
    nvrhi::AutoPtr<engine::CommonRenderPasses> commonPasses)
    : m_Device(device)
    , m_CommonPasses(std::move(commonPasses))
{
    // 2018 CommonRenderPasses ctor 0x140078E70: rect_vs (+216), sharpen_ps (+232), "BlitConstants" (+240),
    // linear clamp sampler (+296).
    m_PixelShader = shaderFactory->CreateShader("framework/sharpen_ps.hlsl", "main", nullptr, nvrhi::ShaderType::Pixel);

    device->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(
        sizeof(blit2018::BlitConstants), "BlitConstants", engine::c_MaxRenderPassConstantBufferVersions), &m_Constants);

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::All;
    layoutDesc.bindings = {
        nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
        nvrhi::BindingLayoutItem::Texture_SRV(0),
        nvrhi::BindingLayoutItem::Sampler(0)
    };
    device->createBindingLayout(layoutDesc, &m_BindingLayout);
}

void SharpenBlitPass2018::Render(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer,
    const nvrhi::Viewport& viewport, nvrhi::ITexture* source, float sharpenFactor)
{
    // Asteroids.exe: 0x14007AAB0 / 0x14007A5B0
    const nvrhi::FramebufferInfo framebufferInfo = framebuffer->getFramebufferInfo().getInfo();
    if (!m_Pipeline || !(m_PipelineFramebufferInfo == framebufferInfo))
    {
        nvrhi::GraphicsPipelineDesc desc;
        desc.primType = nvrhi::PrimitiveType::TriangleStrip;   // 2018 primType 3
        desc.VS = m_CommonPasses->m_RectVS;
        desc.PS = m_PixelShader;
        desc.bindingLayouts = { m_BindingLayout };
        desc.renderState.rasterState.setCullNone();             // 2018 cull mode 2 (none)
        desc.renderState.depthStencilState.setDepthTestEnable(false).setStencilEnable(false);
        m_Device->createGraphicsPipeline1(desc, framebufferInfo, &m_Pipeline);
        m_PipelineFramebufferInfo = framebufferInfo;
    }

    BindingEntry& bindingEntry = m_BindingSets[source];
    bindingEntry.texture = source;
    nvrhi::BindingSetHandle& bindingSet = bindingEntry.bindingSet;
    if (!bindingSet)
    {
        nvrhi::BindingSetDesc setDesc;
        setDesc.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, m_Constants),
            nvrhi::BindingSetItem::Texture_SRV(0, source),
            nvrhi::BindingSetItem::Sampler(0, m_CommonPasses->m_LinearClampSampler)
        };
        m_Device->createBindingSet(setDesc, m_BindingLayout, &bindingSet);
    }

    // Source and target boxes are both [0,1]^2 (xmmword_140259690), sourceSlice 0.
    blit2018::BlitConstants constants = {};
    constants.sourceOrigin = float2(0.f);
    constants.sourceSize = float2(1.f);
    constants.targetOrigin = float2(0.f);
    constants.targetSize = float2(1.f);
    constants.sourceSlice = 0;
    constants.sharpenFactor = sharpenFactor;
    commandList->writeBuffer(m_Constants, &constants, sizeof(constants));

    nvrhi::GraphicsState state;
    state.pipeline = m_Pipeline;
    state.framebuffer = framebuffer;
    state.bindings = { bindingSet };
    state.viewport.addViewportAndScissorRect(viewport);
    commandList->setGraphicsState(state);

    nvrhi::DrawArguments args;
    args.vertexCount = 4;
    commandList->draw(args);
}
