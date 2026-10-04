#include "fx/FogPass.h"
#include "app/GpuProfiler.h"
#include "fx/FxCommon.h"
#include "scene/Lights.h"

#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/FramebufferFactory.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/ShadowMap.h>
#include <donut/engine/View.h>

#include <cmath>
#include <cstddef>

using namespace donut::math;
#include <space_cb.h>

using namespace donut::engine;

namespace fx
{
    // 4x4 ordered-dither pattern ((bayer + 1) / 17), rows from the .rdata constants
    // 0x140256820, 0x140256800 (two rows) and 0x1402567F0.
    static const float4 c_FogNoisePattern[4] = {
        float4(0.059f, 0.529f, 0.176f, 0.647f),
        float4(0.765f, 0.294f, 0.882f, 0.412f),
        float4(0.235f, 0.706f, 0.118f, 0.588f),
        float4(0.941f, 0.471f, 0.824f, 0.353f)
    };

    static nvrhi::ViewportState MakeViewportState(const nvrhi::Viewport& viewport)
    {
        nvrhi::ViewportState state;
        state.addViewport(viewport);
        state.addScissorRect(nvrhi::Rect(
            int(floorf(viewport.minX)), int(ceilf(viewport.maxX)),
            int(floorf(viewport.minY)), int(ceilf(viewport.maxY))));
        return state;
    }

    FogPass::FogPass(
        nvrhi::IDevice* device,
        const nvrhi::AutoPtr<ShaderFactory>& shaderFactory,
        const nvrhi::AutoPtr<CommonRenderPasses>& commonPasses,
        const nvrhi::AutoPtr<FramebufferFactory>& framebufferFactory,
        nvrhi::ITexture* depthBuffer,
        const ICompositeView& compositeView)
        : m_Device(device)
        , m_DepthBuffer(depthBuffer)
        , m_TraceBindingSets(device)
        , m_CommonPasses(commonPasses)
        , m_FramebufferFactory(framebufferFactory)
    {
        const IView* sampleView = compositeView.GetChildView(ViewType::PLANAR, 0);
        nvrhi::IFramebuffer* outputFramebuffer = m_FramebufferFactory->GetFramebuffer(*sampleView);

        device->createBuffer(ConstantBufferDesc(sizeof(FogConstants), "FogConstants"), &m_FogConstants);

        // 2018 sampler: border addressing (white border), linear filters, shadow compare.
        auto shadowSamplerDesc = nvrhi::SamplerDesc()
            .setAllAddressModes(nvrhi::SamplerAddressMode::Border)
            .setBorderColor(1.f)
            .setAllFilters(true)
            .setReductionType(nvrhi::SamplerReductionType::Comparison);
        device->createSampler(shadowSamplerDesc, &m_ShadowSampler);

        m_TracePixelShader = shaderFactory->CreateShader("demo/fog.hlsl", "ps_trace", nullptr, nvrhi::ShaderType::Pixel);
        m_FilterPixelShader = shaderFactory->CreateShader("demo/fog.hlsl", "ps_filter", nullptr, nvrhi::ShaderType::Pixel);

        // Half-resolution trace target.
        const nvrhi::TextureDesc& depthDesc = depthBuffer->getDesc();
        nvrhi::TextureDesc fogDesc;
        fogDesc.width = (depthDesc.width + 1) >> 1;
        fogDesc.height = (depthDesc.height + 1) >> 1;
        // unresolved: 2018 format enum value 34; RGBA16_FLOAT is assumed (HDR inscatter + alpha).
        fogDesc.format = nvrhi::Format::RGBA16_FLOAT;
        fogDesc.isRenderTarget = true;
        fogDesc.initialState = nvrhi::ResourceStates::RenderTarget;
        fogDesc.keepInitialState = true;
        fogDesc.debugName = "UnfilteredFog";
        device->createTexture(fogDesc, &m_UnfilteredFog);

        device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(m_UnfilteredFog), &m_TraceFramebuffer);

        // Trace: b0 FogConstants, t0 depth, t1 shadow map array, s0 shadow sampler.
        nvrhi::BindingLayoutDesc traceLayoutDesc;
        traceLayoutDesc.visibility = nvrhi::ShaderType::Pixel;
        traceLayoutDesc.bindings = {
            nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
            nvrhi::BindingLayoutItem::Texture_SRV(0),
            nvrhi::BindingLayoutItem::Texture_SRV(1),
            nvrhi::BindingLayoutItem::Sampler(0)
        };
        device->createBindingLayout(traceLayoutDesc, &m_TraceBindingLayout);

        nvrhi::GraphicsPipelineDesc tracePipelineDesc;
        tracePipelineDesc.primType = nvrhi::PrimitiveType::TriangleStrip;
        tracePipelineDesc.VS = m_CommonPasses->m_FullscreenVS;
        tracePipelineDesc.PS = m_TracePixelShader;
        tracePipelineDesc.bindingLayouts = { m_TraceBindingLayout };
        tracePipelineDesc.renderState.rasterState.setCullNone();
        tracePipelineDesc.renderState.depthStencilState.disableDepthTest().disableDepthWrite();
        device->createGraphicsPipeline2(tracePipelineDesc, m_TraceFramebuffer, &m_TracePipeline);

        // Filter: b0 FogConstants, t0 depth, t1 unfiltered fog.
        nvrhi::BindingLayoutDesc filterLayoutDesc;
        filterLayoutDesc.visibility = nvrhi::ShaderType::Pixel;
        filterLayoutDesc.bindings = {
            nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
            nvrhi::BindingLayoutItem::Texture_SRV(0),
            nvrhi::BindingLayoutItem::Texture_SRV(1)
        };
        device->createBindingLayout(filterLayoutDesc, &m_FilterBindingLayout);

        nvrhi::BindingSetDesc filterSetDesc;
        filterSetDesc.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, m_FogConstants),
            nvrhi::BindingSetItem::Texture_SRV(0, m_DepthBuffer),
            nvrhi::BindingSetItem::Texture_SRV(1, m_UnfilteredFog)
        };
        device->createBindingSet(filterSetDesc, m_FilterBindingLayout, &m_FilterBindingSet);

        nvrhi::GraphicsPipelineDesc filterPipelineDesc;
        filterPipelineDesc.primType = nvrhi::PrimitiveType::TriangleStrip;
        filterPipelineDesc.VS = m_CommonPasses->m_FullscreenVS;
        filterPipelineDesc.PS = m_FilterPixelShader;
        filterPipelineDesc.bindingLayouts = { m_FilterBindingLayout };
        filterPipelineDesc.renderState.rasterState.setCullNone();
        filterPipelineDesc.renderState.depthStencilState.disableDepthTest().disableDepthWrite();
        filterPipelineDesc.renderState.blendState.targets[0] = BlendStateRT(nvrhi::BlendFactor::One, nvrhi::BlendFactor::InvSrcAlpha);
        device->createGraphicsPipeline2(filterPipelineDesc, outputFramebuffer, &m_FilterPipeline);
    }

    void FogPass::Render(
        nvrhi::ICommandList* commandList,
        const ICompositeView& compositeView,
        const float3& cameraOffset,
        const SceneDirectionalLight& light,
        const FogParameters& params,
        const float3& randomOffset,
        float maxShadowDistance)
    {
        demo::ProfBegin(commandList, "Fog");

        const IShadowMap* shadowMap = light.shadowMap.Get();
        nvrhi::ITexture* shadowTexture = shadowMap ? shadowMap->GetTexture() : m_CommonPasses->m_BlackTexture2DArray.Get();

        nvrhi::BindingSetDesc traceSetDesc;
        traceSetDesc.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, m_FogConstants),
            nvrhi::BindingSetItem::Texture_SRV(0, m_DepthBuffer),
            nvrhi::BindingSetItem::Texture_SRV(1, shadowTexture),
            nvrhi::BindingSetItem::Sampler(0, m_ShadowSampler)
        };
        nvrhi::BindingSetHandle traceBindingSet = m_TraceBindingSets.GetOrCreateBindingSet(traceSetDesc, m_TraceBindingLayout);

        for (uint32_t viewIndex = 0; viewIndex < compositeView.GetNumChildViews(ViewType::PLANAR); viewIndex++)
        {
            const IView* view = compositeView.GetChildView(ViewType::PLANAR, viewIndex);

            const nvrhi::Viewport fullViewport = view->GetViewportState().viewports[0];
            const nvrhi::Viewport halfViewport(
                floorf(fullViewport.minX * 0.5f), ceilf(fullViewport.maxX * 0.5f),
                floorf(fullViewport.minY * 0.5f), ceilf(fullViewport.maxY * 0.5f),
                fullViewport.minZ, fullViewport.maxZ);

            const float4x4 projection = view->GetProjectionMatrix(false);

            FogConstants constants{};
            constants.matClipToView = view->GetInverseProjectionMatrix(false);
            constants.matViewToWorld = affineToHomogeneous(view->GetInverseViewMatrix());
            constants.cameraPos = float4(view->GetViewOrigin(), 1.f);
            for (int i = 0; i < 4; i++)
                constants.noisePattern[i] = c_FogNoisePattern[i];
            constants.randomOffset = randomOffset;
            constants.maxShadowDistance = maxShadowDistance;
            constants.projectionA = projection[2][2] / projection[2][3];
            constants.projectionB = projection[3][2] / projection[2][3];

            FillLightConstants2018(light, constants.light);
            FillShadowConstants2018(shadowMap, constants.light, constants.shadows, FOG_MAX_SHADOWS, false);

            constants.light.radiance *= params.lightIntensityScale;
            constants.meanHeight = cameraOffset.y + params.meanHeight;
            constants.thickness = params.thickness;
            constants.densityScale = params.densityScale;
            constants.visibilityThreshold = params.visibilityThreshold;
            constants.color = params.color;
            constants.intensity = params.intensity;

            commandList->writeBuffer(m_FogConstants, &constants, sizeof(constants));

            nvrhi::DrawArguments args;
            args.vertexCount = 4;
            args.instanceCount = 1;

            // Trace at half resolution.
            nvrhi::GraphicsState traceState;
            traceState.pipeline = m_TracePipeline;
            traceState.framebuffer = m_TraceFramebuffer;
            traceState.bindings = { traceBindingSet };
            traceState.viewport = MakeViewportState(halfViewport);
            commandList->setGraphicsState(traceState);
            commandList->draw(args);

            // Depth-aware upsample, blended over the view's framebuffer.
            nvrhi::GraphicsState filterState;
            filterState.pipeline = m_FilterPipeline;
            filterState.framebuffer = m_FramebufferFactory->GetFramebuffer(*view);
            filterState.bindings = { m_FilterBindingSet };
            filterState.viewport = MakeViewportState(fullViewport);
            commandList->setGraphicsState(filterState);
            commandList->draw(args);
        }

        demo::ProfEnd(commandList);
    }
}
