#include <nvrhi/nvrhi.h>

namespace donut::engine {
class ShaderFactory;
}

class SampleScene;
class GlobalResources;
struct Config;

class PathTracingPass {
 public:
    PathTracingPass();

    bool Initialize(GlobalResources *resources, donut::engine::ShaderFactory *shaderFactory);

    void Update(const Config *config);

    void Execute(nvrhi::ICommandList *cmdList);

 private:
    bool CreateRTPipeline(donut::engine::ShaderFactory *shaderFactory);

    GlobalResources *m_GlobalResources = nullptr;
    nvrhi::rt::ShaderTableHandle m_ShaderTable;
};