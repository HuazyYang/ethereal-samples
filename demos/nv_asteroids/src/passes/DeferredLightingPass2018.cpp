#include "passes/DeferredLightingPass2018.h"
#include "app/GpuProfiler.h"

#include "passes/SurfaceLighting2018.h"

#include <donut/core/log.h>
#include <donut/engine/BindingCache.h>
#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/FramebufferFactory.h>
#include <donut/engine/SceneTypes.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/View.h>
#include <nvrhi/utils.h>

using namespace donut::math;
#include "surface_cb.h"

using namespace donut;
using namespace donut::engine;

nvrhi::BindingLayoutHandle DeferredLightingPass2018::CreateGBufferBindingLayout(nvrhi::IDevice* device)
{
    // Asteroids.exe: CreateRenderPasses (0x14002BD60), FeatureDemo+680.
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Pixel;
    layoutDesc.bindings = {
        nvrhi::BindingLayoutItem::Texture_SRV(8),
        nvrhi::BindingLayoutItem::Texture_SRV(9),
        nvrhi::BindingLayoutItem::Texture_SRV(10),
        nvrhi::BindingLayoutItem::Texture_SRV(11),
    };
    return device->createBindingLayout(layoutDesc);
}

nvrhi::BindingSetHandle DeferredLightingPass2018::CreateGBufferBindingSet(nvrhi::IDevice* device,
    nvrhi::IBindingLayout* layout, nvrhi::ITexture* depth, nvrhi::ITexture* gbuffer0, nvrhi::ITexture* gbuffer1,
    nvrhi::ITexture* gbuffer2)
{
    // Asteroids.exe: CreateRenderPasses (0x14002BD60), FeatureDemo+688 (render targets 0, 2, 3, 4).
    nvrhi::BindingSetDesc setDesc;
    setDesc.bindings = {
        nvrhi::BindingSetItem::Texture_SRV(8, depth),
        nvrhi::BindingSetItem::Texture_SRV(9, gbuffer0),
        nvrhi::BindingSetItem::Texture_SRV(10, gbuffer1),
        nvrhi::BindingSetItem::Texture_SRV(11, gbuffer2),
    };
    return device->createBindingSet(setDesc, layout);
}

DeferredLightingPass2018::DeferredLightingPass2018(
    nvrhi::IDevice* device,
    std::shared_ptr<ShaderFactory> shaderFactory,
    std::shared_ptr<CommonRenderPasses> commonPasses,
    std::shared_ptr<FramebufferFactory> framebufferFactory,
    const ICompositeView& compositeView,
    nvrhi::IShader* pixelShader,
    nvrhi::IBindingLayout* gbufferBindingLayout)
    : m_Device(device)
    , m_PixelShader(pixelShader)
    , m_GBufferBindingLayout(gbufferBindingLayout)
    , m_BindingSets(std::make_unique<BindingCache>(device))
    , m_CommonPasses(std::move(commonPasses))
    , m_FramebufferFactory(std::move(framebufferFactory))
{
    (void)shaderFactory;

    // Shadow samplers (+16, +24): linear, border address mode with a white border; the second one compares.
    auto samplerDesc = nvrhi::SamplerDesc()
        .setAllFilters(true)
        .setAllAddressModes(nvrhi::SamplerAddressMode::Border)
        .setBorderColor(nvrhi::Color(1.f));
    m_ShadowSampler = m_Device->createSampler(samplerDesc);

    samplerDesc.setReductionType(nvrhi::SamplerReductionType::Comparison);
    m_ShadowSamplerComparison = m_Device->createSampler(samplerDesc);

    // "DeferredLightingConstants" (+32)
    m_DeferredCB = m_Device->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(
        sizeof(surface2018::DeferredLightingConstants), "DeferredLightingConstants", c_MaxRenderPassConstantBufferVersions));

    // Binding layouts (+56, +64)
    nvrhi::BindingLayoutDesc shadowLayoutDesc;
    shadowLayoutDesc.visibility = nvrhi::ShaderType::Pixel;
    shadowLayoutDesc.bindings = {
        nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),    // c_Deferred
        nvrhi::BindingLayoutItem::Texture_SRV(0),               // t_ShadowMapArray
        nvrhi::BindingLayoutItem::Sampler(0),                   // s_ShadowSampler
        nvrhi::BindingLayoutItem::Sampler(1),                   // s_ShadowSamplerComparison
    };
    m_ShadowBindingLayout = m_Device->createBindingLayout(shadowLayoutDesc);

    nvrhi::BindingLayoutDesc probeLayoutDesc;
    probeLayoutDesc.visibility = nvrhi::ShaderType::Pixel;
    probeLayoutDesc.bindings = {
        nvrhi::BindingLayoutItem::Texture_SRV(1),               // t_DiffuseLightProbe
        nvrhi::BindingLayoutItem::Texture_SRV(2),               // t_SpecularLightProbe
        nvrhi::BindingLayoutItem::Texture_SRV(3),               // t_EnvironmentBrdf
        nvrhi::BindingLayoutItem::Texture_SRV(4),               // t_IndirectDiffuse
        nvrhi::BindingLayoutItem::Sampler(2),                   // s_LightProbeSampler
        nvrhi::BindingLayoutItem::Sampler(3),                   // s_BrdfSampler
    };
    m_LightProbeBindingLayout = m_Device->createBindingLayout(probeLayoutDesc);

    // Pipeline (+40): full-screen strip, no depth test, no culling.
    const IView* sampleView = compositeView.GetChildView(ViewType::PLANAR, 0);
    nvrhi::IFramebuffer* framebuffer = sampleView ? m_FramebufferFactory->GetFramebuffer(*sampleView) : nullptr;

    if (m_PixelShader && framebuffer)
    {
        nvrhi::GraphicsPipelineDesc pipelineDesc;
        pipelineDesc.primType = nvrhi::PrimitiveType::TriangleStrip;
        pipelineDesc.VS = m_CommonPasses->m_FullscreenVS;
        pipelineDesc.PS = m_PixelShader;
        pipelineDesc.bindingLayouts = { m_GBufferBindingLayout, m_ShadowBindingLayout, m_LightProbeBindingLayout };
        pipelineDesc.renderState.rasterState.setCullNone();
        pipelineDesc.renderState.depthStencilState.disableDepthTest().disableDepthWrite().disableStencil();

        m_Pipeline = m_Device->createGraphicsPipeline(pipelineDesc, framebuffer);
    }
    else
    {
        log::error("DeferredLightingPass2018: missing pixel shader or framebuffer");
    }
}

DeferredLightingPass2018::~DeferredLightingPass2018() = default;

void DeferredLightingPass2018::ResetBindingCache()
{
    m_BindingSets->Clear();
}

void DeferredLightingPass2018::Render(
    nvrhi::ICommandList* commandList,
    const ICompositeView& compositeView,
    const std::vector<std::shared_ptr<SceneLight>>& lights,
    nvrhi::IBindingSet* gbufferBindingSet,
    float2 randomOffset,
    const float3& ambientColorTop,
    const float3& ambientColorBottom,
    const std::vector<std::shared_ptr<LightProbe>>& lightProbes,
    nvrhi::ITexture* indirectDiffuse)
{
    if (!m_Pipeline || !gbufferBindingSet)
        return;

    demo::ProfBegin(commandList, "DeferredLighting");

    // Shadow binding set, cached per shadow map texture (2018 hash map at +72).
    const surface_lighting::ShadowMapInfo shadowMap = surface_lighting::FindShadowMap(lights);

    nvrhi::BindingSetDesc shadowSetDesc;
    shadowSetDesc.bindings = {
        nvrhi::BindingSetItem::ConstantBuffer(0, m_DeferredCB),
        nvrhi::BindingSetItem::Texture_SRV(0, shadowMap.texture ? shadowMap.texture : m_CommonPasses->m_BlackTexture2DArray.Get()),
        nvrhi::BindingSetItem::Sampler(0, m_ShadowSampler),
        nvrhi::BindingSetItem::Sampler(1, m_ShadowSamplerComparison),
    };
    nvrhi::BindingSetHandle shadowSet = m_BindingSets->GetOrCreateBindingSet(shadowSetDesc, m_ShadowBindingLayout);

    // Light probe binding set, cached per (probe textures, indirect diffuse) (2018 hash map at +136).
    surface_lighting::LightProbeTextures probeTextures;
    if (!surface_lighting::CollectLightProbeTextures(lightProbes, "DeferredLightingPass", probeTextures))
    {
        demo::ProfEnd(commandList);
        return;
    }

    nvrhi::BindingSetDesc probeSetDesc;
    probeSetDesc.bindings = {
        nvrhi::BindingSetItem::Texture_SRV(1, probeTextures.diffuse ? probeTextures.diffuse : m_CommonPasses->m_BlackCubeMapArray.Get()),
        nvrhi::BindingSetItem::Texture_SRV(2, probeTextures.specular ? probeTextures.specular : m_CommonPasses->m_BlackCubeMapArray.Get()),
        nvrhi::BindingSetItem::Texture_SRV(3, probeTextures.environmentBrdf ? probeTextures.environmentBrdf : m_CommonPasses->m_BlackTexture.Get()),
        nvrhi::BindingSetItem::Texture_SRV(4, indirectDiffuse ? indirectDiffuse : m_CommonPasses->m_BlackTexture.Get()),
        nvrhi::BindingSetItem::Sampler(2, m_CommonPasses->m_LinearWrapSampler),
        nvrhi::BindingSetItem::Sampler(3, m_CommonPasses->m_LinearClampSampler),
    };
    nvrhi::BindingSetHandle probeSet = m_BindingSets->GetOrCreateBindingSet(probeSetDesc, m_LightProbeBindingLayout);

    for (uint32_t viewIndex = 0; viewIndex < compositeView.GetNumChildViews(ViewType::PLANAR); viewIndex++)
    {
        const IView* view = compositeView.GetChildView(ViewType::PLANAR, viewIndex);

        surface2018::DeferredLightingConstants constants = {};
        constants.matClipToView = inverse(view->GetProjectionMatrix(true));
        constants.matViewToWorld = affineToHomogeneous(inverse(view->GetViewMatrix()));
        constants.shadowMapTextureSize = float2(shadowMap.size);
        constants.gbufferArraySlice = int(view->GetSubresources().baseArraySlice);
        constants.indirectDiffuseScale = indirectDiffuse ? 1.f : 0.f;
        constants.ambientColorTop = float4(ambientColorTop, 0.f);
        constants.ambientColorBottom = float4(ambientColorBottom, 0.f);
        constants.cameraDirectionOrPosition = view->IsOrthographicProjection()
            ? float4(view->GetViewDirection(), 0.f)
            : float4(view->GetViewOrigin(), 1.f);
        constants.randomOffset = randomOffset;

        // 4x4 rotation pattern of the Poisson shadow kernel (0x140256800..0x140256830, same values as donut 2021).
        constants.noisePattern[0] = float4(0.059f, 0.529f, 0.176f, 0.647f);
        constants.noisePattern[1] = float4(0.765f, 0.294f, 0.882f, 0.412f);
        constants.noisePattern[2] = float4(0.235f, 0.706f, 0.118f, 0.588f);
        constants.noisePattern[3] = float4(0.941f, 0.471f, 0.824f, 0.353f);

        constants.numLights = surface_lighting::FillLights(lights, constants.lights, constants.shadows);
        constants.numLightProbes = surface_lighting::FillLightProbes(lightProbes, constants.lightProbes);

        commandList->writeBuffer(m_DeferredCB, &constants, sizeof(constants));

        nvrhi::GraphicsState state;
        state.pipeline = m_Pipeline;
        state.framebuffer = m_FramebufferFactory->GetFramebuffer(*view);
        state.viewport = view->GetViewportState();
        state.bindings = { gbufferBindingSet, shadowSet, probeSet };
        commandList->setGraphicsState(state);

        nvrhi::DrawArguments args;
        args.vertexCount = 4;
        args.instanceCount = 1;
        commandList->draw(args);
    }

    demo::ProfEnd(commandList);
}
