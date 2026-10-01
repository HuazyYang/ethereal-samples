#include "SampleGBufferFillPass.h"
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/MaterialBindingCache.h>
#include <donut/engine/CommonRenderPasses.h>

using namespace dm;
#include <donut/shaders/gbuffer_cb.h>

nvrhi::ShaderHandle SampleGBufferFillPass::CreatePixelShader(donut::engine::ShaderFactory& shaderFactory,
                                                             const CreateParameters& params, bool alphaTested) {
    return shaderFactory.CreateShader("app/UserDefined/SampleGBufferFillPS.hlsl", "main", nullptr,
                                      nvrhi::ShaderType::Pixel);
}

donut::AutoPtr<donut::engine::MaterialBindingCache> SampleGBufferFillPass::CreateMaterialBindingCache(
    donut::engine::CommonRenderPasses& commonPasses) {
    using namespace donut::engine;
    std::vector<MaterialResourceBinding> materialBindings = {
        {MaterialResource::ConstantBuffer,      GBUFFER_BINDING_MATERIAL_CONSTANTS           },
        {MaterialResource::DiffuseTexture,      GBUFFER_BINDING_MATERIAL_DIFFUSE_TEXTURE     },
        {MaterialResource::SpecularTexture,     GBUFFER_BINDING_MATERIAL_SPECULAR_TEXTURE    },
        {MaterialResource::NormalTexture,       GBUFFER_BINDING_MATERIAL_NORMAL_TEXTURE      },
        {MaterialResource::EmissiveTexture,     GBUFFER_BINDING_MATERIAL_EMISSIVE_TEXTURE    },
        {MaterialResource::OcclusionTexture,    GBUFFER_BINDING_MATERIAL_OCCLUSION_TEXTURE   },
        {MaterialResource::TransmissionTexture, GBUFFER_BINDING_MATERIAL_TRANSMISSION_TEXTURE},
        {MaterialResource::OpacityTexture,      GBUFFER_BINDING_MATERIAL_OPACITY_TEXTURE     }
    };

    return MAKE_RC_OBJ_PTR(MaterialBindingCache, m_Device, nvrhi::ShaderType::Pixel,
                           /* registerSpace = */ (uint32_t)GBUFFER_SPACE_MATERIAL,
                           /* registerSpaceIsDescriptorSet = */ true, materialBindings,
                           commonPasses.m_AnisotropicWrapSampler, commonPasses.m_BlackTexture);
}
