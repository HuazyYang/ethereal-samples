#include "RTAOPass.h"
#include "GlobalResources.h"
#include <donut/engine/ShaderFactory.h>
#include "Config.h"

bool RTAOPass::Initialize(GlobalResources* resources,
                                donut::engine::ShaderFactory* shaderFactory) {
    m_GlobalResources = resources;
    return CreatePipelines(shaderFactory);
}

void RTAOPass::Update(const Config* config) {
    m_Enabled = config->renderers.rtao.enabled;

    if(m_Enabled) {
        // Only place GlobalConstants is written into
        auto& globalConsts = m_GlobalResources->GlobalConsts;
        globalConsts.rtao.rayLength = config->renderers.rtao.rayLength;
        globalConsts.rtao.rayNormalBias = config->renderers.rtao.rayNormalBias;
        globalConsts.rtao.rayViewBias = config->renderers.rtao.rayViewBias;
        globalConsts.rtao.power = std::pow(2.f, config->renderers.rtao.powerLog);
        globalConsts.rtao.filterDistanceSigma = config->renderers.rtao.filterDistanceSigma;
        globalConsts.rtao.filterDepthSigma = config->renderers.rtao.filterDepthSigma;
        globalConsts.rtao.filterBufferWidth = m_GlobalResources->Size.x;
        globalConsts.rtao.filterBufferHeight = m_GlobalResources->Size.y;

        float distanceKernel[6];
        for (int i = 0; i < 6; ++i)
            distanceKernel[i] =
                std::exp(-float(i * i) / (2.f * config->renderers.rtao.filterDistanceSigma *
                                          config->renderers.rtao.filterDistanceSigma));

        globalConsts.rtao.filterDistKernel0 = distanceKernel[0];
        globalConsts.rtao.filterDistKernel1 = distanceKernel[1];
        globalConsts.rtao.filterDistKernel2 = distanceKernel[2];
        globalConsts.rtao.filterDistKernel3 = distanceKernel[3];
        globalConsts.rtao.filterDistKernel4 = distanceKernel[4];
        globalConsts.rtao.filterDistKernel5 = distanceKernel[5];

        m_GlobalResources->SetGlobalConstsDirty();
    }
}

void RTAOPass::Execute(nvrhi::ICommandList* commandList) {
    if(!m_Enabled)
        return;

    commandList->beginMarker("RTAO");

    {
        commandList->beginTrackingTextureState(m_GlobalResources->RTAORawTexture,
                                               nvrhi::AllSubresources,
                                               nvrhi::ResourceStates::ShaderResource);
        commandList->setTextureState(m_GlobalResources->RTAORawTexture,
                                     nvrhi::AllSubresources,
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

        commandList->setTextureState(m_GlobalResources->RTAORawTexture,
                                     nvrhi::AllSubresources,
                                     nvrhi::ResourceStates::ShaderResource);
        commandList->commitBarriers();
    }

    {
        commandList->beginTrackingTextureState(m_GlobalResources->RTAOOutputTexture,
                                               nvrhi::AllSubresources,
                                               nvrhi::ResourceStates::ShaderResource);
        commandList->setTextureState(m_GlobalResources->RTAOOutputTexture,
                                     nvrhi::AllSubresources,
                                     nvrhi::ResourceStates::UnorderedAccess);
        commandList->commitBarriers();

        nvrhi::ComputeState state;
        state.pipeline = m_AOFilterPipeline;
        state.bindings = {m_GlobalResources->BindingSet, m_GlobalResources->SceneDescriptorTable,
                          m_GlobalResources->FixedDescriptorTable};
        commandList->setComputeState(state);
        m_GlobalResources->SetDefaultRootConstants(commandList);

        constexpr dm::uint RTAO_FILTER_BLOCK_SIZE = 8;
        const dm::uint numGroupsX =
            dm::div_ceil(m_GlobalResources->Size.x, RTAO_FILTER_BLOCK_SIZE);
        const dm::uint numGroupsY =
            dm::div_ceil(m_GlobalResources->Size.y, RTAO_FILTER_BLOCK_SIZE);

        commandList->dispatch(numGroupsX, numGroupsY);

        commandList->setTextureState(m_GlobalResources->RTAOOutputTexture,
                                     nvrhi::AllSubresources,
                                     nvrhi::ResourceStates::ShaderResource);
        commandList->commitBarriers();
    }

    commandList->endMarker();
}

bool RTAOPass::CreatePipelines(donut::engine::ShaderFactory* shaderFactory) {
    std::vector<donut::engine::ShaderMacro> defines = {{"HLSL", "1"}};
    {
        auto raygenShaderLibrary =
            shaderFactory->CreateShaderLibrary("app/RTAOTraceRGS.hlsl", &defines);
        auto missShaderLibrary =
            shaderFactory->CreateShaderLibrary("app/Miss.hlsl", &defines);
        auto closestShaderLibrary =
            shaderFactory->CreateShaderLibrary("app/CHS.hlsl", &defines);
        auto anyhitShaderLibrary =
            shaderFactory->CreateShaderLibrary("app/AHS.hlsl", &defines);

        nvrhi::rt::PipelineDesc psoDesc;
        psoDesc.globalBindingLayouts = {m_GlobalResources->BindingLayout,
                                        m_GlobalResources->BindlessLayout,
                                        m_GlobalResources->FixedBindlessLayout};

        psoDesc.shaders = {
            {"RTAOTraceRGS",
             raygenShaderLibrary->getShader("RayGen", nvrhi::ShaderType::RayGeneration),
             nullptr},
            {"RTAOMiss", missShaderLibrary->getShader("Miss", nvrhi::ShaderType::Miss),
             nullptr}};
        psoDesc.hitGroups = {
            {"RTAOHitGroup",
             closestShaderLibrary->getShader("CHS_VISIBILITY",
                                             nvrhi::ShaderType::ClosestHit),
             anyhitShaderLibrary->getShader("AHS_GI", nvrhi::ShaderType::AnyHit), nullptr,
             nullptr, false}};

        psoDesc.maxPayloadSize = sizeof(Graphics::PackedPayload);

        auto pipeline = m_GlobalResources->Device->createRayTracingPipeline(psoDesc);
        if (!pipeline) return false;

        m_ShaderTable = pipeline->createShaderTable();
        if (!m_ShaderTable) return false;

        m_ShaderTable->setRayGenerationShader("RTAOTraceRGS");
        m_ShaderTable->addHitGroup("RTAOHitGroup");
        m_ShaderTable->addMissShader("RTAOMiss");
    }

    {
        auto cs = shaderFactory->CreateShader("app/RTAOFilterCS.hlsl", "CS", &defines,
                                              nvrhi::ShaderType::Compute);
        nvrhi::ComputePipelineDesc psoDesc;
        psoDesc.bindingLayouts = { m_GlobalResources->BindingLayout, m_GlobalResources->BindlessLayout, m_GlobalResources->FixedBindlessLayout };
        psoDesc.CS = cs;

        m_AOFilterPipeline = m_GlobalResources->Device->createComputePipeline(psoDesc);
        if (!m_AOFilterPipeline) return false;
    }

    return true;
}
