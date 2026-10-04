#include "passes/ForwardShadingPass2018.h"
#include "app/GpuProfiler.h"

#include "meshlets/MeshletDrawStrategy.h"
#include "passes/SurfaceLighting2018.h"
#include "scene/SceneMaterial.h"

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

ForwardShadingPass2018::ForwardShadingPass2018(
    nvrhi::IDevice* device,
    nvrhi::AutoPtr<ShaderFactory> shaderFactory,
    nvrhi::AutoPtr<CommonRenderPasses> commonPasses,
    nvrhi::AutoPtr<FramebufferFactory> framebufferFactory,
    const ICompositeView& compositeView,
    nvrhi::IBindingLayout* materialBindingLayout,
    bool singlePassStereo,
    bool singlePassCubemap,
    bool trackLiveness)
    : m_Device(device)
    , m_MaterialBindingLayout(materialBindingLayout)
    , m_TrackLiveness(trackLiveness)
    , m_BindingSets(std::make_unique<BindingCache>(device))
    , m_CommonPasses(std::move(commonPasses))
    , m_FramebufferFactory(std::move(framebufferFactory))
{
    (void)shaderFactory;

    m_SupportedViewTypes = ViewType::PLANAR;
    if (singlePassStereo)
        m_SupportedViewTypes = ViewType::PLANAR | ViewType::STEREO;
    if (singlePassCubemap)
        m_SupportedViewTypes |= ViewType::CUBEMAP;

    const IView* sampleView = compositeView.GetChildView(ViewType::Enum(m_SupportedViewTypes), 0);
    const bool reverseDepth = sampleView && sampleView->IsReverseDepth();
    const bool stereo = sampleView && sampleView->IsStereoView();

    // Shadow sampler (+40): linear, border address mode with a white border (lit outside the shadow map).
    auto samplerDesc = nvrhi::SamplerDesc()
        .setAllFilters(true)
        .setAllAddressModes(nvrhi::SamplerAddressMode::Border)
        .setBorderColor(nvrhi::Color(1.f));
    m_Device->createSampler(samplerDesc, &m_ShadowSampler);

    // Binding layouts (+48 PS section, +56).
    nvrhi::BindingLayoutDesc forwardLayoutDesc;
    forwardLayoutDesc.visibility = nvrhi::ShaderType::Pixel;
    forwardLayoutDesc.bindings = {
        nvrhi::BindingLayoutItem::VolatileConstantBuffer(1),    // c_Forward
        nvrhi::BindingLayoutItem::Texture_SRV(4),               // t_ShadowMapArray
        nvrhi::BindingLayoutItem::Sampler(1),                   // s_ShadowSampler
    };
    m_Device->createBindingLayout(forwardLayoutDesc, &m_ForwardBindingLayout);

    nvrhi::BindingLayoutDesc probeLayoutDesc;
    probeLayoutDesc.visibility = nvrhi::ShaderType::Pixel;
    probeLayoutDesc.bindings = {
        nvrhi::BindingLayoutItem::Texture_SRV(5),               // t_DiffuseLightProbe
        nvrhi::BindingLayoutItem::Texture_SRV(6),               // t_SpecularLightProbe
        nvrhi::BindingLayoutItem::Texture_SRV(7),               // t_EnvironmentBrdf
        nvrhi::BindingLayoutItem::Sampler(2),                   // s_LightProbeSampler
        nvrhi::BindingLayoutItem::Sampler(3),                   // s_BrdfSampler
    };
    m_Device->createBindingLayout(probeLayoutDesc, &m_LightProbeBindingLayout);

    // "ForwardShadingConstants" (+72)
    m_Device->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(
        sizeof(surface2018::ForwardShadingConstants), "ForwardShadingConstants", c_MaxRenderPassConstantBufferVersions), &m_ForwardCB);

    // Render states of the opaque (+80), alpha-tested (+88) and transparent (+96) pipelines.
    m_OpaqueRenderState = nvrhi::RenderState();
    m_OpaqueRenderState.rasterState
        .setCullBack()
        .setFrontCounterClockwise(true);    // 2018 RasterState byte +762 = 1
    m_OpaqueRenderState.blendState.disableAlphaToCoverage();
    m_OpaqueRenderState.depthStencilState
        .enableDepthTest()
        .enableDepthWrite()
        .setDepthFunc(reverseDepth ? nvrhi::ComparisonFunc::GreaterOrEqual : nvrhi::ComparisonFunc::LessOrEqual);
    if (stereo)
        m_OpaqueRenderState.singlePassStereo.setEnabled(true).setIndependentViewportMask(true);

    m_AlphaTestedRenderState = m_OpaqueRenderState;
    m_AlphaTestedRenderState.rasterState.setCullNone();
    m_AlphaTestedRenderState.blendState.enableAlphaToCoverage();

    // Transparent: premultiplied blending (One, InvSrcAlpha; alpha Zero, One), no depth writes. The cull mode
    // stays at "none" from the alpha-tested pipeline (the 2018 code reuses the same desc).
    m_TransparentRenderState = m_AlphaTestedRenderState;
    m_TransparentRenderState.blendState.disableAlphaToCoverage();
    m_TransparentRenderState.blendState.targets[0]
        .enableBlend()
        .setSrcBlend(nvrhi::BlendFactor::One)
        .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
        .setBlendOp(nvrhi::BlendOp::Add)
        .setSrcBlendAlpha(nvrhi::BlendFactor::Zero)
        .setDestBlendAlpha(nvrhi::BlendFactor::One)
        .setBlendOpAlpha(nvrhi::BlendOp::Add);
    m_TransparentRenderState.depthStencilState.disableDepthWrite();
}

ForwardShadingPass2018::~ForwardShadingPass2018() = default;

void ForwardShadingPass2018::ResetBindingCache()
{
    m_BindingSets->Clear();
}

bool ForwardShadingPass2018::SetupMaterial(SceneMaterial* material, MeshletPassState& state) const
{
    if (!material)
        return false;

    switch (int(material->domain))
    {
    case int(SceneMaterialDomain::Opaque):
        state.renderState = m_OpaqueRenderState;
        break;
    case 1:     // alpha-tested
        state.renderState = m_AlphaTestedRenderState;
        break;
    case int(SceneMaterialDomain::Transparent):
        state.renderState = m_TransparentRenderState;
        break;
    default:
        return false;
    }

    if (!material->bindingSet)
        return false;

    state.bindings[0] = material->bindingSet;
    state.bindings[1] = m_CurrentForwardSet;
    state.bindings[2] = m_CurrentLightProbeSet;
    return true;
}

void ForwardShadingPass2018::Render(
    nvrhi::ICommandList* commandList,
    MeshletDrawStrategy& drawStrategy,
    const ICompositeView& compositeView,
    const std::vector<std::shared_ptr<SceneLight>>& lights,
    const float3& ambientColorTop,
    const float3& ambientColorBottom,
    const std::vector<nvrhi::AutoPtr<LightProbe>>& lightProbes)
{
    demo::ProfBegin(commandList, "ForwardShading");

    // Forward binding set, cached per shadow map texture (2018 hash map at +112).
    const surface_lighting::ShadowMapInfo shadowMap = surface_lighting::FindShadowMap(lights);

    nvrhi::BindingSetDesc forwardSetDesc;
    forwardSetDesc.trackLiveness = m_TrackLiveness;
    forwardSetDesc.bindings = {
        nvrhi::BindingSetItem::ConstantBuffer(1, m_ForwardCB),
        nvrhi::BindingSetItem::Texture_SRV(4, shadowMap.texture ? shadowMap.texture : m_CommonPasses->m_BlackTexture2DArray.Get()),
        nvrhi::BindingSetItem::Sampler(1, m_ShadowSampler),
    };
    m_CurrentForwardSet = m_BindingSets->GetOrCreateBindingSet(forwardSetDesc, m_ForwardBindingLayout);

    // Light probe binding set, cached per probe texture set (2018 hash map at +176).
    surface_lighting::LightProbeTextures probeTextures;
    if (!surface_lighting::CollectLightProbeTextures(lightProbes, "ForwardShadingPass", probeTextures))
    {
        demo::ProfEnd(commandList);
        return;
    }

    nvrhi::BindingSetDesc probeSetDesc;
    probeSetDesc.trackLiveness = m_TrackLiveness;
    probeSetDesc.bindings = {
        nvrhi::BindingSetItem::Texture_SRV(5, probeTextures.diffuse ? probeTextures.diffuse : m_CommonPasses->m_BlackCubeMapArray.Get()),
        nvrhi::BindingSetItem::Texture_SRV(6, probeTextures.specular ? probeTextures.specular : m_CommonPasses->m_BlackCubeMapArray.Get()),
        nvrhi::BindingSetItem::Texture_SRV(7, probeTextures.environmentBrdf ? probeTextures.environmentBrdf : m_CommonPasses->m_BlackTexture.Get()),
        nvrhi::BindingSetItem::Sampler(2, m_CommonPasses->m_LinearWrapSampler),
        nvrhi::BindingSetItem::Sampler(3, m_CommonPasses->m_LinearClampSampler),
    };
    m_CurrentLightProbeSet = m_BindingSets->GetOrCreateBindingSet(probeSetDesc, m_LightProbeBindingLayout);

    const ViewType::Enum viewTypes = ViewType::Enum(m_SupportedViewTypes);

    for (uint32_t viewIndex = 0; viewIndex < compositeView.GetNumChildViews(viewTypes); viewIndex++)
    {
        const IView* view = compositeView.GetChildView(viewTypes, viewIndex);

        surface2018::ForwardShadingConstants constants = {};

        auto cameraDirectionOrPosition = [](const IView* v)
        {
            return v->IsOrthographicProjection() ? float4(v->GetViewDirection(), 0.f) : float4(v->GetViewOrigin(), 1.f);
        };

        if (view->IsStereoView())
        {
            const IView* left = view->GetChildView(ViewType::PLANAR, 0);
            const IView* right = view->GetChildView(ViewType::PLANAR, 1);
            constants.matWorldToClip = left->GetViewProjectionMatrix(true);
            const float4x4 rightWorldToClip = right->GetViewProjectionMatrix(true);
            constants.worldToClipXRight = float4(rightWorldToClip.row0.x, rightWorldToClip.row1.x,
                rightWorldToClip.row2.x, rightWorldToClip.row3.x);
            constants.cameraDirectionOrPosition = cameraDirectionOrPosition(left);
            constants.cameraDirectionOrPositionRight = cameraDirectionOrPosition(right);
        }
        else
        {
            constants.matWorldToClip = view->GetViewProjectionMatrix(true);
            constants.cameraDirectionOrPosition = cameraDirectionOrPosition(view);
        }

        // 2018: only the size is written; shadowMapTextureSizeInv stays 0 (forward_ps does not read it).
        constants.shadowMapTextureSize = float2(shadowMap.size);
        constants.ambientColorTop = float4(ambientColorTop, 0.f);
        constants.ambientColorBottom = float4(ambientColorBottom, 0.f);

        constants.numLights = surface_lighting::FillLights(lights, constants.lights, constants.shadows);
        constants.numLightProbes = surface_lighting::FillLightProbes(lightProbes, constants.lightProbes);

        commandList->writeBuffer(m_ForwardCB, &constants, sizeof(constants));

        MeshletPassState passState;
        passState.framebuffer = m_FramebufferFactory->GetFramebuffer(*view);
        passState.viewport = view->GetViewportState();
        passState.renderState = m_OpaqueRenderState;
        passState.bindingLayouts = { m_MaterialBindingLayout, m_ForwardBindingLayout, m_LightProbeBindingLayout };
        // [0] is the material set, filled by SetupMaterial for every material the strategy draws.
        passState.bindings = { nullptr, m_CurrentForwardSet, m_CurrentLightProbeSet };

        drawStrategy.Render(commandList, passState, *view,
            [this](SceneMaterial* material, MeshletPassState& state) { return SetupMaterial(material, state); });
    }

    demo::ProfEnd(commandList);
}
