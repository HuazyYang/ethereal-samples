#include "CompositePass.h"
#include "GlobalResources.h"
#include <donut/engine/ShaderFactory.h>
#include "Config.h"

bool CompositePass::Initialize(GlobalResources* resource,
                               donut::engine::ShaderFactory* shaderFactory) {
    m_GlobalResources = resource;
    return CreatePipeline(shaderFactory);
}

void CompositePass::Update(const Config* config) {
    auto& globalConsts = m_GlobalResources->GlobalConsts;

    globalConsts.composite.useFlags = Graphics::COMPOSITE_FLAG_SHOW_NONE;
    if(config->renderers.rtao.enabled)
        globalConsts.composite.useFlags |= Graphics::COMPOSITE_FLAG_USE_RTAO;
    if(config->renderers.ddgi.enabled)
        globalConsts.composite.useFlags |= Graphics::COMPOSITE_FLAG_USE_DDGI;

    globalConsts.composite.showFlags = Graphics::COMPOSITE_FLAG_SHOW_NONE;

    globalConsts.post.useFlags = Graphics::POSTPROCESS_FLAG_USE_NONE;
    if(config->renderers.pp.exposure.enabled) {
        globalConsts.post.exposure = std::pow(2.f, config->renderers.pp.exposure.fstops);
        globalConsts.post.useFlags |= Graphics::POSTPROCESS_FLAG_USE_EXPOSURE;
    }
    if(config->renderers.pp.tonemapping.enabled)
        globalConsts.post.useFlags |= Graphics::POSTPROCESS_FLAG_USE_TONEMAPPING;
    if(config->renderers.pp.dithering.enabled)
        globalConsts.post.useFlags |= Graphics::POSTPROCESS_FLAG_USE_DITHER;
    if(config->renderers.pp.gammaCorrection.enabled)
        globalConsts.post.useFlags |= Graphics::POSTPROCESS_FLAG_USE_GAMMA;

    m_GlobalResources->SetGlobalConstsDirty();
}

void CompositePass::Execute(nvrhi::ICommandList* commandList) {
    commandList->beginMarker("Composite");

    nvrhi::GraphicsState state;
    state.pipeline = m_CompositePipeline;
    state.bindings = {  m_GlobalResources->BindingSet, m_GlobalResources->SceneDescriptorTable, m_GlobalResources->FixedDescriptorTable };
    state.viewport.addViewport(nvrhi::Viewport{(float)m_GlobalResources->Size.x,
                                               (float)m_GlobalResources->Size.y});
    state.viewport.addScissorRect(
        nvrhi::Rect{(int)m_GlobalResources->Size.x, (int)m_GlobalResources->Size.y});
    state.framebuffer = m_GlobalResources->CompositeFramebuffer;
    commandList->setGraphicsState(state);
    m_GlobalResources->SetDefaultRootConstants(commandList);

    nvrhi::DrawArguments args;
    args.vertexCount = 4;
    commandList->draw(args);

    commandList->endMarker();
}

bool CompositePass::CreatePipeline(donut::engine::ShaderFactory* shaderFactory) {
    std::vector<donut::engine::ShaderMacro> defines = {{ "HLSL", "1" }};
    auto vs = shaderFactory->CreateShader("app/Composite.hlsl", "VS", &defines,
                                          nvrhi::ShaderType::Vertex);
    auto ps = shaderFactory->CreateShader("app/Composite.hlsl", "PS", &defines,
                                          nvrhi::ShaderType::Pixel);
    nvrhi::GraphicsPipelineDesc psoDesc;
    psoDesc.primType = nvrhi::PrimitiveType::TriangleStrip;
    psoDesc.bindingLayouts = { m_GlobalResources->BindingLayout, m_GlobalResources->BindlessLayout, m_GlobalResources->FixedBindlessLayout };
    psoDesc.VS = vs;
    psoDesc.PS = ps;
    psoDesc.renderState.rasterState.frontCounterClockwise = true;
    psoDesc.renderState.rasterState.setCullBack();
    psoDesc.renderState.depthStencilState.depthTestEnable = false;
    m_CompositePipeline = m_GlobalResources->Device->createGraphicsPipeline(psoDesc, m_GlobalResources->CompositeFramebuffer->getFramebufferInfo());
    if (!m_CompositePipeline) return false;

    return true;
}
