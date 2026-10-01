#include "PathTracingPass.h"
#include <donut/engine/ShaderFactory.h>
#include "GlobalResources.h"
#include "Config.h"

PathTracingPass::PathTracingPass() {}

bool PathTracingPass::Initialize(GlobalResources* resources,
                                 donut::engine::ShaderFactory* shaderFactory) {
    m_GlobalResources = resources;
    CreateRTPipeline(shaderFactory);

    return true;
}

void PathTracingPass::Update(const Config* config) {
    auto& globalConsts = m_GlobalResources->GlobalConsts;

    globalConsts.pt.rayNormalBias = config->renderers.pt.rayNormalBias;
    globalConsts.pt.rayViewBias = config->renderers.pt.rayViewBias;
    globalConsts.pt.numBounces = config->renderers.pt.numBounces;
    globalConsts.pt.samplesPerPixel = config->renderers.pt.samplersPerPixel;
    globalConsts.pt.SetAntialiasing(config->renderers.pt.antialiasing);
    globalConsts.pt.SetProgressive(true);
    globalConsts.pt.SetShaderExcutionRecording(false);

    globalConsts.post.useFlags = Graphics::POSTPROCESS_FLAG_USE_NONE;
    if (config->renderers.pp.exposure.enabled) {
        globalConsts.post.exposure = std::pow(2.f, config->renderers.pp.exposure.fstops);
        globalConsts.post.useFlags |= Graphics::POSTPROCESS_FLAG_USE_EXPOSURE;
    }
    if (config->renderers.pp.tonemapping.enabled)
        globalConsts.post.useFlags |= Graphics::POSTPROCESS_FLAG_USE_TONEMAPPING;
    if (config->renderers.pp.dithering.enabled)
        globalConsts.post.useFlags |= Graphics::POSTPROCESS_FLAG_USE_DITHER;
    if (config->renderers.pp.gammaCorrection.enabled)
        globalConsts.post.useFlags |= Graphics::POSTPROCESS_FLAG_USE_GAMMA;

    m_GlobalResources->SetGlobalConstsDirty();
}

void PathTracingPass::Execute(nvrhi::ICommandList* commandList) {
    commandList->beginMarker("Path Tracing");

    commandList->beginTrackingTextureState(m_GlobalResources->PTOutputTexture,
                                           nvrhi::AllSubresources,
                                           nvrhi::ResourceStates::ShaderResource);
    commandList->setTextureState(m_GlobalResources->PTOutputTexture, nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::UnorderedAccess);
    commandList->commitBarriers();

    nvrhi::rt::State state;
    state.shaderTable = m_ShaderTable;
    state.bindings = {m_GlobalResources->BindingSet,
                      m_GlobalResources->SceneDescriptorTable,
                      m_GlobalResources->FixedDescriptorTable};
    commandList->setRayTracingState(state);
    m_GlobalResources->SetDefaultRootConstants(commandList);

    nvrhi::rt::DispatchRaysArguments args;
    args.width = m_GlobalResources->Size.x;
    args.height = m_GlobalResources->Size.y;
    commandList->dispatchRays(args);

    commandList->setTextureState(m_GlobalResources->PTOutputTexture, nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::ShaderResource);
    commandList->commitBarriers();

    commandList->endMarker();
}

bool PathTracingPass::CreateRTPipeline(donut::engine::ShaderFactory* shaderFactory) {
    std::vector<donut::engine::ShaderMacro> defines = {{"HLSL", "1"}};
    auto raygenShaderLibrary =
        shaderFactory->CreateShaderLibrary("app/PathTraceRGS.hlsl", &defines);
    auto missShaderLibrary = shaderFactory->CreateShaderLibrary("app/Miss.hlsl", &defines);
    auto closestShaderLibrary =
        shaderFactory->CreateShaderLibrary("app/CHS.hlsl", &defines);
    auto anyhitShaderLibrary = shaderFactory->CreateShaderLibrary("app/AHS.hlsl", &defines);

    nvrhi::rt::PipelineDesc psoDesc;
    psoDesc.globalBindingLayouts = {m_GlobalResources->BindingLayout,
                                    m_GlobalResources->BindlessLayout,
                                    m_GlobalResources->FixedBindlessLayout};

    psoDesc.shaders = {
        {"RayGen",
         raygenShaderLibrary->getShader("RayGen", nvrhi::ShaderType::RayGeneration),
         nullptr},
        {"Miss", missShaderLibrary->getShader("Miss", nvrhi::ShaderType::Miss), nullptr}};
    psoDesc.hitGroups = {
        {"HitGroup",
         closestShaderLibrary->getShader("CHS_LOD0", nvrhi::ShaderType::ClosestHit),
         anyhitShaderLibrary->getShader("AHS_LOD0", nvrhi::ShaderType::AnyHit), nullptr,
         nullptr, false}};

    psoDesc.maxPayloadSize = sizeof(Graphics::PackedPayload);

    auto pipeline = m_GlobalResources->Device->createRayTracingPipeline(psoDesc);
    if (!pipeline) return false;

    m_ShaderTable = pipeline->createShaderTable();
    if (!m_ShaderTable) return false;

    m_ShaderTable->setRayGenerationShader("RayGen");
    m_ShaderTable->addHitGroup("HitGroup");
    m_ShaderTable->addMissShader("Miss");
    return true;
}
