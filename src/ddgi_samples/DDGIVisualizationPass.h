#ifndef DDGIVISUALIZATIONPASS_H
#define DDGIVISUALIZATIONPASS_H
#include <nvrhi/nvrhi.h>

namespace donut::engine {
class ShaderFactory;
}

struct Config;
struct GlobalResources;

class DDGIVisualizationPass {
public:
   bool Initialize(GlobalResources *resources, donut::engine::ShaderFactory *shaderFactory);

   void Update(const Config *config);

   void Execute(nvrhi::ICommandList *commandList);

private:
   bool CreatePipelines(donut::engine::ShaderFactory *shaderFactory);

   GlobalResources *m_GlobalResources;
   nvrhi::rt::ShaderTableHandle m_ShaderTable;
   nvrhi::rt::ShaderTableHandle m_HideInactiveShaderTable;
   nvrhi::ComputePipelineHandle m_VisTexturesPipeline;
};

#endif /* DDGIVISUALIZATIONPASS_H */
