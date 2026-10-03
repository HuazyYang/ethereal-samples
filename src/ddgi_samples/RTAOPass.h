#ifndef RTAORENDERPASS_H
#define RTAORENDERPASS_H
#include <nvrhi/nvrhi.h>

namespace donut::engine {
class ShaderFactory;
}

class GlobalResources;
struct Config;

class RTAOPass {
public:
   bool Initialize(GlobalResources *resources, donut::engine::ShaderFactory *shaderFactory);

   void Update(const Config *config);

   void Execute(nvrhi::ICommandList *commandList);

private:
   bool CreatePipelines(donut::engine::ShaderFactory *shaderFactory);

   GlobalResources *m_GlobalResources;
   bool m_Enabled;
   nvrhi::rt::ShaderTableHandle m_ShaderTable;
   nvrhi::ComputePipelineHandle m_AOFilterPipeline;
};

#endif /* RTAORENDERPASS_H */
