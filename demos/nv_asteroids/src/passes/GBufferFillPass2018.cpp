#include "passes/GBufferFillPass2018.h"
#include "app/GpuProfiler.h"

#include "meshlets/MeshletDrawStrategy.h"
#include "scene/SceneMaterial.h"

#include <donut/core/log.h>
#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/FramebufferFactory.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/View.h>
#include <nvrhi/utils.h>

using namespace donut::math;
#include "surface_cb.h"

using namespace donut;
using namespace donut::engine;

namespace
{
    // 2018 VertexAttribute -> input element (0x14007E6E0): POS RGB32F, UV RG32F, NORMAL / TANGENT / BITANGENT
    // RGBA8_SNORM (packed), TRANSFORM / PREV_TRANSFORM 3 x RGBA32F in one 96-byte instance element at offsets 0 / 48.
    nvrhi::VertexAttributeDesc MakeVertexAttribute(const char* name, nvrhi::Format format, uint32_t bufferIndex,
        uint32_t elementStride, uint32_t arraySize = 1, uint32_t offset = 0, bool instanced = false)
    {
        return nvrhi::VertexAttributeDesc()
            .setName(name)
            .setFormat(format)
            .setBufferIndex(bufferIndex)
            .setElementStride(elementStride)
            .setArraySize(arraySize)
            .setOffset(offset)
            .setIsInstanced(instanced);
    }

    // Viewport -> window transform of the previous frame (motion vectors), as computed by 0x1400890D0.
    void GetViewportScaleBias(const nvrhi::Viewport& viewport, float2& scale, float2& bias)
    {
        scale = float2((viewport.maxX - viewport.minX) * 0.5f, (viewport.maxY - viewport.minY) * -0.5f);
        bias = float2(scale.x + viewport.minX, (viewport.maxY - viewport.minY) * 0.5f + viewport.minY);
    }
}

GBufferFillPass2018::GBufferFillPass2018(
    nvrhi::IDevice* device,
    nvrhi::AutoPtr<ShaderFactory> shaderFactory,
    nvrhi::AutoPtr<CommonRenderPasses> commonPasses,
    nvrhi::AutoPtr<FramebufferFactory> framebufferFactory,
    const ICompositeView& compositeView,
    nvrhi::IBindingLayout* materialBindingLayout,
    const CreateParameters& params)
    : m_Device(device)
    , m_CommonPasses(std::move(commonPasses))
    , m_FramebufferFactory(std::move(framebufferFactory))
    , m_MaterialBindingLayout(materialBindingLayout)
{
    m_SupportedViewTypes = ViewType::PLANAR;
    if (params.enableSinglePassStereo)
        m_SupportedViewTypes = ViewType::PLANAR | ViewType::STEREO;
    if (params.enableSinglePassCubemap)
        m_SupportedViewTypes |= ViewType::CUBEMAP;

    const IView* sampleView = compositeView.GetChildView(ViewType::Enum(m_SupportedViewTypes), 0);
    nvrhi::IFramebuffer* sampleFramebuffer = sampleView ? m_FramebufferFactory->GetFramebuffer(*sampleView) : nullptr;
    const bool stereo = sampleView && sampleView->IsStereoView();

    CreateShaders(*shaderFactory, params);

    m_UseAlphaToCoverage = (params.alphaTestedPixelShader == nullptr);

    // Input layout (+8)
    std::vector<nvrhi::VertexAttributeDesc> attributes = {
        MakeVertexAttribute("POS", nvrhi::Format::RGB32_FLOAT, 0, 12),
        MakeVertexAttribute("UV", nvrhi::Format::RG32_FLOAT, 1, 8),
        MakeVertexAttribute("NORMAL", nvrhi::Format::RGBA8_SNORM, 2, 4),
        MakeVertexAttribute("TANGENT", nvrhi::Format::RGBA8_SNORM, 3, 4),
        MakeVertexAttribute("BITANGENT", nvrhi::Format::RGBA8_SNORM, 4, 4),
        MakeVertexAttribute("TRANSFORM", nvrhi::Format::RGBA32_FLOAT, 5, 96, 3, 0, true),
    };
    if (params.enableMotionVectors)
        attributes.push_back(MakeVertexAttribute("PREV_TRANSFORM", nvrhi::Format::RGBA32_FLOAT, 5, 96, 3, 48, true));

    if (m_VertexShader)
        m_Device->createInputLayout(attributes.data(), uint32_t(attributes.size()), m_VertexShader, &m_InputLayout);

    // "GBufferFillConstants" (+56)
    m_Device->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(
        sizeof(surface2018::GBufferFillConstants), "GBufferFillConstants", c_MaxRenderPassConstantBufferVersions), &m_GBufferCB);

    // View binding layout (+48) and set (+80): c_GBuffer at b0 for gbuffer_vs and at b1 for the pixel shader.
    nvrhi::BindingLayoutDesc vertexLayoutDesc;
    vertexLayoutDesc.visibility = nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Geometry;
    vertexLayoutDesc.bindings = { nvrhi::BindingLayoutItem::VolatileConstantBuffer(0) };
    m_Device->createBindingLayout(vertexLayoutDesc, &m_VertexViewBindingLayout);

    nvrhi::BindingLayoutDesc pixelLayoutDesc;
    pixelLayoutDesc.visibility = nvrhi::ShaderType::Pixel;
    pixelLayoutDesc.bindings = { nvrhi::BindingLayoutItem::VolatileConstantBuffer(1) };
    m_Device->createBindingLayout(pixelLayoutDesc, &m_PixelViewBindingLayout);

    nvrhi::BindingSetDesc vertexSetDesc;
    vertexSetDesc.bindings = { nvrhi::BindingSetItem::ConstantBuffer(0, m_GBufferCB) };
    m_Device->createBindingSet(vertexSetDesc, m_VertexViewBindingLayout, &m_VertexViewBindingSet);

    nvrhi::BindingSetDesc pixelSetDesc;
    pixelSetDesc.bindings = { nvrhi::BindingSetItem::ConstantBuffer(1, m_GBufferCB) };
    m_Device->createBindingSet(pixelSetDesc, m_PixelViewBindingLayout, &m_PixelViewBindingSet);

    // Render states of the opaque (+64) and alpha-tested (+72) pipelines.
    const bool reverseDepth = sampleView && sampleView->IsReverseDepth();

    m_OpaqueRenderState = nvrhi::RenderState();
    m_OpaqueRenderState.rasterState
        .setCullBack()
        .setFrontCounterClockwise(true);     // 2018 RasterState byte +762 = 1
    m_OpaqueRenderState.blendState.disableAlphaToCoverage();
    m_OpaqueRenderState.depthStencilState
        .enableDepthTest()
        .setDepthWriteEnable(params.enableDepthWrite)
        .setDepthFunc(reverseDepth ? nvrhi::ComparisonFunc::GreaterOrEqual : nvrhi::ComparisonFunc::LessOrEqual);

    if (params.stencilWriteMask)
    {
        const nvrhi::DepthStencilState::StencilOpDesc stencilOp = nvrhi::DepthStencilState::StencilOpDesc()
            .setPassOp(nvrhi::StencilOp::Replace);

        m_OpaqueRenderState.depthStencilState
            .enableStencil()
            .setStencilReadMask(0)
            .setStencilWriteMask(uint8_t(params.stencilWriteMask))
            .setStencilRefValue(uint8_t(params.stencilWriteMask))
            .setFrontFaceStencil(stencilOp)
            .setBackFaceStencil(stencilOp);
    }

    if (stereo)
        m_OpaqueRenderState.singlePassStereo.setEnabled(true).setIndependentViewportMask(true); // 2018 +816 = 0x101

    m_AlphaTestedRenderState = m_OpaqueRenderState;
    m_AlphaTestedRenderState.rasterState.setCullNone();
    if (m_UseAlphaToCoverage)
        m_AlphaTestedRenderState.blendState.enableAlphaToCoverage();

    if (sampleFramebuffer)
        CreatePipelines(sampleFramebuffer);
}

void GBufferFillPass2018::CreateShaders(ShaderFactory& shaderFactory, const CreateParameters& params)
{
    // Vertex shader (+16): SINGLE_PASS_STEREO and MOTION_VECTORS.
    std::vector<ShaderMacro> vertexMacros = {
        ShaderMacro("SINGLE_PASS_STEREO", params.enableSinglePassStereo ? "1" : "0"),
        ShaderMacro("MOTION_VECTORS", params.enableMotionVectors ? "1" : "0"),
    };

    if (params.enableSinglePassStereo)
    {
        // 2018: NVAPI fast GS with the NV_X_RIGHT / NV_VIEWPORT_MASK custom semantics.
        static nvrhi::CustomSemantic semantics[] = {
            nvrhi::CustomSemantic().setType(nvrhi::CustomSemantic::XRight).setName("NV_X_RIGHT"),
            nvrhi::CustomSemantic().setType(nvrhi::CustomSemantic::ViewportMask).setName("NV_VIEWPORT_MASK"),
        };

        auto vsDesc = nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Vertex).setCustomSemantics(2, semantics);
        m_VertexShader = shaderFactory.CreateShader("framework/passes/gbuffer_vs.hlsl", "main", &vertexMacros, vsDesc);

        std::vector<ShaderMacro> geometryMacros = {
            ShaderMacro("MOTION_VECTORS", params.enableMotionVectors ? "1" : "0"),
        };
        auto gsDesc = nvrhi::ShaderDesc()
            .setShaderType(nvrhi::ShaderType::Geometry)
            .setCustomSemantics(2, semantics)
            .setFastGSFlags(nvrhi::FastGeometryShaderFlags::ForceFastGS | nvrhi::FastGeometryShaderFlags::UseViewportMask);
        m_GeometryShader = shaderFactory.CreateShader("framework/passes/forward_gs.hlsl", "main", &geometryMacros, gsDesc);
    }
    else
    {
        m_VertexShader = shaderFactory.CreateShader("framework/passes/gbuffer_vs.hlsl", "main", &vertexMacros,
            nvrhi::ShaderType::Vertex);

        if (params.enableSinglePassCubemap)
        {
            // deviation: 2018 passed fast-GS flags 13; donut main's cubemap GS setup is used (same shader semantics).
            auto gsDesc = nvrhi::ShaderDesc()
                .setShaderType(nvrhi::ShaderType::Geometry)
                .setFastGSFlags(nvrhi::FastGeometryShaderFlags::ForceFastGS
                    | nvrhi::FastGeometryShaderFlags::UseViewportMask
                    | nvrhi::FastGeometryShaderFlags::OffsetTargetIndexByViewportIndex)
                .setCoordinateSwizzling(CubemapView::GetCubemapCoordinateSwizzle());
            m_GeometryShader = shaderFactory.CreateShader("framework/passes/cubemap_gs.hlsl", "main", nullptr, gsDesc);
        }
    }

    m_PixelShader = params.materialPixelShader;                 // +24
    m_PixelShaderAlphaTested = params.alphaTestedPixelShader;   // +32
}

void GBufferFillPass2018::CreatePipelines(nvrhi::IFramebuffer* framebuffer)
{
    if (!m_VertexShader || !m_PixelShader || !m_InputLayout || !m_MaterialBindingLayout)
        return;

    nvrhi::GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.inputLayout = m_InputLayout;
    pipelineDesc.VS = m_VertexShader;
    pipelineDesc.GS = m_GeometryShader;
    pipelineDesc.PS = m_PixelShader;
    pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;
    pipelineDesc.bindingLayouts = { m_MaterialBindingLayout, m_VertexViewBindingLayout, m_PixelViewBindingLayout };
    pipelineDesc.renderState = m_OpaqueRenderState;
    m_Device->createGraphicsPipeline2(pipelineDesc, framebuffer, &m_OpaquePipeline);

    pipelineDesc.renderState = m_AlphaTestedRenderState;
    if (!m_UseAlphaToCoverage)
        pipelineDesc.PS = m_PixelShaderAlphaTested;

    m_Device->createGraphicsPipeline2(pipelineDesc, framebuffer, &m_AlphaTestedPipeline);
}

bool GBufferFillPass2018::SetupMaterial(SceneMaterial* material, MeshletPassState& state) const
{
    if (!material || !material->bindingSet)
        return false;

    state.bindings[0] = material->bindingSet;

    switch (int(material->domain))
    {
    case 1:     // alpha-tested
        state.renderState = m_AlphaTestedRenderState;
        return true;
    case int(SceneMaterialDomain::Transparent):
        return false;
    default:
        state.renderState = m_OpaqueRenderState;
        return true;
    }
}

void GBufferFillPass2018::Render(
    nvrhi::ICommandList* commandList,
    MeshletDrawStrategy& drawStrategy,
    const ICompositeView& compositeView,
    const ICompositeView* compositeViewPrevious)
{
    demo::ProfBegin(commandList, "GBufferFill");

    const ViewType::Enum viewTypes = ViewType::Enum(m_SupportedViewTypes);
    const ICompositeView& compositeViewPrev = compositeViewPrevious ? *compositeViewPrevious : compositeView;

    for (uint32_t viewIndex = 0; viewIndex < compositeView.GetNumChildViews(viewTypes); viewIndex++)
    {
        const IView* view = compositeView.GetChildView(viewTypes, viewIndex);
        const IView* viewPrev = compositeViewPrev.GetChildView(viewTypes, viewIndex);

        surface2018::GBufferFillConstants constants = {};

        const nvrhi::ViewportState viewportPrev = viewPrev->GetViewportState();

        if (view->IsStereoView())
        {
            const IView* left = view->GetChildView(ViewType::PLANAR, 0);
            const IView* right = view->GetChildView(ViewType::PLANAR, 1);
            constants.matWorldToView = affineToHomogeneous(left->GetViewMatrix());
            constants.matViewToClip = left->GetProjectionMatrix(true);
            constants.matWorldToViewRight = affineToHomogeneous(right->GetViewMatrix());
            constants.matViewToClipRight = right->GetProjectionMatrix(true);

            const IView* leftPrev = viewPrev->GetChildView(ViewType::PLANAR, 0);
            const IView* rightPrev = viewPrev->GetChildView(ViewType::PLANAR, 1);
            constants.matWorldToClipPrev = leftPrev->GetViewProjectionMatrix(false);
            constants.matWorldToClipRightPrev = rightPrev->GetViewProjectionMatrix(false);

            if (viewportPrev.viewports.size() > 1)
                GetViewportScaleBias(viewportPrev.viewports[1], constants.viewportScaleRightPrev, constants.viewportBiasRightPrev);
        }
        else
        {
            constants.matWorldToView = affineToHomogeneous(view->GetViewMatrix());
            constants.matViewToClip = view->GetProjectionMatrix(true);
            constants.matWorldToClipPrev = viewPrev->GetViewProjectionMatrix(false);
        }

        if (!viewportPrev.viewports.empty())
            GetViewportScaleBias(viewportPrev.viewports[0], constants.viewportScalePrev, constants.viewportBiasPrev);

        constants.pixelOffset = view->GetPixelOffset();

        commandList->writeBuffer(m_GBufferCB, &constants, sizeof(constants));

        MeshletPassState passState;
        passState.framebuffer = m_FramebufferFactory->GetFramebuffer(*view);
        passState.viewport = view->GetViewportState();
        passState.renderState = m_OpaqueRenderState;
        passState.bindingLayouts = { m_MaterialBindingLayout, m_PixelViewBindingLayout };
        // [0] is the material set, filled by SetupMaterial for every material the strategy draws.
        passState.bindings = { nullptr, m_PixelViewBindingSet };

        drawStrategy.Render(commandList, passState, *view,
            [this](SceneMaterial* material, MeshletPassState& state) { return SetupMaterial(material, state); });
    }

    demo::ProfEnd(commandList);
}
