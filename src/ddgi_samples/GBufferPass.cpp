#include "GBufferPass.h"
#include "GlobalResources.h"
#include "Config.h"
#include <donut/engine/ShaderFactory.h>

bool GBufferPass::Initialize(GlobalResources* resources,
                             donut::engine::ShaderFactory* shaderFactory) {
    m_GlobalResources = resources;

    std::vector<donut::engine::ShaderMacro> defines = { { "HLSL", "1" }};
    auto raygenShaderLibrary =
        shaderFactory->CreateShaderLibrary("app/GBufferRGS.hlsl", &defines);
    auto missShaderLibrary = shaderFactory->CreateShaderLibrary("app/Miss.hlsl", &defines);
    auto closestShaderLibrary = shaderFactory->CreateShaderLibrary("app/CHS.hlsl", &defines);
    auto anyhitShaderLibrary = shaderFactory->CreateShaderLibrary("app/AHS.hlsl", &defines);

    nvrhi::rt::PipelineDesc psoDesc;
    psoDesc.globalBindingLayouts = {m_GlobalResources->BindingLayout,
                                    m_GlobalResources->BindlessLayout,
                                    m_GlobalResources->FixedBindlessLayout};

    nvrhi::ShaderHandle rayGenShader;
    raygenShaderLibrary->getShader("RayGen", nvrhi::ShaderType::RayGeneration, &rayGenShader);
    nvrhi::ShaderHandle missShader;
    missShaderLibrary->getShader("Miss", nvrhi::ShaderType::Miss, &missShader);
    psoDesc.shaders = {
        {"RayGen",
         rayGenShader,
         nullptr},
        {"Miss", missShader, nullptr}};
    nvrhi::ShaderHandle chsPrimaryShader;
    closestShaderLibrary->getShader("CHS_PRIMARY", nvrhi::ShaderType::ClosestHit, &chsPrimaryShader);
    nvrhi::ShaderHandle ahsPrimaryShader;
    anyhitShaderLibrary->getShader("AHS_PRIMARY", nvrhi::ShaderType::AnyHit, &ahsPrimaryShader);
    psoDesc.hitGroups = {{
        "HitGroup",
        chsPrimaryShader,
        ahsPrimaryShader,
        nullptr,
        nullptr,
        false}};

    psoDesc.maxPayloadSize = sizeof(Graphics::PackedPayload);

    if (NVRHI_FAILED(m_GlobalResources->Device->createRayTracingPipeline(psoDesc, &m_GBufferPipeline))) return false;

    if (NVRHI_FAILED(m_GBufferPipeline->createShaderTable(nvrhi::rt::ShaderTableDesc(), &m_GBufferShaderTable))) return false;

    m_GBufferShaderTable->setRayGenerationShader("RayGen");
    m_GBufferShaderTable->addHitGroup("HitGroup");
    m_GBufferShaderTable->addMissShader("Miss");

    return true;
}

void GBufferPass::Update(const Config* config) {
    auto& globalConsts = m_GlobalResources->GlobalConsts;
    globalConsts.pt.rayNormalBias = config->renderers.pt.rayNormalBias;
    globalConsts.pt.rayViewBias = config->renderers.pt.rayViewBias;
}

void GBufferPass::Execute(nvrhi::ICommandList* commandList) {
    commandList->beginMarker("GBuffer");

    nvrhi::ITexture* gbufferTextures[] = {
        m_GlobalResources->GBufferA, m_GlobalResources->GBufferB,
        m_GlobalResources->GBufferC, m_GlobalResources->GBufferD};
    for(auto &texture : gbufferTextures) {
        commandList->beginTrackingTextureState(texture, nvrhi::AllSubresources,
                                               nvrhi::ResourceStates::ShaderResource);
        commandList->setTextureState(texture, nvrhi::AllSubresources,
                                     nvrhi::ResourceStates::UnorderedAccess);
    }
    commandList->commitBarriers();

    nvrhi::rt::State state;
    state.shaderTable = m_GBufferShaderTable;
    state.bindings = {m_GlobalResources->BindingSet,
                      m_GlobalResources->SceneDescriptorTable,
                      m_GlobalResources->FixedDescriptorTable};
    commandList->setRayTracingState(state);
    m_GlobalResources->SetDefaultRootConstants(commandList);

    nvrhi::rt::DispatchRaysArguments args;
    args.width = m_GlobalResources->Size.x;
    args.height = m_GlobalResources->Size.y;
    commandList->dispatchRays(args);

    for (auto& texture : gbufferTextures) {
        commandList->setTextureState(texture, nvrhi::AllSubresources,
                                     nvrhi::ResourceStates::ShaderResource);
    }
    commandList->commitBarriers();

    commandList->endMarker();
}
