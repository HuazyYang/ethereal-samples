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

        psoDesc.shaders = {
            {"DDGIVisProbesRGS",
             raygenShaderLibrary->getShader("RayGen", nvrhi::ShaderType::RayGeneration),
             nullptr},
            {"DDGIVisProbesMiss",
             missShaderLibrary->getShader("Miss", nvrhi::ShaderType::Miss), nullptr}};
        psoDesc.hitGroups = {
            {"DDGIVisProbesHitGroup",
             closestShaderLibrary->getShader("CHS", nvrhi::ShaderType::ClosestHit), nullptr,
             nullptr, nullptr, false}};

        psoDesc.maxPayloadSize = sizeof(Graphics::PackedPayload);

        auto pipeline = m_GlobalResources->Device->createRayTracingPipeline(psoDesc);
        if (!pipeline) return false;

        m_ShaderTable = pipeline->createShaderTable();
        if (!m_ShaderTable) return false;

        m_ShaderTable->setRayGenerationShader("DDGIVisProbesRGS");
        m_ShaderTable->addHitGroup("DDGIVisProbesHitGroup");
        m_ShaderTable->addMissShader("DDGIVisProbesMiss");

        psoDesc.shaders[0] = {"DDGIVisProbesRGS",
                              raygenShaderLibrary->getShader(
                                  "RayGenHideInactive", nvrhi::ShaderType::RayGeneration),
                              nullptr};
        pipeline = m_GlobalResources->Device->createRayTracingPipeline(psoDesc);
        if (!pipeline) return false;

        m_HideInactiveShaderTable = pipeline->createShaderTable();
        if (!m_HideInactiveShaderTable) return false;

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
        m_VisTexturesPipeline = m_GlobalResources->Device->createComputePipeline(psoDesc);
        if(!m_VisTexturesPipeline) return false;
    }

    return true;
}
