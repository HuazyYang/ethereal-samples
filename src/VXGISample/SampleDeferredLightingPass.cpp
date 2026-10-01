#include "SampleDeferredLightingPass.h"
#include <donut/engine/ShaderFactory.h>

nvrhi::ShaderHandle SampleDeferredLightingPass::CreateComputeShader(
    donut::engine::ShaderFactory& shaderFactory) {
    return shaderFactory.CreateShader("app/UserDefined/SampleDeferredLightingCS.hlsl", "main", nullptr,
                                      nvrhi::ShaderType::Compute);
}
