#ifndef COMPOSITEPASS_H
#define COMPOSITEPASS_H
#include <nvrhi/nvrhi.h>

namespace donut::engine {
class ShaderFactory;
}

class GlobalResources;
struct Config;

class CompositePass {
public:
   bool Initialize(GlobalResources *resource, donut::engine::ShaderFactory *shaderFactory);

   void Update(const Config *config);

   void Execute(nvrhi::ICommandList *commandList);

private:
   bool CreatePipeline(donut::engine::ShaderFactory *shaderFactory);

   GlobalResources *m_GlobalResources;
   nvrhi::GraphicsPipelineHandle m_CompositePipeline;
};

#endif /* COMPOSITEPASS_H */
