// SPH particle sets in a Donut scene graph and the point rasterizer
// (Particles::Draw and the SPNT shader of the fluids5.0 reference).
#include "SPHFluidScene.h"
#include <donut/core/log.h>
#include <iterator>

namespace sph {

using namespace donut;

// ---- SPHFluidInstance -------------------------------------------------------

dm::box3 SPHFluidInstance::GetLocalBoundingBox() {
    if (!m_particles) return dm::box3::empty();
    return m_particles->getBounds();
}

nvrhi::AutoPtr<engine::SceneGraphLeaf> SPHFluidInstance::Clone() {
    auto copy = MAKE_RC_OBJ_PTR(SPHFluidInstance, m_particles.Get());   // shares the particle set
    copy->m_attributes = m_attributes;
    return copy;
}

dm::affine3 SPHFluidInstance::GetLocalToWorld() const {
    engine::SceneGraphNode *node = GetNode();
    if (!node) return dm::affine3::identity();
    return node->GetLocalToWorldTransformFloat();
}

void collectFluidInstances(engine::SceneGraphNode *root, std::vector<SPHFluidInstance *> &outInstances) {
    if (!root) return;
    engine::SceneGraphWalker walker(root);
    while (walker) {
        // Skip sub-trees without particle leaves.
        const bool relevant = (walker->GetSubgraphContentFlags() & SceneContentFlags_Particles) != engine::SceneContentFlags(0) ||
                              (walker->GetLeafContentFlags() & SceneContentFlags_Particles) != engine::SceneContentFlags(0);
        if (relevant) {
            if (auto instance = dynamic_cast<SPHFluidInstance *>(walker->GetLeaf())) outInstances.push_back(instance);
        }
        walker.Next(relevant);
    }
}

// ---- SPHPointRenderer -------------------------------------------------------

namespace {
// PointsConstants of shaders/Points.hlsl
struct PointsConstants {
    dm::float4x4 matWorldViewProj;
    float velocityTint;
    dm::float3 padding;
};
static_assert(sizeof(PointsConstants) == 80, "PointsConstants layout");
}  // namespace

struct SPHPointRenderer::Impl {
    nvrhi::DeviceHandle device;
    nvrhi::ShaderHandle vertexShader;
    nvrhi::ShaderHandle pixelShader;
    nvrhi::InputLayoutHandle inputLayout;
    nvrhi::BindingLayoutHandle bindingLayout;
    nvrhi::BindingSetHandle bindingSet;
    nvrhi::GraphicsPipelineHandle pipeline;
    nvrhi::FramebufferInfo pipelineFramebufferInfo;
    bool warnedNoBuffers = false;

    bool ensurePipeline(const nvrhi::FramebufferInfo &fbInfo) {
        if (pipeline && pipelineFramebufferInfo == fbInfo) return true;
        if (!vertexShader || !pixelShader || !bindingLayout) return false;
        pipeline = nullptr;

        nvrhi::GraphicsPipelineDesc psoDesc;
        psoDesc.primType = nvrhi::PrimitiveType::PointList;
        psoDesc.inputLayout = inputLayout;
        psoDesc.VS = vertexShader;
        psoDesc.PS = pixelShader;
        psoDesc.bindingLayouts = {bindingLayout};
        psoDesc.renderState.rasterState.setCullNone();
        // selfDraw3D of the reference: depth test LESS (writing), SRC_ALPHA / ONE_MINUS_SRC_ALPHA blending.
        psoDesc.renderState.depthStencilState.depthTestEnable = true;
        psoDesc.renderState.depthStencilState.depthWriteEnable = true;
        psoDesc.renderState.depthStencilState.depthFunc = nvrhi::ComparisonFunc::Less;
        psoDesc.renderState.depthStencilState.stencilEnable = false;
        auto &blend = psoDesc.renderState.blendState.targets[0];
        blend.blendEnable = true;
        blend.srcBlend = nvrhi::BlendFactor::SrcAlpha;
        blend.destBlend = nvrhi::BlendFactor::InvSrcAlpha;
        blend.srcBlendAlpha = nvrhi::BlendFactor::SrcAlpha;
        blend.destBlendAlpha = nvrhi::BlendFactor::InvSrcAlpha;
        device->createGraphicsPipeline1(psoDesc, fbInfo, &pipeline);
        if (!pipeline) {
            log::error("SPHPointRenderer: cannot create the point pipeline");
            return false;
        }
        pipelineFramebufferInfo = fbInfo;
        return true;
    }
};

SPHPointRenderer::SPHPointRenderer(nvrhi::IDevice *device, engine::ShaderFactory *shaderFactory)
    : m_impl(std::make_unique<Impl>()) {
    Impl &s = *m_impl;
    s.device = device;
    s.vertexShader = shaderFactory->CreateShader("sph_fluid_pool/Points.hlsl", "main_vs", nullptr, nvrhi::ShaderType::Vertex);
    s.pixelShader = shaderFactory->CreateShader("sph_fluid_pool/Points.hlsl", "main_ps", nullptr, nvrhi::ShaderType::Pixel);
    if (!s.vertexShader || !s.pixelShader) {
        log::error("SPHPointRenderer: sph_fluid_pool/Points.hlsl shaders not found");
        return;
    }

    // One stream per particle buffer: position, packed colour, velocity.
    nvrhi::VertexAttributeDesc attributes[] = {
        nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RGB32_FLOAT).setBufferIndex(0).setElementStride(
            particleBufferStride(ParticleBuffer::Position)),
        nvrhi::VertexAttributeDesc().setName("COLOR").setFormat(nvrhi::Format::R32_UINT).setBufferIndex(1).setElementStride(
            particleBufferStride(ParticleBuffer::Color)),
        nvrhi::VertexAttributeDesc().setName("VELOCITY").setFormat(nvrhi::Format::RGB32_FLOAT).setBufferIndex(2).setElementStride(
            particleBufferStride(ParticleBuffer::Velocity)),
    };
    device->createInputLayout(attributes, uint32_t(std::size(attributes)), s.vertexShader, &s.inputLayout);

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel;
    layoutDesc.bindings = {nvrhi::BindingLayoutItem::PushConstants(0, sizeof(PointsConstants))};
    device->createBindingLayout(layoutDesc, &s.bindingLayout);

    nvrhi::BindingSetDesc setDesc;
    setDesc.bindings = {nvrhi::BindingSetItem::PushConstants(0, sizeof(PointsConstants))};
    if (s.bindingLayout) device->createBindingSet(setDesc, s.bindingLayout, &s.bindingSet);
    if (!s.inputLayout || !s.bindingLayout || !s.bindingSet) log::error("SPHPointRenderer: cannot create the pipeline inputs");
}

SPHPointRenderer::~SPHPointRenderer() {}

void SPHPointRenderer::render(nvrhi::ICommandList *commandList, nvrhi::IFramebuffer *framebuffer,
                              const std::vector<SPHFluidInstance *> &instances, const dm::float4x4 &viewProj) {
    Impl &s = *m_impl;
    if (!commandList || !framebuffer || instances.empty()) return;
    if (!s.inputLayout || !s.bindingSet) return;
    const nvrhi::FramebufferInfoEx &fbInfo = framebuffer->getFramebufferInfo();
    if (!s.ensurePipeline(fbInfo.getInfo())) return;

    for (SPHFluidInstance *instance : instances) {
        if (!instance || !instance->GetRenderAttributes().visible) continue;
        ISPHParticles *particles = instance->GetParticles();
        if (!particles || particles->getNumParticles() == 0) continue;

        nvrhi::IBuffer *positions = particles->getRenderBuffer(ParticleBuffer::Position);
        nvrhi::IBuffer *colors = particles->getRenderBuffer(ParticleBuffer::Color);
        nvrhi::IBuffer *velocities = particles->getRenderBuffer(ParticleBuffer::Velocity);
        if (!positions || !colors || !velocities) {
            if (!s.warnedNoBuffers)
                log::warning("SPHPointRenderer: a particle set without shared vertex buffers is skipped "
                             "(configure it with an interop device)");
            s.warnedNoBuffers = true;
            continue;
        }

        PointsConstants constants = {};
        constants.matWorldViewProj = dm::affineToHomogeneous(instance->GetLocalToWorld()) * viewProj;
        constants.velocityTint = instance->GetRenderAttributes().velocityTint;

        nvrhi::GraphicsState state;
        state.pipeline = s.pipeline;
        state.framebuffer = framebuffer;
        state.bindings = {s.bindingSet};
        state.vertexBuffers = {nvrhi::VertexBufferBinding{positions, 0, 0}, nvrhi::VertexBufferBinding{colors, 1, 0},
                               nvrhi::VertexBufferBinding{velocities, 2, 0}};
        state.viewport.addViewportAndScissorRect(nvrhi::Viewport(float(fbInfo.width), float(fbInfo.height)));
        commandList->setGraphicsState(state);
        commandList->setPushConstants(&constants, sizeof(constants));

        nvrhi::DrawArguments args;
        args.vertexCount = particles->getNumParticles();
        commandList->draw(args);
    }
}

}  // namespace sph
