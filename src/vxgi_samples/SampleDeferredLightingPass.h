#ifndef SRC_VXGISAMPLE_SAMPLEDEFERREDLIGHTINGPASS_H
#define SRC_VXGISAMPLE_SAMPLEDEFERREDLIGHTINGPASS_H
#include <donut/render/DeferredLightingPass.h>

class SampleDeferredLightingPass : public donut::render::DeferredLightingPass {
   NVRHI_INHERIT_INTERFACE_TABLE()
public:
   using donut::render::DeferredLightingPass::DeferredLightingPass;

protected:
   nvrhi::ShaderHandle CreateComputeShader(donut::engine::ShaderFactory& shaderFactory) override;
};

#endif /* SRC_VXGISAMPLE_SAMPLEDEFERREDLIGHTINGPASS_H */
