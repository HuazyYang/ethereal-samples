#include "fx/RectPass.h"
#include "fx/FxCommon.h"

#include <donut/engine/FramebufferFactory.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/View.h>

#include <cstddef>

#include <algorithm>
#include <cmath>

using namespace donut::math;
#include <space_cb.h>

using namespace donut::engine;

namespace fx
{
    RectPass::RectPass(
        nvrhi::IDevice* device,
        const nvrhi::AutoPtr<ShaderFactory>& shaderFactory,
        const char* pixelShaderFile,
        const nvrhi::BindingLayoutDesc& pixelBindingLayoutDesc,
        const nvrhi::BlendState::RenderTarget& blendState,
        const nvrhi::AutoPtr<FramebufferFactory>& framebufferFactory,
        const ICompositeView& compositeView)
        : m_FramebufferFactory(framebufferFactory)
    {
        m_VertexShader = shaderFactory->CreateShader("demo/RectPass_vs.hlsl", "main", nullptr, nvrhi::ShaderType::Vertex);
        m_PixelShader = shaderFactory->CreateShader(pixelShaderFile, "main", nullptr, nvrhi::ShaderType::Pixel);

        device->createBuffer(ConstantBufferDesc(sizeof(RectConstants), "RectConstants"), &m_RectConstants);

        nvrhi::BindingLayoutDesc vertexLayoutDesc;
        vertexLayoutDesc.visibility = nvrhi::ShaderType::Vertex;
        vertexLayoutDesc.bindings = { nvrhi::BindingLayoutItem::VolatileConstantBuffer(0) };
        device->createBindingLayout(vertexLayoutDesc, &m_VertexBindingLayout);

        nvrhi::BindingSetDesc vertexSetDesc;
        vertexSetDesc.bindings = { nvrhi::BindingSetItem::ConstantBuffer(0, m_RectConstants) };
        device->createBindingSet(vertexSetDesc, m_VertexBindingLayout, &m_VertexBindingSet);

        nvrhi::BindingLayoutDesc pixelLayoutDesc = pixelBindingLayoutDesc;
        pixelLayoutDesc.visibility = nvrhi::ShaderType::Pixel;
        device->createBindingLayout(pixelLayoutDesc, &m_PixelBindingLayout);

        const IView* sampleView = compositeView.GetChildView(ViewType::PLANAR, 0);

        nvrhi::GraphicsPipelineDesc pipelineDesc;
        pipelineDesc.primType = nvrhi::PrimitiveType::TriangleStrip;
        pipelineDesc.VS = m_VertexShader;
        pipelineDesc.PS = m_PixelShader;
        pipelineDesc.bindingLayouts = { m_PixelBindingLayout, m_VertexBindingLayout };
        pipelineDesc.renderState.blendState.targets[0] = blendState;
        pipelineDesc.renderState.rasterState.setCullNone();
        pipelineDesc.renderState.depthStencilState
            .enableDepthTest()
            .disableDepthWrite()
            .disableStencil()
            .setDepthFunc(sampleView->IsReverseDepth()
                ? nvrhi::ComparisonFunc::GreaterOrEqual
                : nvrhi::ComparisonFunc::LessOrEqual);

        device->createGraphicsPipeline1(pipelineDesc,
            m_FramebufferFactory->GetFramebuffer(*sampleView)->getFramebufferInfo().getInfo(), &m_Pipeline);
    }

    void RectPass::Render(
        nvrhi::ICommandList* commandList,
        const IView& view,
        const float3& direction,
        float angularSize,
        float distance,
        nvrhi::IBindingSet* pixelBindingSet) const
    {
        // Camera-relative view-projection: the quad is built around the eye.
        affine3 viewMatrix = view.GetViewMatrix();
        viewMatrix.m_translation = 0.f;
        const float4x4 viewProjection = affineToHomogeneous(viewMatrix) * view.GetProjectionMatrix(true);

        const float halfAngle = radians(angularSize * 0.5f);
        const float quadDistance = std::min(cosf(halfAngle), 0.9f) * distance;
        const float halfSize = tanf(halfAngle) * quadDistance;

        const float3 center = normalize(direction) * quadDistance;

        // Avoid a degenerate basis when looking straight up or down.
        const float3 up0 = (sqrtf(direction.x * direction.x + direction.z * direction.z) <= 0.01f)
            ? float3(1.f, 0.f, 0.f)
            : float3(0.f, 1.f, 0.f);
        const float3 right = normalize(cross(up0, direction)) * halfSize;
        const float3 up = normalize(cross(direction, normalize(cross(up0, direction)))) * halfSize;

        // Triangle strip order, matching ST in RectPass_vs: (-1,1) (1,1) (-1,-1) (1,-1).
        const float3 corners[4] = {
            center - right + up,
            center + right + up,
            center - right - up,
            center + right - up
        };

        RectConstants constants{};
        for (int i = 0; i < 4; i++)
        {
            constants.vertices[i] = float4(corners[i], 1.f) * viewProjection;
            constants.directions[i] = float4(normalize(corners[i]), 0.f);
        }
        commandList->writeBuffer(m_RectConstants, &constants, sizeof(constants));

        nvrhi::GraphicsState state;
        state.pipeline = m_Pipeline;
        state.framebuffer = m_FramebufferFactory->GetFramebuffer(view);
        state.bindings = { pixelBindingSet, m_VertexBindingSet };
        state.viewport = view.GetViewportState();

        // Collapse the depth range onto the far plane so that the quad lies behind all geometry.
        for (auto& viewport : state.viewport.viewports)
        {
            if (view.IsReverseDepth())
                viewport.maxZ = viewport.minZ;
            else
                viewport.minZ = viewport.maxZ;
        }

        commandList->setGraphicsState(state);

        nvrhi::DrawArguments args;
        args.vertexCount = 4;
        args.instanceCount = 1;
        commandList->draw(args);
    }
}
