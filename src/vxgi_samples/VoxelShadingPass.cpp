#include "VoxelShadingPass.h"
#include "VoxelRenderer.h"
#include "ViewTracer.h"
#include <donut/render/DrawStrategy.h>
#include <donut/engine/FramebufferFactory.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/ShadowMap.h>
#include <donut/engine/SceneTypes.h>
#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/MaterialBindingCache.h>
#include <donut/engine/SceneGraph.h>
#include <donut/core/log.h>
#include <nvrhi/utils.h>
#include <utility>

using namespace donut::math;
#include <donut/shaders/forward_cb.h>

using namespace donut::engine;
using namespace donut::render;

#define VOXELIZE_TRANSPARENT 1

namespace vxgi {

VoxelShadingPass::VoxelShadingPass(nvrhi::IDevice* device,
                                 donut::engine::ShaderFactory* shaderFactory,
                                 CommonRenderPasses* commonPasses)
    : m_Device(device), m_ShaderFactory(shaderFactory), m_CommonPasses(commonPasses) {
    m_IsDX11 = m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D11;
}

VoxelShadingPass::~VoxelShadingPass() {}

void VoxelShadingPass::Init(const CreateParameters& params) {
    m_UseInputAssembler = params.useInputAssembler;

    m_VoxelizeVS = CreateVertexShader(params);
    m_InputLayout = CreateInputLayout(m_VoxelizeVS, params);

    if (params.materialBindings)
        m_MaterialBindings = params.materialBindings;
    else
        m_MaterialBindings = CreateMaterialBindingCache(*m_CommonPasses);
    {
        // auto samplerDesc =
        //     nvrhi::SamplerDesc().setAllAddressModes(nvrhi::SamplerAddressMode::Border).setBorderColor(1.0f);
        // m_ShadowSampler = m_Device->createSampler(samplerDesc);
        auto samplerDesc = nvrhi::SamplerDesc()
                               .setAllAddressModes(nvrhi::SamplerAddressMode::Border)
                               .setBorderColor(1.f)
                               .setReductionType(nvrhi::SamplerReductionType::Comparison);
        m_Device->createSampler(samplerDesc, &m_ShadowSampler);
    }

    m_Device->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(
        sizeof(ForwardShadingViewConstants), "ForwardShadingViewConstants",
        params.numConstantBufferVersions), &m_ForwardViewCB);
    m_Device->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(
            sizeof(ForwardShadingLightConstants), "ForwardShadingLightConstants",
            params.numConstantBufferVersions), &m_ForwardLightCB);

    m_ViewBindingLayout = CreateViewBindingLayout();
    m_ViewBindingSet = CreateViewBindingSet();
    m_ShadingBindingLayout = CreateShadingBindingLayout();
    m_InputBindingLayout = CreateInputBindingLayout();

    m_VoxelRenderer = MAKE_RC_OBJ_PTR(VoxelRenderer, m_Device, m_ShaderFactory);
    Status rc;
    if (VXGI_FAILED(rc = m_VoxelRenderer->createNewTracer(&m_GIViewTracer, false)))
        NVRHI_ASSERT(0);
    if(VXGI_FAILED(rc = m_VoxelRenderer->createNewTracer(&m_AOViewTracer, true)))
        NVRHI_ASSERT(0);
}

void VoxelShadingPass::ResetBindingCache() {
    m_MaterialBindings->Clear();
    m_ShadingBindingSets.clear();
    m_InputBindingSets.clear();
}

void VoxelShadingPass::SetVoxelizationParameters(nvrhi::ICommandList* commandList,
                                                 const vxgi::VoxelizationParameters& params) {
    Status rc;
    bool invalidated;
    if(VXGI_FAILED(rc = m_VoxelRenderer->setVoxelizationParameters(commandList, params, &invalidated))) {
        NVRHI_ASSERT(0);
        return;
    }

    if(invalidated) {
        m_Pipelines.clear();
    }
}

void VoxelShadingPass::PrepareForOpacityVoxelization(nvrhi::ICommandList* commandList,
                                                     UpdateVoxelizationParameters& params,
                                                     bool* performOpacityVoxelization,
                                                     bool* performEmittanceVoxelization) {
    m_VoxelRenderer->prepareForOpacityVoxelization(commandList, &params, performOpacityVoxelization,
                                                   performEmittanceVoxelization);
}

void VoxelShadingPass::GetVoxelizationViewMatrix(dm::float4x4& viewMatrix) {
    m_VoxelRenderer->getVoxelizationViewMatrix(&viewMatrix);
}

bool VoxelShadingPass::GetInvalidatedRegions(uint numMaxRegions, uint* numRegions, box3* regions) {
    Status rc;
    if (VXGI_FAILED(rc = m_VoxelRenderer->getInvalidatedRegion(regions, numMaxRegions, numRegions))) {
        NVRHI_ASSERT(0);
        return false;
    }
    return true;
}

void VoxelShadingPass::PrepareForEmittanceVoxelization() { m_VoxelRenderer->prepareForEmittanceVoxelization(); }

void VoxelShadingPass::FinalizeVoxelization(nvrhi::ICommandList* commandList) {
    m_VoxelRenderer->finalizeVoxelization(commandList);
}

void VoxelShadingPass::RenderDebug(nvrhi::ICommandList* commandList, const DebugRenderParameters& params) {
    Status rc;
    if (VXGI_FAILED(rc = m_VoxelRenderer->renderDebug(commandList, &params))) {
        NVRHI_ASSERT(0);
        return;
    }
}

void VoxelShadingPass::ComputeDiffuseChannel(nvrhi::ICommandList* commandList,
                                             const DiffuseTracingParameters& params,
                                             nvrhi::ITexture** indirectDiffuse,
                                             const ViewTracerInputBuffers* inputBuffers,
                                             const ViewTracerInputBuffers* inputBuffersPreviousFrame) {
    Status rc;
    if (VXGI_FAILED(rc = m_GIViewTracer->computeDiffuseChannel(commandList, params, indirectDiffuse,
                                                               inputBuffers, inputBuffersPreviousFrame))) {
        NVRHI_ASSERT(0);
        return;
    }
    return;
}

void VoxelShadingPass::ComputeAmbientChannel(nvrhi::ICommandList* commandList,
                                             const DiffuseTracingParameters& params,
                                             nvrhi::ITexture** ambientTexture,
                                             const ViewTracerInputBuffers* inputBuffers,
                                             const ViewTracerInputBuffers* inputBuffersPreviousFrame) {
    Status rc;
    if (VXGI_FAILED(rc = m_AOViewTracer->computeDiffuseChannel(commandList, params, ambientTexture,
                                                               inputBuffers, inputBuffersPreviousFrame))) {
        NVRHI_ASSERT(0);
        return;
    }
    return;
}

void VoxelShadingPass::ComputeSpecularChannel(nvrhi::ICommandList* commandList,
                                              const SpecularTracingParameters& params,
                                              nvrhi::ITexture** indirectSpecular,
                                              const ViewTracerInputBuffers* inputBuffers,
                                              const ViewTracerInputBuffers* inputBuffersPreviousFrame) {
    Status rc;
    if (VXGI_FAILED(rc = m_GIViewTracer->computeSpecularChannel(commandList, params, indirectSpecular,
                                                                inputBuffers, inputBuffersPreviousFrame))) {
        NVRHI_ASSERT(0);
        return;
    }
    return;
}

nvrhi::ShaderHandle VoxelShadingPass::CreateVertexShader(const CreateParameters& params) {
    char const* sourceFileName = "app/UserDefined/VoxelizeVS.hlsl";

    if (params.useInputAssembler) {
        return m_ShaderFactory->CreateShader(
            sourceFileName, "input_assembler", nullptr,
            nvrhi::ShaderType::Vertex);
    } else {
        return m_ShaderFactory->CreateShader(
            sourceFileName, "buffer_loads", nullptr,
            nvrhi::ShaderType::Vertex);
    }
}

nvrhi::InputLayoutHandle VoxelShadingPass::CreateInputLayout(
    nvrhi::IShader* vertexShader, const CreateParameters& params) {
    if (params.useInputAssembler) {
        const nvrhi::VertexAttributeDesc inputDescs[] = {
            GetVertexAttributeDesc(VertexAttribute::Position, "POS", 0),
            // GetVertexAttributeDesc(VertexAttribute::PrevPosition, "PREV_POS", 1),
            GetVertexAttributeDesc(VertexAttribute::TexCoord1, "TEXCOORD", 1),
            GetVertexAttributeDesc(VertexAttribute::Normal, "NORMAL", 2),
            GetVertexAttributeDesc(VertexAttribute::Tangent, "TANGENT", 3),
            GetVertexAttributeDesc(VertexAttribute::Transform, "TRANSFORM", 4),
        };

        nvrhi::InputLayoutHandle inputLayout;
        m_Device->createInputLayout(inputDescs, uint32_t(std::size(inputDescs)),
                                           vertexShader, &inputLayout);
        return inputLayout;
    }

    return nullptr;
}

nvrhi::BindingLayoutHandle VoxelShadingPass::CreateViewBindingLayout() {
    auto bindingLayoutDesc =
        nvrhi::BindingLayoutDesc()
            .setVisibility(nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel)
            .setRegisterSpaceAndDescriptorSet(FORWARD_SPACE_VIEW)
            .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(
                FORWARD_BINDING_VIEW_CONSTANTS));

    nvrhi::BindingLayoutHandle bindingLayout;
    m_Device->createBindingLayout(bindingLayoutDesc, &bindingLayout);
    return bindingLayout;
}

nvrhi::BindingSetHandle VoxelShadingPass::CreateViewBindingSet() {
    auto bindingSetDesc = nvrhi::BindingSetDesc()
                              .setTrackLiveness(m_TrackLiveness)
                              .addItem(nvrhi::BindingSetItem::ConstantBuffer(
                                  FORWARD_BINDING_VIEW_CONSTANTS, m_ForwardViewCB));

    nvrhi::BindingSetHandle bindingSet;
    m_Device->createBindingSet(bindingSetDesc, m_ViewBindingLayout, &bindingSet);
    return bindingSet;
}

nvrhi::BindingLayoutHandle VoxelShadingPass::CreateShadingBindingLayout() {
    auto bindingLayoutDesc =
        nvrhi::BindingLayoutDesc()
            .setVisibility(nvrhi::ShaderType::Pixel)
            .setRegisterSpaceAndDescriptorSet(FORWARD_SPACE_SHADING)
            .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(
                FORWARD_BINDING_LIGHT_CONSTANTS))
            .addItem(
                nvrhi::BindingLayoutItem::Texture_SRV(FORWARD_BINDING_SHADOW_MAP_TEXTURE))
            .addItem(nvrhi::BindingLayoutItem::Texture_SRV(
                FORWARD_BINDING_DIFFUSE_LIGHT_PROBE_TEXTURE))
            .addItem(nvrhi::BindingLayoutItem::Texture_SRV(
                FORWARD_BINDING_SPECULAR_LIGHT_PROBE_TEXTURE))
            .addItem(nvrhi::BindingLayoutItem::Texture_SRV(
                FORWARD_BINDING_ENVIRONMENT_BRDF_TEXTURE))
            .addItem(nvrhi::BindingLayoutItem::Sampler(FORWARD_BINDING_MATERIAL_SAMPLER))
            .addItem(nvrhi::BindingLayoutItem::Sampler(FORWARD_BINDING_SHADOW_MAP_SAMPLER))
            .addItem(nvrhi::BindingLayoutItem::Sampler(FORWARD_BINDING_LIGHT_PROBE_SAMPLER))
            .addItem(nvrhi::BindingLayoutItem::Sampler(
                FORWARD_BINDING_ENVIRONMENT_BRDF_SAMPLER));

    nvrhi::BindingLayoutHandle bindingLayout;
    m_Device->createBindingLayout(bindingLayoutDesc, &bindingLayout);
    return bindingLayout;
}

nvrhi::BindingSetHandle VoxelShadingPass::CreateShadingBindingSet(
    nvrhi::ITexture* shadowMapTexture, nvrhi::ITexture* diffuse, nvrhi::ITexture* specular,
    nvrhi::ITexture* environmentBrdf) {
    auto bindingSetDesc =
        nvrhi::BindingSetDesc()
            .setTrackLiveness(m_TrackLiveness)
            .addItem(nvrhi::BindingSetItem::ConstantBuffer(FORWARD_BINDING_LIGHT_CONSTANTS,
                                                           m_ForwardLightCB))
            .addItem(nvrhi::BindingSetItem::Texture_SRV(
                FORWARD_BINDING_SHADOW_MAP_TEXTURE,
                shadowMapTexture ? shadowMapTexture
                                 : m_CommonPasses->m_BlackTexture2DArray.Get()))
            .addItem(nvrhi::BindingSetItem::Texture_SRV(
                FORWARD_BINDING_DIFFUSE_LIGHT_PROBE_TEXTURE,
                diffuse ? diffuse : m_CommonPasses->m_BlackCubeMapArray.Get()))
            .addItem(nvrhi::BindingSetItem::Texture_SRV(
                FORWARD_BINDING_SPECULAR_LIGHT_PROBE_TEXTURE,
                specular ? specular : m_CommonPasses->m_BlackCubeMapArray.Get()))
            .addItem(nvrhi::BindingSetItem::Texture_SRV(
                FORWARD_BINDING_ENVIRONMENT_BRDF_TEXTURE,
                environmentBrdf ? environmentBrdf : m_CommonPasses->m_BlackTexture.Get()))
            .addItem(nvrhi::BindingSetItem::Sampler(
                FORWARD_BINDING_MATERIAL_SAMPLER, m_CommonPasses->m_AnisotropicWrapSampler))
            .addItem(nvrhi::BindingSetItem::Sampler(FORWARD_BINDING_SHADOW_MAP_SAMPLER,
                                                    m_ShadowSampler))
            .addItem(nvrhi::BindingSetItem::Sampler(FORWARD_BINDING_LIGHT_PROBE_SAMPLER,
                                                    m_CommonPasses->m_LinearWrapSampler))
            .addItem(
                nvrhi::BindingSetItem::Sampler(FORWARD_BINDING_ENVIRONMENT_BRDF_SAMPLER,
                                               m_CommonPasses->m_LinearClampSampler));

    nvrhi::BindingSetHandle bindingSet;
    m_Device->createBindingSet(bindingSetDesc, m_ShadingBindingLayout, &bindingSet);
    return bindingSet;
}

nvrhi::GraphicsPipelineHandle VoxelShadingPass::CreateGraphicsPipeline(
    VXGIShadingPassPipelineKey const& key,
    nvrhi::FramebufferInfo const& framebufferInfo) {
    nvrhi::ShaderHandle voxelizeGS, voxelizePS;
    if(!key.useForEmittance) {
        m_VoxelRenderer->GetOpacityShaders(&voxelizeGS, &voxelizePS);
    } else {
        m_VoxelRenderer->GetEmittanceShaders(key.useCoverageSuperSampling, &voxelizeGS,
                                        &voxelizePS);
    }

    nvrhi::BindingLayoutHandle VXGIBindingLayout;
    m_VoxelRenderer->GetBindingLayout(&VXGIBindingLayout);

    nvrhi::GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.inputLayout = m_InputLayout;
    pipelineDesc.VS = m_VoxelizeVS;
    pipelineDesc.GS = voxelizeGS;
    pipelineDesc.PS = voxelizePS;
    pipelineDesc.renderState.rasterState.frontCounterClockwise = key.frontCounterClockwise;
    pipelineDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
    pipelineDesc.renderState.rasterState.scissorEnable = true;
    pipelineDesc.renderState.blendState.alphaToCoverageEnable = false;
    pipelineDesc.shadingRateState = key.shadingRateState;
    pipelineDesc.bindingLayouts = {m_MaterialBindings->GetLayout(), m_ViewBindingLayout,
                                   m_ShadingBindingLayout};
    if (!m_UseInputAssembler)
        pipelineDesc.bindingLayouts.push_back(m_InputBindingLayout);
    pipelineDesc.bindingLayouts.push_back(VXGIBindingLayout);

    bool const framebufferUsesMSAA = framebufferInfo.sampleCount > 1;

    pipelineDesc.renderState.depthStencilState.depthTestEnable = false;
    pipelineDesc.renderState.depthStencilState.stencilEnable = false;
    pipelineDesc.renderState.rasterState.multisampleEnable = true;

    if(m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12) {
        pipelineDesc.renderState.rasterState.forcedSampleCount = 8;
    }

    if(key.useForEmittance && key.useCoverageSuperSampling) {
        pipelineDesc.renderState.rasterState.conservativeRasterEnable = true;
    }

    // switch (key.domain) {
    //     case MaterialDomain::Opaque:
    //         pipelineDesc.PS = m_PixelShader;
    //         break;

    //     case MaterialDomain::AlphaTested:
    //         pipelineDesc.PS = m_PixelShader;
    //         pipelineDesc.renderState.blendState.alphaToCoverageEnable = framebufferUsesMSAA;
    //         break;

    //     case MaterialDomain::AlphaBlended: {
    //         pipelineDesc.PS = m_PixelShader;
    //         pipelineDesc.renderState.blendState.targets[0]
    //             .enableBlend()
    //             .setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
    //             .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
    //             .setSrcBlendAlpha(nvrhi::BlendFactor::Zero)
    //             .setDestBlendAlpha(nvrhi::BlendFactor::One);

    //         pipelineDesc.renderState.depthStencilState.disableDepthWrite();
    //         break;
    //     }

    //     case MaterialDomain::Transmissive:
    //     case MaterialDomain::TransmissiveAlphaTested:
    //     case MaterialDomain::TransmissiveAlphaBlended: {
    //         pipelineDesc.PS = m_PixelShaderTransmissive;
    //         pipelineDesc.renderState.blendState.targets[0]
    //             .enableBlend()
    //             .setSrcBlend(nvrhi::BlendFactor::One)
    //             .setDestBlend(nvrhi::BlendFactor::Src1Color)
    //             .setSrcBlendAlpha(nvrhi::BlendFactor::Zero)
    //             .setDestBlendAlpha(nvrhi::BlendFactor::One);

    //         pipelineDesc.renderState.depthStencilState.disableDepthWrite();
    //         break;
    //     }
    //     default:
    //         return nullptr;
    // }

    nvrhi::GraphicsPipelineHandle pipeline;
    m_Device->createGraphicsPipeline1(pipelineDesc, framebufferInfo, &pipeline);
    return pipeline;
}

nvrhi::AutoPtr<MaterialBindingCache> VoxelShadingPass::CreateMaterialBindingCache(
    CommonRenderPasses& commonPasses) {
    std::vector<MaterialResourceBinding> materialBindings = {
        {MaterialResource::ConstantBuffer,      FORWARD_BINDING_MATERIAL_CONSTANTS        },
        {MaterialResource::DiffuseTexture,      FORWARD_BINDING_MATERIAL_DIFFUSE_TEXTURE  },
        {MaterialResource::SpecularTexture,     FORWARD_BINDING_MATERIAL_SPECULAR_TEXTURE },
        {MaterialResource::NormalTexture,       FORWARD_BINDING_MATERIAL_NORMAL_TEXTURE   },
        {MaterialResource::EmissiveTexture,     FORWARD_BINDING_MATERIAL_EMISSIVE_TEXTURE },
        {MaterialResource::OcclusionTexture,    FORWARD_BINDING_MATERIAL_OCCLUSION_TEXTURE},
        {MaterialResource::TransmissionTexture,
         FORWARD_BINDING_MATERIAL_TRANSMISSION_TEXTURE                                    },
        {MaterialResource::OpacityTexture,      FORWARD_BINDING_MATERIAL_OPACITY_TEXTURE  }
    };

    return MAKE_RC_OBJ_PTR(MaterialBindingCache, m_Device, nvrhi::ShaderType::Pixel,
                           /* registerSpace = */ (uint32_t)FORWARD_SPACE_MATERIAL,
                           /* registerSpaceIsDescriptorSet = */ true, materialBindings,
                           commonPasses.m_AnisotropicWrapSampler,
                           commonPasses.m_GrayTexture);
}

void VoxelShadingPass::SetupView(GeometryPassContext& abstractContext,
                                   nvrhi::ICommandList* commandList, const IView* view,
                                   const IView* viewPrev) {
    auto& context = static_cast<Context&>(abstractContext);

    ForwardShadingViewConstants viewConstants = {};
    view->FillPlanarViewConstants(viewConstants.view);
    commandList->writeBuffer(m_ForwardViewCB, &viewConstants, sizeof(viewConstants));

    context.commandList = commandList;
    context.keyTemplate.frontCounterClockwise = view->IsMirrored();
    context.keyTemplate.reverseDepth = view->IsReverseDepth();
    context.keyTemplate.shadingRateState = view->GetVariableRateShadingState();
}

void VoxelShadingPass::PrepareLights(
    Context& context, nvrhi::ICommandList* commandList,
    const std::vector<nvrhi::AutoPtr<Light>>& lights, dm::float3 ambientColorTop,
    dm::float3 ambientColorBottom,
    const std::vector<nvrhi::AutoPtr<LightProbe>>& lightProbes) {
    nvrhi::ITexture* shadowMapTexture = nullptr;
    int2 shadowMapTextureSize = 0;
    for (const auto& light : lights) {
        if (light->shadowMap) {
            shadowMapTexture = light->shadowMap->GetTexture();
            shadowMapTextureSize = light->shadowMap->GetTextureSize();
            break;
        }
    }

    nvrhi::ITexture* lightProbeDiffuse = nullptr;
    nvrhi::ITexture* lightProbeSpecular = nullptr;
    nvrhi::ITexture* lightProbeEnvironmentBrdf = nullptr;

    for (const auto& probe : lightProbes) {
        if (!probe->enabled)
            continue;

        if (lightProbeDiffuse == nullptr || lightProbeSpecular == nullptr ||
            lightProbeEnvironmentBrdf == nullptr) {
            lightProbeDiffuse = probe->diffuseMap;
            lightProbeSpecular = probe->specularMap;
            lightProbeEnvironmentBrdf = probe->environmentBrdf;
        } else {
            if (lightProbeDiffuse != probe->diffuseMap ||
                lightProbeSpecular != probe->specularMap ||
                lightProbeEnvironmentBrdf != probe->environmentBrdf) {
                donut::log::error(
                    "All lights probe submitted to VXGIShadingPass::PrepareLights(...) "
                    "must use the same set of textures");
                return;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lockGuard(m_Mutex);

        nvrhi::BindingSetHandle& shadingBindings =
            m_ShadingBindingSets[std::make_pair(shadowMapTexture, lightProbeDiffuse)];

        if (!shadingBindings) {
            shadingBindings =
                CreateShadingBindingSet(shadowMapTexture, lightProbeDiffuse,
                                        lightProbeSpecular, lightProbeEnvironmentBrdf);
        }

        context.shadingBindingSet = shadingBindings;
    }

    ForwardShadingLightConstants constants = {};

    constants.shadowMapTextureSize = float2(shadowMapTextureSize);
    constants.shadowMapTextureSizeInv = 1.f / constants.shadowMapTextureSize;

    int numShadows = 0;

    for (int nLight = 0;
         nLight < std::min(static_cast<int>(lights.size()), FORWARD_MAX_LIGHTS); nLight++) {
        const auto& light = lights[nLight];

        LightConstants& lightConstants = constants.lights[constants.numLights];
        light->FillLightConstants(lightConstants);

        if (light->shadowMap) {
            for (uint32_t cascade = 0; cascade < light->shadowMap->GetNumberOfCascades();
                 cascade++) {
                if (numShadows < FORWARD_MAX_SHADOWS) {
                    light->shadowMap->GetCascade(cascade)->FillShadowConstants(
                        constants.shadows[numShadows]);
                    lightConstants.shadowCascades[cascade] = numShadows;
                    ++numShadows;
                }
            }

            for (uint32_t perObjectShadow = 0;
                 perObjectShadow < light->shadowMap->GetNumberOfPerObjectShadows();
                 perObjectShadow++) {
                if (numShadows < FORWARD_MAX_SHADOWS) {
                    light->shadowMap->GetPerObjectShadow(perObjectShadow)
                        ->FillShadowConstants(constants.shadows[numShadows]);
                    lightConstants.perObjectShadows[perObjectShadow] = numShadows;
                    ++numShadows;
                }
            }
        }

        ++constants.numLights;
    }

    constants.ambientColorTop = float4(ambientColorTop, 0.f);
    constants.ambientColorBottom = float4(ambientColorBottom, 0.f);

    for (const auto& probe : lightProbes) {
        if (!probe->IsActive())
            continue;

        LightProbeConstants& lightProbeConstants =
            constants.lightProbes[constants.numLightProbes];
        probe->FillLightProbeConstants(lightProbeConstants);

        ++constants.numLightProbes;

        if (constants.numLightProbes >= FORWARD_MAX_LIGHT_PROBES)
            break;
    }

    commandList->writeBuffer(m_ForwardLightCB, &constants, sizeof(constants));
}

ViewType::Enum VoxelShadingPass::GetSupportedViewTypes() const {
    return ViewType::PLANAR;
}

bool VoxelShadingPass::SetupMaterial(GeometryPassContext& abstractContext,
                                       const Material* material,
                                       nvrhi::RasterCullMode cullMode,
                                       nvrhi::GraphicsState& state) {
    auto& context = static_cast<Context&>(abstractContext);

    state.framebuffer = m_VoxelRenderer->GetProjectedCoverageFramebuffer();

    nvrhi::IBindingSet* materialBindingSet =
        m_MaterialBindings->GetMaterialBindingSet(material);

    if (!materialBindingSet)
        return false;

    if (material->domain >= MaterialDomain::Count ||
        cullMode > nvrhi::RasterCullMode::None) {
        assert(false);
        return false;
    }

    VXGIShadingPassPipelineKey key = context.keyTemplate;
    key.cullMode = cullMode;
    key.domain = material->domain;
    key.useForEmittance = context.isEmittance;

    nvrhi::GraphicsPipelineHandle& pipeline = m_Pipelines[key];

    if (!pipeline) {
        std::lock_guard<std::mutex> lockGuard(m_Mutex);

        if (!pipeline)
            pipeline = CreateGraphicsPipeline(key, state.framebuffer->getFramebufferInfo().getInfo());

        if (!pipeline)
            return false;
    }

    assert(pipeline->getFramebufferInfo() == state.framebuffer->getFramebufferInfo().getInfo());

    state.pipeline = pipeline;
    state.bindings = {materialBindingSet, m_ViewBindingSet, context.shadingBindingSet};

    if (!m_UseInputAssembler)
        state.bindings.push_back(context.inputBindingSet);

    Status rc;
    MaterialInfo vxgiMaterialInfo;
    vxgiMaterialInfo.voxelizationThickness = 1.f;
    vxgiMaterialInfo.twoSided = false;
    if(VXGI_FAILED(m_VoxelRenderer->getVoxelizationState(context.commandList, context.isEmittance, vxgiMaterialInfo, &state)))
        return false;

    return true;
}

void VoxelShadingPass::SetupInputBuffers(GeometryPassContext& abstractContext,
                                           const BufferGroup* buffers,
                                           nvrhi::GraphicsState& state) {
    auto& context = static_cast<Context&>(abstractContext);

    state.indexBuffer = {buffers->indexBuffer, nvrhi::Format::R32_UINT, 0};

    if (m_UseInputAssembler) {
        state.vertexBuffers = {
            {buffers->vertexBuffer,   0,
             buffers->getVertexBufferRange(VertexAttribute::Position).byteOffset    },
            // {buffers->vertexBuffer,   1,
            //  buffers->getVertexBufferRange(VertexAttribute::PrevPosition).byteOffset},
            {buffers->vertexBuffer,   1,
             buffers->getVertexBufferRange(VertexAttribute::TexCoord1).byteOffset   },
            {buffers->vertexBuffer,   2,
             buffers->getVertexBufferRange(VertexAttribute::Normal).byteOffset      },
            {buffers->vertexBuffer,   3,
             buffers->getVertexBufferRange(VertexAttribute::Tangent).byteOffset     },
            {buffers->instanceBuffer, 4, 0                                          }
        };
    } else {
        context.inputBindingSet = GetOrCreateInputBindingSet(buffers);
        context.positionOffset =
            uint32_t(buffers->getVertexBufferRange(VertexAttribute::Position).byteOffset);
        context.texCoordOffset =
            uint32_t(buffers->getVertexBufferRange(VertexAttribute::TexCoord1).byteOffset);
        context.normalOffset =
            uint32_t(buffers->getVertexBufferRange(VertexAttribute::Normal).byteOffset);
        context.tangentOffset =
            uint32_t(buffers->getVertexBufferRange(VertexAttribute::Tangent).byteOffset);
    }
}

nvrhi::BindingLayoutHandle VoxelShadingPass::CreateInputBindingLayout() {
    if (m_UseInputAssembler)
        return nullptr;

    auto bindingLayoutDesc =
        nvrhi::BindingLayoutDesc()
            .setVisibility(nvrhi::ShaderType::Vertex)
            .setRegisterSpaceAndDescriptorSet(FORWARD_SPACE_INPUT)
            .addItem(m_IsDX11 ? nvrhi::BindingLayoutItem::RawBuffer_SRV(
                                    FORWARD_BINDING_INSTANCE_BUFFER)
                              : nvrhi::BindingLayoutItem::StructuredBuffer_SRV(
                                    FORWARD_BINDING_INSTANCE_BUFFER))
            .addItem(nvrhi::BindingLayoutItem::RawBuffer_SRV(FORWARD_BINDING_VERTEX_BUFFER))
            .addItem(nvrhi::BindingLayoutItem::PushConstants(FORWARD_BINDING_PUSH_CONSTANTS,
                                                             sizeof(ForwardPushConstants)));

    nvrhi::BindingLayoutHandle bindingLayout;
    m_Device->createBindingLayout(bindingLayoutDesc, &bindingLayout);
    return bindingLayout;
}

nvrhi::BindingSetHandle VoxelShadingPass::CreateInputBindingSet(
    const BufferGroup* bufferGroup) {
    auto bindingSetDesc =
        nvrhi::BindingSetDesc()
            .addItem(
                m_IsDX11
                    ? nvrhi::BindingSetItem::RawBuffer_SRV(FORWARD_BINDING_INSTANCE_BUFFER,
                                                           bufferGroup->instanceBuffer)
                    : nvrhi::BindingSetItem::StructuredBuffer_SRV(
                          FORWARD_BINDING_INSTANCE_BUFFER, bufferGroup->instanceBuffer))
            .addItem(nvrhi::BindingSetItem::RawBuffer_SRV(FORWARD_BINDING_VERTEX_BUFFER,
                                                          bufferGroup->vertexBuffer))
            .addItem(nvrhi::BindingSetItem::PushConstants(FORWARD_BINDING_PUSH_CONSTANTS,
                                                          sizeof(ForwardPushConstants)));

    nvrhi::BindingSetHandle bindingSet;
    m_Device->createBindingSet(bindingSetDesc, m_InputBindingLayout, &bindingSet);
    return bindingSet;
}

nvrhi::BindingSetHandle VoxelShadingPass::GetOrCreateInputBindingSet(
    const BufferGroup* bufferGroup) {
    auto it = m_InputBindingSets.find(bufferGroup);
    if (it == m_InputBindingSets.end()) {
        auto bindingSet = CreateInputBindingSet(bufferGroup);
        m_InputBindingSets[bufferGroup] = bindingSet;
        return bindingSet;
    }

    return it->second;
}

void VoxelShadingPass::SetPushConstants(
    donut::render::GeometryPassContext& abstractContext, nvrhi::ICommandList* commandList,
    nvrhi::GraphicsState& state, nvrhi::DrawArguments& args) {
    if (m_UseInputAssembler)
        return;

    auto& context = static_cast<Context&>(abstractContext);

    ForwardPushConstants constants;
    constants.startInstanceLocation = args.startInstanceLocation;
    constants.startVertexLocation = args.startVertexLocation;
    constants.positionOffset = context.positionOffset;
    constants.texCoordOffset = context.texCoordOffset;
    constants.normalOffset = context.normalOffset;
    constants.tangentOffset = context.tangentOffset;

    commandList->setPushConstants(&constants, sizeof(constants));

    args.startInstanceLocation = 0;
    args.startVertexLocation = 0;
}

void VoxelizationInstancedDrawStrategy::PrepareForView(donut::engine::SceneGraphNode* rootNode,
                                               const donut::engine::IView& view) {
    m_Walker = donut::engine::SceneGraphWalker(rootNode);
    m_ClipRegions = static_cast<const VoxelizationView&>(view).GetClipRegions();
    m_InstanceChunk.clear();
    m_ReadPtr = 0;
}

const donut::render::DrawItem* VoxelizationInstancedDrawStrategy::GetNextItem() {
    if(m_ReadPtr >= m_InstancePtrChunk.size())
        FillChunk();

    if(m_InstancePtrChunk.empty())
        return nullptr;

    return m_InstancePtrChunk[m_ReadPtr++];
}

static int CompareDrawItemsOpaque(const DrawItem *a, const DrawItem *b) {
    if(a->material != b->material)
        return a->material < b->material;

    if(a->buffers != b->buffers)
        return a->buffers < b->buffers;

    return a->instance < b->instance;
}

void VoxelizationInstancedDrawStrategy::FillChunk() {
    m_InstanceChunk.resize(m_ChunkSize);

    DrawItem* writePtr = m_InstanceChunk.data();
    size_t itemCount = 0;

    while (m_Walker && itemCount < m_ChunkSize) {
        auto relevantContentFlags = SceneContentFlags::OpaqueMeshes | SceneContentFlags::AlphaTestedMeshes;
        bool subgraphContentRelevant = (m_Walker->GetSubgraphContentFlags() & relevantContentFlags) != 0;
        bool nodeContentsRelevant = (m_Walker->GetLeafContentFlags() & relevantContentFlags) != 0;

        bool nodeVisible = false;
        if (subgraphContentRelevant) {
            nodeVisible = TestClipSceneGraphNode(m_Walker->GetGlobalBoundingBox());

            if (nodeVisible && nodeContentsRelevant) {
                auto meshInstance = donut::query_cast<MeshInstance>(m_Walker->GetLeaf());
                if (meshInstance) {
                    const donut::engine::MeshInfo* mesh = meshInstance->GetMesh();

                    size_t requiredChunkSize = itemCount + mesh->geometries.size();
                    if (m_InstanceChunk.size() < requiredChunkSize) {
                        m_InstanceChunk.resize(requiredChunkSize);
                        writePtr = m_InstanceChunk.data() + itemCount;
                    }

                    for (const auto& geometry : mesh->geometries) {
                        auto domain = geometry->material->domain;
                        if (domain != MaterialDomain::Opaque && domain != MaterialDomain::AlphaTested)
                            continue;

                        if (mesh->geometries.size() > 1 && !mesh->skinPrototype) {
                            dm::box3 geometryGlobalBoundingBox =
                                geometry->objectSpaceBounds * m_Walker->GetLocalToWorldTransformFloat();
                            if (!TestClipSceneGraphNode(geometryGlobalBoundingBox))
                                continue;
                        }

                        DrawItem& item = *writePtr;
                        item.instance = meshInstance;
                        item.mesh = mesh;
                        item.geometry = geometry;
                        item.material = geometry->material;
                        item.buffers = item.mesh->buffers;
                        item.cullMode = (item.material->doubleSided) ? nvrhi::RasterCullMode::None
                                                                     : nvrhi::RasterCullMode::Back;
                        item.distanceToCamera = 0;  // don't care

                        ++writePtr;
                        ++itemCount;
                    }
                }
            }
        }

        m_Walker.Next(nodeVisible);
    }

    m_InstanceChunk.resize(itemCount);
    m_InstancePtrChunk.resize(itemCount);

    for (size_t i = 0; i < itemCount; i++) {
        m_InstancePtrChunk[i] = &m_InstanceChunk[i];
    }

    if (itemCount > 1) {
        std::sort(m_InstancePtrChunk.data(), m_InstancePtrChunk.data() + m_InstancePtrChunk.size(),
                  CompareDrawItemsOpaque);
    }

    m_ReadPtr = 0;
}

bool VoxelizationInstancedDrawStrategy::TestClipSceneGraphNode(const dm::box3& boundingBox) {
    bool clipped = true;

    if(m_ClipRegions.empty())
        return true;

    for(auto &clipRegion : m_ClipRegions) {
        if(clipRegion.intersects(boundingBox))
            clipped = false;
    }

    return !clipped;
}

void VoxelizationView::SetClipRegions(uint32_t numRegions, dm::box3* regions) {
    m_ClipRegions.resize(numRegions);
    if(numRegions)
        std::copy(regions, regions + numRegions, m_ClipRegions.data());
}

const std::vector<dm::box3>& VoxelizationView::GetClipRegions() const { return m_ClipRegions; }

}  // namespace vxgi