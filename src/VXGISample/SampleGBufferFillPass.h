#ifndef SRC_VXGISAMPLE_SAMPLEGBUFFERFILLPASS_H
#define SRC_VXGISAMPLE_SAMPLEGBUFFERFILLPASS_H

#include <donut/render/GBufferFillPass.h>

class SampleGBufferFillPass : public donut::render::GBufferFillPass {
    NVRHI_INHERIT_INTERFACE_TABLE()
 public:
    using GBufferFillPass::GBufferFillPass;

 protected:
    nvrhi::ShaderHandle CreatePixelShader(donut::engine::ShaderFactory& shaderFactory, const CreateParameters& params,
                                          bool alphaTested);
    nvrhi::AutoPtr<donut::engine::MaterialBindingCache> CreateMaterialBindingCache(
        donut::engine::CommonRenderPasses& commonPasses) override;
};

#endif /* SRC_VXGISAMPLE_SAMPLEGBUFFERFILLPASS_H */
