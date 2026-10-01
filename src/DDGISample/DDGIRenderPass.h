#ifndef DDGIRENDERPASS_H
#define DDGIRENDERPASS_H
#include <nvrhi/nvrhi.h>

namespace donut::engine {
class ShaderFactory;
}

namespace ddgi {
class DDGIVolume;
};

struct Config;
class GlobalResources;

class DDGIRenderPass {
 public:
    bool Initialize(GlobalResources *resources,
                    donut::engine::ShaderFactory *shaderFactory);

    void Update(const Config *config);

    void Execute(nvrhi::ICommandList *commandList);

 private:
    bool CreatePipelines(donut::engine::ShaderFactory *shaderFactory);
    void ExecuteRayTraceVolumes(nvrhi::ICommandList *comandList);
    void ExecuteUpdateDDGIVolumeProbes(nvrhi::ICommandList *commandList);
    void ExecuteRelocateDDGIVolumeProbes(nvrhi::ICommandList *commandList);
    void ExecuteClassifyDDGIVolumeProbes(nvrhi::ICommandList *commandList);
    void ExecuteCalculateDDGIVolumeVariability(nvrhi::ICommandList *commandList);
    void ExecuteReadbackDDGIVolumeVariability(nvrhi::ICommandList *commandList);
    void ExecuteGatherIndirectLighting(nvrhi::ICommandList *commandList);

    GlobalResources *m_GlobalResources;
    std::vector<uint32_t> m_SelectedVolumeIndices;
    nvrhi::rt::ShaderTableHandle m_ProbeTraceShaderTable;
    nvrhi::ComputePipelineHandle m_IndirectCSPipeline;
    nvrhi::ComputePipelineHandle m_ProbeBlendCSPipelines[2];
    nvrhi::ComputePipelineHandle m_ProbeRelocationCSPipeline;
    nvrhi::ComputePipelineHandle m_ProbeRelocationResetCSPipeline;
    nvrhi::ComputePipelineHandle m_ProbeClassificationCSPipeline;
    nvrhi::ComputePipelineHandle m_ProbeClassificationResetCSPipeline;
    nvrhi::ComputePipelineHandle m_ReductionCSPipeline;
    nvrhi::ComputePipelineHandle m_ExtraReductionCSPipeline;
};

#endif /* DDGIRENDERPASS_H */
