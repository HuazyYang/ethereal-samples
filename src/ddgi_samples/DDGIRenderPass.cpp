#include "DDGIRenderPass.h"
#include <donut/engine/ShaderFactory.h>
#include "GlobalResources.h"

bool DDGIRenderPass::Initialize(GlobalResources* resources,
                                donut::engine::ShaderFactory* shaderFactory) {
    m_GlobalResources = resources;
    CreatePipelines(shaderFactory);
    return false;
}

void DDGIRenderPass::Update(const Config* config) {
    m_SelectedVolumeIndices.clear();

    for (dm::uint volumeIndex = 0; volumeIndex < m_GlobalResources->GetNumVolumes(); ++volumeIndex) {
        auto volume = &m_GlobalResources->DDGIVolumes[volumeIndex];

        // Don't update volumes whose variability measurement is low enough to be considered
        // converged. Enforce a minimum of 16 samples to filter out early outliers
        const uint32_t MinimumVariabilitySamples = 16;
        float volumeAverageVariability = volume->GetVolumeAverageVariability();
        bool isConverged =
            volume->GetProbeVariabilityEnabled() &&
            (volume->AdvanceNumVariabilitySamples() > MinimumVariabilitySamples) &&
            volumeAverageVariability < volume->GetDesc().probeVariabilityThreshold;

        // Add the volume to the list of volumes to update (it hasn't converged)
        if (!isConverged) m_SelectedVolumeIndices.push_back(volumeIndex);
    }

    for (auto& volumeIndex : m_SelectedVolumeIndices) {
        m_GlobalResources->DDGIVolumes[volumeIndex].Update();
    }
}

void DDGIRenderPass::Execute(nvrhi::ICommandList* commandList) {
    commandList->beginMarker("DDGI");
    ExecuteRayTraceVolumes(commandList);
    ExecuteUpdateDDGIVolumeProbes(commandList);
    ExecuteRelocateDDGIVolumeProbes(commandList);
    ExecuteClassifyDDGIVolumeProbes(commandList);
    ExecuteCalculateDDGIVolumeVariability(commandList);
    ExecuteReadbackDDGIVolumeVariability(commandList);
    ExecuteGatherIndirectLighting(commandList);
    commandList->endMarker();
}

bool DDGIRenderPass::CreatePipelines(donut::engine::ShaderFactory* shaderFactory) {
    auto device = m_GlobalResources->Device;

    std::vector<donut::engine::ShaderMacro> defines{{"HLSL", "1"}};
    // ProbeTrace
    {
        auto raygenShaderLib =
            shaderFactory->CreateShaderLibrary("app/ProbeTraceRGS.hlsl", &defines);
        auto missShaderLib = shaderFactory->CreateShaderLibrary("app/Miss.hlsl", &defines);
        auto closesthitShaderLib =
            shaderFactory->CreateShaderLibrary("app/CHS.hlsl", &defines);
        auto anyhitShaderLib = shaderFactory->CreateShaderLibrary("app/AHS.hlsl", &defines);

        nvrhi::rt::PipelineDesc psoDesc;
        psoDesc.globalBindingLayouts = {m_GlobalResources->BindingLayout,
                                        m_GlobalResources->BindlessLayout,
                                        m_GlobalResources->FixedBindlessLayout};
        nvrhi::ShaderHandle rayGenShader;
        raygenShaderLib->getShader("RayGen", nvrhi::ShaderType::RayGeneration, &rayGenShader);
        nvrhi::ShaderHandle missShader;
        missShaderLib->getShader("Miss", nvrhi::ShaderType::Miss, &missShader);
        psoDesc.shaders = {
            {"DDGIProbeTraceRGS",
             rayGenShader,
             nullptr},
            {"DDGIProbeTraceMiss",
             missShader, nullptr},
        };
        nvrhi::ShaderHandle chsGiShader;
        closesthitShaderLib->getShader("CHS_GI", nvrhi::ShaderType::ClosestHit, &chsGiShader);
        nvrhi::ShaderHandle ahsGiShader;
        anyhitShaderLib->getShader("AHS_GI", nvrhi::ShaderType::AnyHit, &ahsGiShader);
        psoDesc.hitGroups = {
            {"DDGIProbeTraceHitGroup",
             chsGiShader,
             ahsGiShader, nullptr,
             nullptr, false}};
        psoDesc.maxPayloadSize = sizeof(Graphics::PackedPayload);

        nvrhi::rt::PipelineHandle pipeline;
        if (NVRHI_FAILED(device->createRayTracingPipeline(psoDesc, &pipeline))) return false;

        pipeline->createShaderTable(nvrhi::rt::ShaderTableDesc(), &m_ProbeTraceShaderTable);
        m_ProbeTraceShaderTable->setRayGenerationShader("DDGIProbeTraceRGS");
        m_ProbeTraceShaderTable->addHitGroup("DDGIProbeTraceHitGroup");
        m_ProbeTraceShaderTable->addMissShader("DDGIProbeTraceMiss");
    }

    nvrhi::ComputePipelineDesc psoDesc;
    psoDesc.bindingLayouts = {m_GlobalResources->BindingLayout, m_GlobalResources->BindlessLayout,
                              m_GlobalResources->FixedBindlessLayout};

    // IrradianceCS
    {
        auto indirectCS = shaderFactory->CreateShader("app/IndirectCS.hlsl", "CS", &defines,
                                                      nvrhi::ShaderType::Compute);
        psoDesc.CS = indirectCS;

        device->createComputePipeline(psoDesc, &m_IndirectCSPipeline);
    }

    // Probe Blending
    {
        // Probe Blending (irradiance)
        std::vector<donut::engine::ShaderMacro> blendingDefines = {
            {"HLSL", "1"},
            {"RTXGI_DDGI_BLEND_RADIANCE", "1"},
            {"RTXGI_DDGI_PROBE_NUM_TEXELS", "8"},
            {"RTXGI_DDGI_PROBE_NUM_INTERIOR_TEXELS", "6"}};

        auto cs = shaderFactory->CreateShader("app/ddgi/ProbeBlendingCS.hlsl",
                                              "DDGIProbeBlendingCS", &blendingDefines,
                                              nvrhi::ShaderType::Compute);
        psoDesc.CS = cs;
        device->createComputePipeline(psoDesc, &m_ProbeBlendCSPipelines[0]);

        // Probe Blending (distance)
        blendingDefines[1].definition = "0";
        blendingDefines[2].definition = "16";
        blendingDefines[3].definition = "14";
        cs = shaderFactory->CreateShader("app/ddgi/ProbeBlendingCS.hlsl",
                                         "DDGIProbeBlendingCS", &blendingDefines,
                                         nvrhi::ShaderType::Compute);
        psoDesc.CS = cs;
        device->createComputePipeline(psoDesc, &m_ProbeBlendCSPipelines[1]);
    }

    // Probe Relocation
    {
        auto cs = shaderFactory->CreateShader("app/ddgi/ProbeRelocationCS.hlsl",
                                              "DDGIProbeRelocationCS", &defines,
                                              nvrhi::ShaderType::Compute);
        psoDesc.CS = cs;
        device->createComputePipeline(psoDesc, &m_ProbeRelocationCSPipeline);

        cs = shaderFactory->CreateShader("app/ddgi/ProbeRelocationCS.hlsl",
                                         "DDGIProbeRelocationResetCS", &defines,
                                         nvrhi::ShaderType::Compute);
        psoDesc.CS = cs;
        device->createComputePipeline(psoDesc, &m_ProbeRelocationResetCSPipeline);
    }

    // Probe Classification
    {
        auto cs = shaderFactory->CreateShader("app/ddgi/ProbeClassificationCS.hlsl",
                                              "DDGIProbeClassificationCS", &defines,
                                              nvrhi::ShaderType::Compute);
        psoDesc.CS = cs;
        device->createComputePipeline(psoDesc, &m_ProbeClassificationCSPipeline);
        cs = shaderFactory->CreateShader("app/ddgi/ProbeClassificationCS.hlsl",
                                         "DDGIProbeClassificationResetCS", &defines,
                                         nvrhi::ShaderType::Compute);
        psoDesc.CS = cs;
        device->createComputePipeline(psoDesc, &m_ProbeClassificationResetCSPipeline);
    }

    nvrhi::WaveLaneCountMinMaxFeatureInfo info;
    bool waveLaneSupported = device->queryFeatureSupport(
        nvrhi::Feature::WaveLaneCountMinMax, &info, sizeof(info));
    assert(waveLaneSupported);
    defines.emplace_back("RTXGI_DDGI_WAVE_LANE_COUNT",
                         std::to_string(info.minWaveLaneCount));

    // Probe variability reduction
    {
        auto cs = shaderFactory->CreateShader("app/ddgi/ReductionCS.hlsl", "DDGIReductionCS",
                                              &defines, nvrhi::ShaderType::Compute);
        psoDesc.CS = cs;
        device->createComputePipeline(psoDesc, &m_ReductionCSPipeline);
    }

    // Extra reduction
    {
        auto cs =
            shaderFactory->CreateShader("app/ddgi/ReductionCS.hlsl", "DDGIExtraReductionCS",
                                        &defines, nvrhi::ShaderType::Compute);
        psoDesc.CS = cs;
        device->createComputePipeline(psoDesc, &m_ExtraReductionCSPipeline);
    }

    return true;
}

void DDGIRenderPass::ExecuteRayTraceVolumes(nvrhi::ICommandList* comandList) {
    comandList->beginMarker("DDGI: Ray Trace DDGIVolumes");

    nvrhi::rt::State state;
    state.bindings = { m_GlobalResources->DDGIBindingSet, m_GlobalResources->SceneDescriptorTable, m_GlobalResources->FixedDescriptorTable };
    state.shaderTable = m_ProbeTraceShaderTable;
    comandList->setRayTracingState(state);

    dm::uint width, height, depth;
    nvrhi::rt::DispatchRaysArguments args;

    for(auto volumeIndex : m_SelectedVolumeIndices) {
        ddgi::DDGIRootConstants constants;
        constants.volumeIndex = volumeIndex;
        comandList->setPushConstants(&constants, sizeof(constants));

        auto& resources = m_GlobalResources->DDGIVolumesResources[volumeIndex];
        m_GlobalResources->DDGIVolumes[volumeIndex].GetRayDispatchDimensions(width, height,
                                                                             depth);
        args.width = width;
        args.height = height;
        args.depth = depth;
        comandList->dispatchRays(args);
    }

    for(auto volumeIndex : m_SelectedVolumeIndices) {
        auto& resources = m_GlobalResources->DDGIVolumesResources[volumeIndex];
        comandList->beginTrackingTextureState(resources.rayDataTexture,
                                              nvrhi::AllSubresources,
                                              nvrhi::ResourceStates::UnorderedAccess);
        comandList->setTextureState(resources.rayDataTexture, nvrhi::AllSubresources,
                                    nvrhi::ResourceStates::UnorderedAccess);
    }
    comandList->commitBarriers();

    comandList->endMarker();
}

void DDGIRenderPass::ExecuteUpdateDDGIVolumeProbes(nvrhi::ICommandList* commandList) {
    commandList->beginMarker("DDGI: Update Irradiance");

    nvrhi::ComputeState state;
    state.bindings = {m_GlobalResources->DDGIBindingSet, m_GlobalResources->SceneDescriptorTable,
                      m_GlobalResources->FixedDescriptorTable};
    state.pipeline = m_ProbeBlendCSPipelines[0];
    commandList->setComputeState(state);

    dm::uint width, height, depth;

    // Probe irradiance blending
    for(auto volumeIndex : m_SelectedVolumeIndices) {
        ddgi::DDGIRootConstants constants;
        constants.volumeIndex = volumeIndex;
        commandList->setPushConstants(&constants, sizeof(constants));
        ddgi::GetDDGIVolumeProbeCounts(m_GlobalResources->DDGIVolumes[volumeIndex].GetDesc(), width,
                                       height, depth);
        commandList->dispatch(width, height, depth);
    }

    for(auto volumeIndex : m_SelectedVolumeIndices) {
        auto& resources = m_GlobalResources->DDGIVolumesResources[volumeIndex];
        commandList->beginTrackingTextureState(resources.probeIrradianceTexture,
                                               nvrhi::AllSubresources,
                                               nvrhi::ResourceStates::UnorderedAccess);
        commandList->setTextureState(resources.probeIrradianceTexture,
                                     nvrhi::AllSubresources,
                                     nvrhi::ResourceStates::UnorderedAccess);
        commandList->beginTrackingTextureState(resources.probeVariabilityTexture,
                                               nvrhi::AllSubresources,
                                               nvrhi::ResourceStates::UnorderedAccess);
        commandList->setTextureState(resources.probeVariabilityTexture,
                                     nvrhi::AllSubresources,
                                     nvrhi::ResourceStates::UnorderedAccess);
    }
    commandList->commitBarriers();

    commandList->endMarker();

    commandList->beginMarker("DDGI: Update Distances");

    // Probe distance blending
    state.pipeline = m_ProbeBlendCSPipelines[1];
    commandList->setComputeState(state);

    for(auto volumeIndex : m_SelectedVolumeIndices) {
        ddgi::DDGIRootConstants constants;
        constants.volumeIndex = volumeIndex;
        commandList->setPushConstants(&constants, sizeof(constants));
        ddgi::GetDDGIVolumeProbeCounts(
            m_GlobalResources->DDGIVolumes[volumeIndex].GetDesc(), width, height, depth);
        commandList->dispatch(width, height, depth);
    }

    for (auto volumeIndex : m_SelectedVolumeIndices) {
        auto& resources = m_GlobalResources->DDGIVolumesResources[volumeIndex];
        commandList->beginTrackingTextureState(resources.probeDistanceTexture,
                                               nvrhi::AllSubresources,
                                               nvrhi::ResourceStates::UnorderedAccess);
        commandList->setTextureState(resources.probeDistanceTexture, nvrhi::AllSubresources,
                                     nvrhi::ResourceStates::UnorderedAccess);
    }
    commandList->commitBarriers();

    commandList->endMarker();
}

void DDGIRenderPass::ExecuteRelocateDDGIVolumeProbes(nvrhi::ICommandList* commandList) {
    commandList->beginMarker("DDGI: Relocate Probes");

    nvrhi::ComputeState state;
    state.bindings = {m_GlobalResources->DDGIBindingSet, m_GlobalResources->SceneDescriptorTable,
                      m_GlobalResources->FixedDescriptorTable};
    state.pipeline = m_ProbeRelocationResetCSPipeline;
    commandList->setComputeState(state);

    dm::uint width, height, depth;

    // Probe Relocation Reset
    for(auto volumeIndex : m_SelectedVolumeIndices) {
        auto volume = &m_GlobalResources->DDGIVolumes[volumeIndex];

        if (!volume->GetProbeRelocationNeedsReset()) continue;

        auto& resources = m_GlobalResources->DDGIVolumesResources[volumeIndex];
        commandList->beginTrackingTextureState(resources.probeDataTexture,
                                               nvrhi::AllSubresources,
                                               nvrhi::ResourceStates::UnorderedAccess);

        ddgi::DDGIRootConstants constants;
        constants.volumeIndex = volumeIndex;
        commandList->setPushConstants(&constants, sizeof(constants));

        const dm::uint groupSizeX = 32;
        dm::uint numGroupX = (volume->GetNumProbes() + groupSizeX - 1) / groupSizeX;
        commandList->dispatch(numGroupX);

        volume->SetProbeClassificationNeedsReset(false);

        commandList->setTextureState(resources.probeDataTexture, nvrhi::AllSubresources,
                                     nvrhi::ResourceStates::UnorderedAccess);
    }
    commandList->commitBarriers();

    // Probe Relocation
    state.pipeline = m_ProbeRelocationCSPipeline;
    commandList->setComputeState(state);

    for(auto volumeIndex : m_SelectedVolumeIndices) {
        auto volume = &m_GlobalResources->DDGIVolumes[volumeIndex];

        if (!volume->GetProbeRelocationEnabled()) continue;

        auto& resources = m_GlobalResources->DDGIVolumesResources[volumeIndex];
        commandList->beginTrackingTextureState(resources.probeDataTexture,
                                               nvrhi::AllSubresources,
                                               nvrhi::ResourceStates::UnorderedAccess);

        ddgi::DDGIRootConstants constants;
        constants.volumeIndex = volumeIndex;
        commandList->setPushConstants(&constants, sizeof(constants));

        const dm::uint groupSizeX = 32;
        dm::uint numGroupX = (volume->GetNumProbes() + groupSizeX - 1) / groupSizeX;
        commandList->dispatch(numGroupX);

        commandList->setTextureState(resources.probeDataTexture, nvrhi::AllSubresources,
                                     nvrhi::ResourceStates::UnorderedAccess);
    }
    commandList->commitBarriers();

    commandList->endMarker();
}

void DDGIRenderPass::ExecuteClassifyDDGIVolumeProbes(nvrhi::ICommandList* commandList) {
    commandList->beginMarker("DDGI: Classify Probes");

    nvrhi::ComputeState state;
    state.bindings = {m_GlobalResources->DDGIBindingSet, m_GlobalResources->SceneDescriptorTable,
                      m_GlobalResources->FixedDescriptorTable};
    state.pipeline = m_ProbeClassificationResetCSPipeline;
    commandList->setComputeState(state);

    for(auto volumeIndex : m_SelectedVolumeIndices) {
        auto volume = &m_GlobalResources->DDGIVolumes[volumeIndex];

        if(!volume->GetProbeClassificationNeedsReset()) continue;

        auto& resources = m_GlobalResources->DDGIVolumesResources[volumeIndex];
        commandList->beginTrackingTextureState(resources.probeDataTexture,
                                               nvrhi::AllSubresources,
                                               nvrhi::ResourceStates::UnorderedAccess);

        ddgi::DDGIRootConstants constants;
        constants.volumeIndex = volumeIndex;
        commandList->setPushConstants(&constants, sizeof(constants));

        const dm::uint groupSizeX = 32;
        dm::uint numGroupX = (volume->GetNumProbes() + groupSizeX - 1) / groupSizeX;
        commandList->dispatch(numGroupX);

        volume->SetProbeClassificationNeedsReset(false);

        commandList->setTextureState(resources.probeDataTexture, nvrhi::AllSubresources,
                                     nvrhi::ResourceStates::UnorderedAccess);
    }
    commandList->commitBarriers();

    state.pipeline = m_ProbeClassificationCSPipeline;
    commandList->setComputeState(state);

    for(auto volumeIndex : m_SelectedVolumeIndices) {
        auto volume = &m_GlobalResources->DDGIVolumes[volumeIndex];

        if(!volume->GetProbeClassificationEnabled()) continue;

        auto& resources = m_GlobalResources->DDGIVolumesResources[volumeIndex];
        commandList->beginTrackingTextureState(resources.probeDataTexture,
                                               nvrhi::AllSubresources,
                                               nvrhi::ResourceStates::UnorderedAccess);

        ddgi::DDGIRootConstants constants;
        constants.volumeIndex = volumeIndex;
        commandList->setPushConstants(&constants, sizeof(constants));

        const dm::uint groupSizeX = 32;
        dm::uint numGroupX = (volume->GetNumProbes() + groupSizeX - 1) / groupSizeX;
        commandList->dispatch(numGroupX);

        commandList->setTextureState(resources.probeDataTexture, nvrhi::AllSubresources,
                                     nvrhi::ResourceStates::UnorderedAccess);
    }
    commandList->commitBarriers();

    commandList->endMarker();
}

void DDGIRenderPass::ExecuteCalculateDDGIVolumeVariability(
    nvrhi::ICommandList* commandList) {
    commandList->beginMarker("DDGI: Compute Variability");

    // Reduction
    nvrhi::ComputeState reductionState, extraReductionState;
    reductionState.bindings = { m_GlobalResources->DDGIBindingSet, m_GlobalResources->SceneDescriptorTable, m_GlobalResources->FixedDescriptorTable };
    reductionState.pipeline = m_ReductionCSPipeline;
    extraReductionState.bindings = { m_GlobalResources->DDGIBindingSet, m_GlobalResources->SceneDescriptorTable, m_GlobalResources->FixedDescriptorTable };
    extraReductionState.pipeline = m_ExtraReductionCSPipeline;

    for (auto volumeIndex : m_SelectedVolumeIndices) {
        auto volume = &m_GlobalResources->DDGIVolumes[volumeIndex];

        if(!volume->GetProbeVariabilityEnabled()) continue;

        auto& resources = m_GlobalResources->DDGIVolumesResources[volumeIndex];
        commandList->beginTrackingTextureState(resources.probeVariabilityAverageTexture,
                                               nvrhi::AllSubresources,
                                               nvrhi::ResourceStates::UnorderedAccess);

        commandList->setComputeState(reductionState);

        ddgi::DDGIRootConstants constants;
        constants.volumeIndex = volumeIndex;

        dm::uint probeCountX, probeCountY, probeCountZ;
        ddgi::GetDDGIVolumeProbeCounts(volume->GetDesc(), probeCountX, probeCountY,
                                       probeCountZ);

        // Initially, the reduction input is the full variability size (same as irradiance
        // texture without border texels)
        dm::uint inputTexelsX =
            probeCountX * volume->GetDesc().probeNumIrradianceInteriorTexels;
        dm::uint inputTexelsY =
            probeCountY * volume->GetDesc().probeNumIrradianceInteriorTexels;
        dm::uint inputTexelsZ = probeCountZ;

        const dm::uint3 NumThreadsInGroup = {4, 8,
                                         4};  // Each thread group will have 8x8x8 threads
        constexpr dm::uint2 ThreadSampleFootprint = {4,
                                                 2};  // Each thread will sample 4x2 texels

        // One thread group per output texel
        dm::uint outputTexelsX =
            dm::div_ceil(inputTexelsX, NumThreadsInGroup.x * ThreadSampleFootprint.x);
        dm::uint outputTexelsY =
            dm::div_ceil(inputTexelsY, NumThreadsInGroup.y * ThreadSampleFootprint.y);
        dm::uint outputTexelsZ = dm::div_ceil(inputTexelsZ, NumThreadsInGroup.z);

        constants.reductionInputSizeX = inputTexelsX;
        constants.reductionInputSizeY = inputTexelsY;
        constants.reductionInputSizeZ = inputTexelsZ;
        commandList->setPushConstants(&constants, sizeof(constants));
        commandList->dispatch(outputTexelsX, outputTexelsY, outputTexelsZ);

        // Each thread group will write out a value to the averaging texture
        // If there is more than one thread group, we will need to do extra averaging passes
        inputTexelsX = outputTexelsX;
        inputTexelsY = outputTexelsY;
        inputTexelsZ = outputTexelsZ;

        commandList->setTextureState(resources.probeVariabilityAverageTexture,
                                     nvrhi::AllSubresources,
                                     nvrhi::ResourceStates::UnorderedAccess);
        commandList->commitBarriers();

        while (inputTexelsX > 1 || inputTexelsY > 1 || inputTexelsZ > 1) {
            commandList->setComputeState(extraReductionState);

            outputTexelsX =
                dm::div_ceil(inputTexelsX, NumThreadsInGroup.x * ThreadSampleFootprint.x);
            outputTexelsY =
                dm::div_ceil(inputTexelsY, NumThreadsInGroup.y * ThreadSampleFootprint.y);
            outputTexelsZ = dm::div_ceil(inputTexelsZ, NumThreadsInGroup.z);

            constants.reductionInputSizeX = inputTexelsX;
            constants.reductionInputSizeY = inputTexelsY;
            constants.reductionInputSizeZ = inputTexelsZ;
            commandList->setPushConstants(&constants, sizeof(constants));
            commandList->dispatch(outputTexelsX, outputTexelsY, outputTexelsZ);

            inputTexelsX = outputTexelsX;
            inputTexelsY = outputTexelsY;
            inputTexelsZ = outputTexelsZ;

            commandList->setTextureState(resources.probeVariabilityAverageTexture,
                                         nvrhi::AllSubresources,
                                         nvrhi::ResourceStates::UnorderedAccess);
            commandList->commitBarriers();
        }
    }

    for(auto volumeIndex : m_SelectedVolumeIndices) {
        auto volume = &m_GlobalResources->DDGIVolumes[volumeIndex];

        if (!volume->GetProbeVariabilityEnabled()) continue;

        auto& resources = m_GlobalResources->DDGIVolumesResources[volumeIndex];

        commandList->setTextureState(resources.probeVariabilityAverageTexture,
                                     nvrhi::AllSubresources,
                                     nvrhi::ResourceStates::CopySource);
        commandList->commitBarriers();

        commandList->copyTexture2(
            resources.probeVariabilityReadbackBuffer,
            nvrhi::TextureSlice().setWidth(1).setHeight(1).setDepth(1),
            resources.probeVariabilityAverageTexture,
            nvrhi::TextureSlice().setWidth(1).setHeight(1).setDepth(1));
    }

    // Restore variability texture state
    for (auto volumeIndex : m_SelectedVolumeIndices) {
        auto volume = &m_GlobalResources->DDGIVolumes[volumeIndex];

        if (!volume->GetProbeVariabilityEnabled()) continue;

        auto& resources = m_GlobalResources->DDGIVolumesResources[volumeIndex];

        commandList->setTextureState(resources.probeVariabilityAverageTexture,
                                     nvrhi::AllSubresources,
                                     nvrhi::ResourceStates::UnorderedAccess);
    }
    commandList->commitBarriers();

    commandList->endMarker();
}

void DDGIRenderPass::ExecuteReadbackDDGIVolumeVariability(
    nvrhi::ICommandList* commandList) {
    commandList->beginMarker("DDGI: Read Back Variability");

    for (dm::uint volumeIndex = 0; volumeIndex < m_GlobalResources->GetNumVolumes(); ++volumeIndex) {
        auto volume = &m_GlobalResources->DDGIVolumes[volumeIndex];

        if(!volume->GetProbeVariabilityEnabled()) continue;

        auto& resources = m_GlobalResources->DDGIVolumesResources[volumeIndex];
        size_t rowPitch;
        auto pMapped = (float *)commandList->getDevice()->mapStagingTexture(
            resources.probeVariabilityReadbackBuffer, nvrhi::TextureSlice{},
            nvrhi::CpuAccessMode::Read, rowPitch);

        volume->SetVolumeAverageVariability(pMapped[0]);

        commandList->getDevice()->unmapStagingTexture(
            resources.probeVariabilityReadbackBuffer);
    }

    commandList->endMarker();
}

void DDGIRenderPass::ExecuteGatherIndirectLighting(nvrhi::ICommandList* commandList) {

    commandList->beginMarker("DDGI: Gather Indirect Lighting");

    nvrhi::ComputeState state;
    state.bindings = {m_GlobalResources->DDGIBindingSet, m_GlobalResources->SceneDescriptorTable,
                      m_GlobalResources->FixedDescriptorTable};
    state.pipeline = m_IndirectCSPipeline;
    commandList->setComputeState(state);

    dm::uint groupX = dm::div_ceil(m_GlobalResources->Size.x, 8u);
    dm::uint groupY = dm::div_ceil(m_GlobalResources->Size.y, 4u);

    ddgi::DDGIRootConstants constants;
    commandList->setPushConstants(&constants, sizeof(constants));

    commandList->beginTrackingTextureState(m_GlobalResources->DDGIOutputTexture,
                                           nvrhi::AllSubresources,
                                           nvrhi::ResourceStates::ShaderResource);
    commandList->setTextureState(m_GlobalResources->DDGIOutputTexture,
                                 nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::UnorderedAccess);
    commandList->commitBarriers();

    commandList->dispatch(groupX, groupY, 1);

    commandList->setTextureState(m_GlobalResources->DDGIOutputTexture,
                                 nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::ShaderResource);
    commandList->commitBarriers();

    commandList->endMarker();
}
