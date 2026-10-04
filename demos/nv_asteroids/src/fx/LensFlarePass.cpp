#include "fx/LensFlarePass.h"

#include "app/RenderHandedness.h"
#include "app/GpuProfiler.h"
#include "fx/FxCommon.h"

#include "scene/Lights.h"

#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/FramebufferFactory.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/ShadowMap.h>
#include <donut/engine/View.h>

#include <algorithm>
#include <cmath>
#include <cstddef>

using namespace donut::math;
#include <space_cb.h>

using namespace donut::engine;

namespace fx
{
    LensFlarePass::LensFlarePass(
        nvrhi::IDevice* device,
        const nvrhi::AutoPtr<ShaderFactory>& shaderFactory,
        const nvrhi::AutoPtr<CommonRenderPasses>& commonPasses,
        const nvrhi::AutoPtr<FramebufferFactory>& framebufferFactory,
        const ICompositeView& compositeView)
        : m_Device(device)
        , m_CommonPasses(commonPasses)
    {
        m_VertexShader = shaderFactory->CreateShader("demo/lensflare.hlsl", "vs_main", nullptr, nvrhi::ShaderType::Vertex);
        m_PixelShader = shaderFactory->CreateShader("demo/lensflare.hlsl", "ps_main", nullptr, nvrhi::ShaderType::Pixel);

        // deviation: the 2018 buffer was 768 bytes (the HLSL cbuffer is 804: 'pad[3]' spills into
        // three more registers); D3D12 needs the full size, so the C++ struct size is used.
        device->createBuffer(ConstantBufferDesc(sizeof(LensFlareConstants), "LensFlareConstants"), &m_LensFlareConstants);

        // Shadow comparison sampler: border addressing, linear filtering, comparison reduction.
        // unresolved: the 2018 border colour constant (xmmword_140256830) was not decoded; 1.0 = lit.
        nvrhi::SamplerDesc samplerDesc;
        samplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::Border)
            .setBorderColor(nvrhi::Color(1.f))
            .setAllFilters(true)
            .setMaxAnisotropy(1.f)
            .setReductionType(nvrhi::SamplerReductionType::Comparison);
        device->createSampler(samplerDesc, &m_ShadowSampler);

        // b0 LensFlareConstants (VS + PS), t0 t_ShadowMapArray, s0 s_ShadowSampler (VS).
        nvrhi::BindingLayoutDesc layoutDesc;
        layoutDesc.visibility = nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel;
        layoutDesc.bindings = {
            nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
            nvrhi::BindingLayoutItem::Texture_SRV(0),
            nvrhi::BindingLayoutItem::Sampler(0)
        };
        device->createBindingLayout(layoutDesc, &m_BindingLayout);

        const IView* sampleView = compositeView.GetChildView(ViewType::PLANAR, 0);

        nvrhi::GraphicsPipelineDesc pipelineDesc;
        pipelineDesc.primType = nvrhi::PrimitiveType::TriangleStrip;
        pipelineDesc.VS = m_VertexShader;
        pipelineDesc.PS = m_PixelShader;
        pipelineDesc.bindingLayouts = { m_BindingLayout };
        // Additive (One, One); no depth test or write.
        pipelineDesc.renderState.blendState.targets[0] = BlendStateRT(nvrhi::BlendFactor::One, nvrhi::BlendFactor::One);
        pipelineDesc.renderState.rasterState.setCullNone();
        pipelineDesc.renderState.depthStencilState.disableDepthTest().disableDepthWrite().disableStencil();

        device->createGraphicsPipeline2(pipelineDesc, framebufferFactory->GetFramebuffer(*sampleView), &m_Pipeline);
    }

    void LensFlarePass::Render(
        nvrhi::ICommandList* commandList,
        const ICompositeView& compositeView,
        const nvrhi::AutoPtr<FramebufferFactory>& framebufferFactory,
        const SceneDirectionalLight& sun,
        const float3& cameraPosition)
    {
        demo::ProfBegin(commandList, "LensFlare");

        // One binding set per shadow map texture (2018: std::map keyed by the texture pointer).
        nvrhi::ITexture* shadowTexture = sun.shadowMap
            ? sun.shadowMap->GetTexture()
            : m_CommonPasses->m_BlackTexture2DArray.Get();

        nvrhi::BindingSetHandle& bindingSet = m_BindingSets[shadowTexture];
        if (!bindingSet)
        {
            nvrhi::BindingSetDesc setDesc;
            setDesc.bindings = {
                nvrhi::BindingSetItem::ConstantBuffer(0, m_LensFlareConstants),
                nvrhi::BindingSetItem::Texture_SRV(0, shadowTexture),
                nvrhi::BindingSetItem::Sampler(0, m_ShadowSampler)
            };
            m_Device->createBindingSet(setDesc, m_BindingLayout, &bindingSet);
        }

        for (uint32_t viewIndex = 0; viewIndex < compositeView.GetNumChildViews(ViewType::PLANAR); viewIndex++)
        {
            const IView* view = compositeView.GetChildView(ViewType::PLANAR, viewIndex);

            nvrhi::GraphicsState state;
            state.pipeline = m_Pipeline;
            state.framebuffer = framebufferFactory->GetFramebuffer(*view);
            state.bindings = { bindingSet };
            state.viewport = view->GetViewportState();

            LensFlareConstants constants{};
            constants.matWorldToClip = view->GetViewProjectionMatrix(false);
            constants.cameraPos = float4(-cameraPosition, 1.f);

            const float4x4 projection = view->GetProjectionMatrix(false);
            constants.screenScale = demo::ProjectionScale(projection);   // an aspect ratio: magnitudes

            FillLightConstants2018(sun, constants.light);
            FillShadowConstants2018(sun.shadowMap.Get(), constants.light, constants.shadows, LENSFLARE_MAX_SHADOWS, true);

            // Project a point far along the direction towards the sun.
            float4 sunClip = float4(constants.light.direction * -1000000.f, 1.f) * view->GetViewProjectionMatrix(true);
            sunClip /= fabsf(sunClip.w);

            // Fade the flare out as the sun leaves the screen (fully off at 2x the screen extent).
            float irradiance = 0.f;
            if (sun.GetLightType() == 1 /* LightType_Directional */)
            {
                const float fadeY = std::clamp(2.f - fabsf(sunClip.y), 0.f, 1.f);
                const float fadeX = std::clamp(2.f - fabsf(sunClip.x), 0.f, 1.f);
                irradiance = sun.irradiance * (fadeX * fadeY);
            }

            if (sunClip.w > 0.f && irradiance > 0.f)
            {
                constants.pos = float2(-sunClip.x, sunClip.y);
                constants.irradiance = irradiance;

                commandList->writeBuffer(m_LensFlareConstants, &constants, sizeof(constants));
                commandList->setGraphicsState(state);

                nvrhi::DrawArguments args;
                args.vertexCount = 4;
                args.instanceCount = 1;
                commandList->draw(args);
            }
        }

        demo::ProfEnd(commandList);
    }
}
