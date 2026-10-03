#include "DDGIVisualizationPass.h"
#include <donut/engine/ShaderFactory.h>
#include "GlobalResources.h"
#include "Config.h"

bool DDGIVisualizationPass::Initialize(GlobalResources* resources,
                                       donut::engine::ShaderFactory* shaderFactory) {
    m_GlobalResources = resources;
    return CreatePipelines(shaderFactory);
}

void DDGIVisualizationPass::Update(const Config* config) {}

void DDGIVisualizationPass::Execute(nvrhi::ICommandList* commandList) {}

bool DDGIVisualizationPass::CreatePipelines(donut::engine::ShaderFactory* shaderFactory) {
    std::vector<donut::engine::ShaderMacro> defines = {{"HLSL", "1"}};

    {
        auto raygenShaderLibrary = shaderFactory->CreateShaderLibrary(
            "app/visualizations/ProobesRGS.hlsl", &defines);
        auto missShaderLibrary = shaderFactory->CreateShaderLibrary(
            "app/visualizaitons/ProbesMiss.hlsl", &defines);
        auto closestShaderLibrary = shaderFactory->CreateShaderLibrary(
            "app/visualizations/ProbesCHS.hlsl", &defines);

        nvrhi::rt::PipelineDesc psoDesc;
        psoDesc.globalBindingLayouts = {m_GlobalResources->BindingLayout, m_GlobalResources->BindlessLayout,
                                        m_GlobalResources->FixedBindlessLayout};

        nvrhi::ShaderHandle rayGenShader;
        raygenShaderLibrary->getShader("RayGen", nvrhi::ShaderType::RayGeneration, &rayGenShader);
        nvrhi::ShaderHandle missShader;
        missShaderLibrary->getShader("Miss", nvrhi::ShaderType::Miss, &missShader);
        psoDesc.shaders = {
            {"DDGIVisProbesRGS",
             rayGenShader,
             nullptr},
            {"DDGIVisProbesMiss",
             missShader, nullptr}};
        nvrhi::ShaderHandle chsShader;
        closestShaderLibrary->getShader("CHS", nvrhi::ShaderType::ClosestHit, &chsShader);
        psoDesc.hitGroups = {
            {"DDGIVisProbesHitGroup",
             chsShader, nullptr,
             nullptr, nullptr, false}};

        psoDesc.maxPayloadSize = sizeof(Graphics::PackedPayload);

        nvrhi::rt::PipelineHandle pipeline;
        if (NVRHI_FAILED(m_GlobalResources->Device->createRayTracingPipeline(psoDesc, &pipeline))) return false;

        if (NVRHI_FAILED(pipeline->createShaderTable(nvrhi::rt::ShaderTableDesc(), &m_ShaderTable))) return false;

        m_ShaderTable->setRayGenerationShader("DDGIVisProbesRGS");
        m_ShaderTable->addHitGroup("DDGIVisProbesHitGroup");
        m_ShaderTable->addMissShader("DDGIVisProbesMiss");

        nvrhi::ShaderHandle rayGenHideInactiveShader;
        raygenShaderLibrary->getShader("RayGenHideInactive", nvrhi::ShaderType::RayGeneration, &rayGenHideInactiveShader);
        psoDesc.shaders[0] = {"DDGIVisProbesRGS",
                              rayGenHideInactiveShader,
                              nullptr};
        if (NVRHI_FAILED(m_GlobalResources->Device->createRayTracingPipeline(psoDesc, &pipeline))) return false;

        if (NVRHI_FAILED(pipeline->createShaderTable(nvrhi::rt::ShaderTableDesc(), &m_HideInactiveShaderTable))) return false;

        m_HideInactiveShaderTable->setRayGenerationShader("DDGIVisProbesRGS");
        m_HideInactiveShaderTable->addHitGroup("DDGIVisProbesHitGroup");
        m_HideInactiveShaderTable->addMissShader("DDGIVisProbesMiss");
    }

    // Volume textures pipeline
    {
        auto cs = shaderFactory->CreateShader("app/visualizations/VolumeTexturesCS.hlsl",
                                              "CS", &defines, nvrhi::ShaderType::Compute);
        nvrhi::ComputePipelineDesc psoDesc;
        psoDesc.CS = cs;
        psoDesc.bindingLayouts = {m_GlobalResources->BindingLayout, m_GlobalResources->BindlessLayout,
                                  m_GlobalResources->FixedBindlessLayout};
        if (NVRHI_FAILED(m_GlobalResources->Device->createComputePipeline(psoDesc, &m_VisTexturesPipeline))) return false;
    }

    return true;
}
