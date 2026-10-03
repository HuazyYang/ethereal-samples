#ifndef GBUFFERPASS_H
#define GBUFFERPASS_H
#include <nvrhi/nvrhi.h>

namespace donut::engine {
class ShaderFactory;
}

class GlobalResources;
struct Config;

class GBufferPass {
 public:
    bool Initialize(GlobalResources *resources, donut::engine::ShaderFactory *shaderFactory);
    void Update(const Config *config);
    void Execute(nvrhi::ICommandList *commandList);

 private:
    GlobalResources *m_GlobalResources;
    nvrhi::rt::PipelineHandle m_GBufferPipeline;
    nvrhi::rt::ShaderTableHandle m_GBufferShaderTable;
};

#endif /* GBUFFERPASS_H */
