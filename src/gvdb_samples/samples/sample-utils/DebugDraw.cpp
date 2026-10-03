// Immediate-mode world-space lines (shaders/Lines.hlsl).
#include "DebugDraw.h"
#include <donut/core/log.h>
#include <nvrhi/utils.h>
#include <iterator>

namespace SampleUtils {

using namespace donut;

namespace {
struct LinesConstants {
    dm::float4x4 matViewProj;
};
}  // namespace

DebugDraw::DebugDraw(nvrhi::IDevice *device, engine::ShaderFactory *shaderFactory, const nvrhi::FramebufferInfoEx &fbInfo)
    : m_device(device) {
    auto vs = shaderFactory->CreateShader("Lines.hlsl", "VSMain", nullptr, nvrhi::ShaderType::Vertex);
    auto ps = shaderFactory->CreateShader("Lines.hlsl", "PSMain", nullptr, nvrhi::ShaderType::Pixel);
    if (!vs || !ps) {
        log::error("DebugDraw: Lines.hlsl shaders not found");
        return;
    }

    nvrhi::VertexAttributeDesc attributes[] = {
        nvrhi::VertexAttributeDesc()
            .setName("POSITION")
            .setFormat(nvrhi::Format::RGB32_FLOAT)
            .setOffset(offsetof(Vertex, position))
            .setBufferIndex(0)
            .setElementStride(sizeof(Vertex)),
        nvrhi::VertexAttributeDesc()
            .setName("COLOR")
            .setFormat(nvrhi::Format::RGBA32_FLOAT)
            .setOffset(offsetof(Vertex, color))
            .setBufferIndex(0)
            .setElementStride(sizeof(Vertex)),
    };
    m_device->createInputLayout(attributes, uint32_t(std::size(attributes)), vs, &m_inputLayout);

    nvrhi::BufferDesc cbDesc = nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(LinesConstants), "DebugDraw constants", 16);
    m_device->createBuffer(cbDesc, &m_constantBuffer);

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Vertex;
    layoutDesc.bindings = {nvrhi::BindingLayoutItem::VolatileConstantBuffer(0)};
    m_device->createBindingLayout(layoutDesc, &m_bindingLayout);

    nvrhi::BindingSetDesc setDesc;
    setDesc.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, m_constantBuffer)};
    m_device->createBindingSet(setDesc, m_bindingLayout, &m_bindingSet);

    nvrhi::GraphicsPipelineDesc psoDesc;
    psoDesc.primType = nvrhi::PrimitiveType::LineList;
    psoDesc.VS = vs;
    psoDesc.PS = ps;
    psoDesc.inputLayout = m_inputLayout;
    psoDesc.bindingLayouts = {m_bindingLayout};
    psoDesc.renderState.depthStencilState.depthTestEnable = false;
    psoDesc.renderState.depthStencilState.depthWriteEnable = false;
    psoDesc.renderState.rasterState.setCullNone();
    auto &blend = psoDesc.renderState.blendState.targets[0];
    blend.blendEnable = true;
    blend.srcBlend = nvrhi::BlendFactor::SrcAlpha;
    blend.destBlend = nvrhi::BlendFactor::InvSrcAlpha;
    blend.srcBlendAlpha = nvrhi::BlendFactor::One;
    blend.destBlendAlpha = nvrhi::BlendFactor::InvSrcAlpha;
    m_device->createGraphicsPipeline1(psoDesc, fbInfo.getInfo(), &m_pipeline);
}

DebugDraw::~DebugDraw() {}

void DebugDraw::line(dm::float3 a, dm::float3 b, dm::float4 color) {
    m_vertices.push_back({a, color});
    m_vertices.push_back({b, color});
}

void DebugDraw::box(dm::float3 bmin, dm::float3 bmax, dm::float4 color, const dm::affine3 *xform) {
    dm::float3 c[8];
    for (int i = 0; i < 8; ++i) {
        c[i] = dm::float3((i & 1) ? bmax.x : bmin.x, (i & 2) ? bmax.y : bmin.y, (i & 4) ? bmax.z : bmin.z);
        if (xform) c[i] = xform->transformPoint(c[i]);
    }
    static const int edges[12][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7},
                                     {7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (auto &e : edges) line(c[e[0]], c[e[1]], color);
}

void DebugDraw::topology(gvdb::GVDBVolumeInstance *instance) {
    if (!instance || !instance->GetVolume()) return;
    topology(instance->GetVolume(), instance->GetIndexToWorld());
}

void DebugDraw::topology(gvdb::IGVDBVolume *volume, const dm::affine3 &indexToWorld) {
    if (!volume) return;
    for (uint8_t lev = 0; lev < volume->getNumLevels(); ++lev) {
        dm::float4 color(volume->getLevelColor(lev), 1.f);
        uint64_t count = volume->getNumNodes(lev);
        for (uint64_t n = 0; n < count; ++n) {
            gvdb::Node *node = volume->getNode(lev, n);
            if (!node) continue;
            if (lev == 0 && node->flags == 0) continue;   // inactive brick
            dm::box3 b = volume->getNodeBounds(node);
            box(b.m_mins, b.m_maxs, color, &indexToWorld);
        }
    }
}

void DebugDraw::frustum(dm::float3 eye, dm::float3 rayTL, dm::float3 rayU, dm::float3 rayV, float length,
                        dm::float4 color) {
    dm::float3 corners[4] = {rayTL, rayTL + rayU, rayTL + rayU + rayV, rayTL + rayV};
    dm::float3 p[4];
    for (int i = 0; i < 4; ++i) {
        p[i] = eye + dm::normalize(corners[i]) * length;
        line(eye, p[i], color);
    }
    for (int i = 0; i < 4; ++i) line(p[i], p[(i + 1) % 4], color);
}

void DebugDraw::flush(nvrhi::ICommandList *commandList, nvrhi::IFramebuffer *framebuffer, const dm::float4x4 &viewProj) {
    if (m_vertices.empty() || !m_pipeline) {
        m_vertices.clear();
        return;
    }
    if (m_vertexCapacity < m_vertices.size() || !m_vertexBuffer) {
        m_vertexCapacity = std::max<size_t>(m_vertices.size() * 2, 4096);
        nvrhi::BufferDesc desc;
        desc.byteSize = m_vertexCapacity * sizeof(Vertex);
        desc.isVertexBuffer = true;
        desc.initialState = nvrhi::ResourceStates::VertexBuffer;
        desc.keepInitialState = true;
        desc.debugName = "DebugDraw vertices";
        m_vertexBuffer = nullptr;
        m_device->createBuffer(desc, &m_vertexBuffer);
    }
    commandList->writeBuffer(m_vertexBuffer, m_vertices.data(), m_vertices.size() * sizeof(Vertex));

    LinesConstants constants;
    constants.matViewProj = viewProj;
    commandList->writeBuffer(m_constantBuffer, &constants, sizeof(constants));

    const nvrhi::FramebufferInfoEx &fbInfo = framebuffer->getFramebufferInfo();
    nvrhi::GraphicsState state;
    state.pipeline = m_pipeline;
    state.framebuffer = framebuffer;
    state.bindings = {m_bindingSet};
    state.vertexBuffers = {nvrhi::VertexBufferBinding{m_vertexBuffer, 0, 0}};
    state.viewport.addViewportAndScissorRect(nvrhi::Viewport(float(fbInfo.width), float(fbInfo.height)));
    commandList->setGraphicsState(state);

    nvrhi::DrawArguments args;
    args.vertexCount = uint32_t(m_vertices.size());
    commandList->draw(args);

    m_vertices.clear();
}

}  // namespace SampleUtils
