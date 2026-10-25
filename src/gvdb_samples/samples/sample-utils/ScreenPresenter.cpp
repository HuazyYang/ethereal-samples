// CUDA render buffers -> shared nvrhi textures -> swap chain.
//
// Synchronisation: the interop device orders the queues with shared timeline
// fences. Before the compute context writes the shared textures, the CUDA queue
// waits for the graphics work executed so far (the blits of the previous frame);
// after the copies are submitted, the next graphics submission waits for them.
#include "ScreenPresenter.h"
#include "SampleTypes.h"
#include <donut/core/log.h>
#include <nvrhi/utils.h>
#include <iterator>

namespace SampleUtils {

using namespace donut;

namespace {
// The CUDA device's queue the compute context submits to, and the graphics queue that presents.
constexpr nvrhi::CommandQueue c_computeQueue = nvrhi::CommandQueue::Graphics;
constexpr nvrhi::CommandQueue c_graphicsQueue = nvrhi::CommandQueue::Graphics;
}  // namespace

struct ScreenPresenter::Target {
    uint32_t width = 0;
    uint32_t height = 0;
    nvrhi::BufferHandle renderBuffer;       // CUDA device
    nvrhi::TextureHandle texture;           // graphics device, shared
    nvrhi::TextureHandle textureCuda;       // the same memory on the CUDA device
    uint64_t surface = 0;                   // surface handle of textureCuda
    nvrhi::BindingSetHandle bindingSet;
};

ScreenPresenter::ScreenPresenter(nvrhi::IDevice *device, nvrhi::IInteropDevice *interop, gvdb::IComputeContext *compute,
                                 engine::ShaderFactory *shaderFactory, const nvrhi::FramebufferInfoEx &fbInfo)
    : m_device(device), m_interop(interop), m_compute(compute) {
    m_copyKernel = m_compute->getKernel(GVDB_SHADERS_RAYCAST, "gvdbCopyToSurface");

    auto vs = shaderFactory->CreateShader("FullScreen.hlsl", "VSMain", nullptr, nvrhi::ShaderType::Vertex);
    std::vector<engine::ShaderMacro> defines = {{"HAS_TEXTURE_2", "0"}};
    auto ps = shaderFactory->CreateShader("FullScreen.hlsl", "PSMain", &defines, nvrhi::ShaderType::Pixel);
    if (!vs || !ps) {
        log::error("ScreenPresenter: FullScreen.hlsl shaders not found");
        return;
    }

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel;
    layoutDesc.bindings = {nvrhi::BindingLayoutItem::PushConstants(0, sizeof(dm::float4)),
                           nvrhi::BindingLayoutItem::Sampler(0), nvrhi::BindingLayoutItem::Texture_SRV(0)};
    m_device->createBindingLayout(layoutDesc, &m_bindingLayout);

    nvrhi::SamplerDesc samplerDesc;
    samplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::ClampToEdge);
    samplerDesc.setAllFilters(false);
    m_device->createSampler(samplerDesc, &m_sampler);

    nvrhi::GraphicsPipelineDesc psoDesc;
    psoDesc.primType = nvrhi::PrimitiveType::TriangleStrip;
    psoDesc.VS = vs;
    psoDesc.PS = ps;
    psoDesc.bindingLayouts = {m_bindingLayout};
    psoDesc.renderState.depthStencilState.depthTestEnable = false;
    psoDesc.renderState.depthStencilState.depthWriteEnable = false;
    psoDesc.renderState.rasterState.setCullNone();
    m_device->createGraphicsPipeline1(psoDesc, fbInfo.getInfo(), &m_pipeline);
}

ScreenPresenter::~ScreenPresenter() {
    if (m_compute) m_compute->finish();
}

int ScreenPresenter::createTarget(uint32_t width, uint32_t height) {
    m_targets.push_back(std::make_unique<Target>());
    int id = int(m_targets.size()) - 1;
    resizeTarget(id, width, height);
    return id;
}

void ScreenPresenter::resizeTarget(int id, uint32_t width, uint32_t height) {
    if (id < 0 || id >= int(m_targets.size())) return;
    Target &t = *m_targets[id];
    width = std::max(width, 1u);
    height = std::max(height, 1u);
    if (t.width == width && t.height == height && t.texture) return;

    // Make sure neither device still uses the old resources.
    m_compute->finish();
    m_device->waitForIdle();

    t.width = width;
    t.height = height;
    t.bindingSet = nullptr;
    t.textureCuda = nullptr;
    t.texture = nullptr;
    t.renderBuffer = nullptr;
    t.surface = 0;

    nvrhi::BufferDesc bufDesc;
    bufDesc.byteSize = size_t(width) * height * 4;
    bufDesc.debugName = "ScreenPresenter render buffer";
    UT_CHECK(m_compute->getDevice()->createBuffer(bufDesc, &t.renderBuffer));
    if (t.renderBuffer) m_compute->getCommandList()->clearBufferUInt(t.renderBuffer, 0u);

    nvrhi::TextureDesc texDesc;
    texDesc.dimension = nvrhi::TextureDimension::Texture2D;
    texDesc.width = width;
    texDesc.height = height;
    texDesc.format = nvrhi::Format::RGBA8_UNORM;
    texDesc.isShaderResource = true;
    texDesc.isUAV = true;   // CUDA writes the imported texture through a surface
    texDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    texDesc.keepInitialState = true;
    texDesc.debugName = "ScreenPresenter target";
    texDesc.sharedResourceFlags = interopSharedFlags();
    m_device->createTexture(texDesc, &t.texture);
    if (!t.texture) {
        log::error("ScreenPresenter: cannot create the shared texture %ux%u", width, height);
        return;
    }
    UT_CHECK(m_interop->importTexture(t.texture, &t.textureCuda));
    if (t.textureCuda) t.surface = m_compute->getDevice()->getSurfaceHandle(t.textureCuda, 0);

    nvrhi::BindingSetDesc setDesc;
    setDesc.bindings = {nvrhi::BindingSetItem::PushConstants(0, sizeof(dm::float4)),
                        nvrhi::BindingSetItem::Sampler(0, m_sampler), nvrhi::BindingSetItem::Texture_SRV(0, t.texture)};
    m_device->createBindingSet(setDesc, m_bindingLayout, &t.bindingSet);
}

uint32_t ScreenPresenter::getWidth(int id) const {
    return (id >= 0 && id < int(m_targets.size())) ? m_targets[id]->width : 0;
}
uint32_t ScreenPresenter::getHeight(int id) const {
    return (id >= 0 && id < int(m_targets.size())) ? m_targets[id]->height : 0;
}
nvrhi::IBuffer *ScreenPresenter::getRenderBuffer(int id) const {
    return (id >= 0 && id < int(m_targets.size())) ? m_targets[id]->renderBuffer.Get() : nullptr;
}
nvrhi::ITexture *ScreenPresenter::getTexture(int id) const {
    return (id >= 0 && id < int(m_targets.size())) ? m_targets[id]->texture.Get() : nullptr;
}

void ScreenPresenter::beginComputeFrame() {
    // The CUDA queue waits until the graphics queue has finished reading the textures of the previous frame.
    UT_CHECK(m_interop->computeQueueWaitForGraphicsQueue(c_computeQueue, c_graphicsQueue));
}

void ScreenPresenter::endComputeFrame() {
    nvrhi::ICommandList *commandList = m_compute->getCommandList();
    for (auto &tp : m_targets) {
        Target &t = *tp;
        if (!t.surface || !t.renderBuffer) continue;
        const dm::uint2 size(t.width, t.height);
        gvdb::launchKernel(commandList, m_copyKernel, gvdb::groupsFor(m_copyKernel, dm::uint3(t.width, t.height, 1u)),
                           gvdb::deviceAddress(t.renderBuffer), size, t.surface);
    }
    m_compute->flush();
    // The next graphics submission waits for the copies.
    UT_CHECK(m_interop->graphicsQueueWaitForComputeQueue(c_graphicsQueue, c_computeQueue));
}

void ScreenPresenter::blit(nvrhi::ICommandList *commandList, nvrhi::IFramebuffer *framebuffer, int id,
                           dm::box2 dstRect) {
    if (!m_pipeline || id < 0 || id >= int(m_targets.size())) return;
    Target &t = *m_targets[id];
    if (!t.bindingSet) return;

    const nvrhi::FramebufferInfoEx &fbInfo = framebuffer->getFramebufferInfo();
    float x0 = dstRect.m_mins.x * fbInfo.width, x1 = dstRect.m_maxs.x * fbInfo.width;
    float y0 = dstRect.m_mins.y * fbInfo.height, y1 = dstRect.m_maxs.y * fbInfo.height;

    nvrhi::GraphicsState state;
    state.pipeline = m_pipeline;
    state.framebuffer = framebuffer;
    state.bindings = {t.bindingSet};
    state.viewport.addViewportAndScissorRect(nvrhi::Viewport(x0, x1, y0, y1, 0.f, 1.f));
    commandList->setGraphicsState(state);

    dm::float4 screenST = {1.f, 1.f, 0.f, 0.f};
    commandList->setPushConstants(&screenST, sizeof(screenST));

    nvrhi::DrawArguments args;
    args.vertexCount = 4;
    commandList->draw(args);
}

}  // namespace SampleUtils
