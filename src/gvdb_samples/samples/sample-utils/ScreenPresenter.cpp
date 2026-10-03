// CUDA render buffers -> shared nvrhi textures -> swap chain.
//
// Synchronisation follows the previous port (DepthMapPass): one timeline per
// graphics queue inside IGPAndNVRHIInteropDevice; the gp queue waits for the
// last graphics signal before touching the shared textures and signals when it
// is done, and the graphics queue does the same in the other direction. On
// D3D11 the shared textures additionally carry keyed mutexes (acquired with an
// alternating key); on D3D12 / Vulkan the keyed-mutex calls are no-ops.
#include "ScreenPresenter.h"
#include "SampleTypes.h"
#include <donut/core/log.h>
#include <nvrhi/utils.h>
#include <iterator>

namespace SampleUtils {

using namespace donut;

struct ScreenPresenter::Target {
    uint32_t width = 0;
    uint32_t height = 0;
    nvrhi::AutoPtr<gp::IBuffer> renderBuffer;
    nvrhi::TextureHandle texture;
    nvrhi::AutoPtr<gp::ITexture> textureGP;
    nvrhi::BindingSetHandle bindingSet;
    uint32_t mutexKey = 0;
};

ScreenPresenter::ScreenPresenter(nvrhi::IDevice *device, IGPAndNVRHIInteropDevice *interop, gp::IDevice *gpDevice,
                                 gp::IDeviceQueue *gpQueue, engine::ShaderFactory *shaderFactory,
                                 const nvrhi::FramebufferInfoEx &fbInfo)
    : m_device(device), m_interop(interop), m_gpDevice(gpDevice), m_gpQueue(gpQueue) {
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

ScreenPresenter::~ScreenPresenter() {}

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

    // Make sure neither queue still uses the old resources.
    syncQueue(m_gpDevice, m_gpQueue);
    m_device->waitForIdle();

    t.width = width;
    t.height = height;
    t.bindingSet = nullptr;
    t.textureGP = nullptr;
    t.texture = nullptr;
    t.renderBuffer = nullptr;

    gp::BufferDesc bufDesc;
    bufDesc.byteSize = size_t(width) * height * 4;
    UT_V_GP(m_gpDevice->createBuffer(bufDesc, &t.renderBuffer));
    UT_V_GP(m_gpQueue->clearBufferUint(t.renderBuffer, 0u));

    nvrhi::TextureDesc texDesc;
    texDesc.dimension = nvrhi::TextureDimension::Texture2D;
    texDesc.width = width;
    texDesc.height = height;
    texDesc.format = nvrhi::Format::RGBA8_UNORM;
    texDesc.isShaderResource = true;
    texDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    texDesc.keepInitialState = true;
    texDesc.debugName = "ScreenPresenter target";
    texDesc.sharedResourceFlags = interopSharedFlags(m_device);
    m_device->createTexture(texDesc, &t.texture);
    if (!t.texture) {
        log::error("ScreenPresenter: cannot create the shared texture %ux%u", width, height);
        return;
    }
    UT_V_GP(m_interop->createGPTexture(t.texture, &t.textureGP));
    t.mutexKey = 0;

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
gp::IBuffer *ScreenPresenter::getRenderBuffer(int id) const {
    return (id >= 0 && id < int(m_targets.size())) ? m_targets[id]->renderBuffer.Get() : nullptr;
}
nvrhi::ITexture *ScreenPresenter::getTexture(int id) const {
    return (id >= 0 && id < int(m_targets.size())) ? m_targets[id]->texture.Get() : nullptr;
}

void ScreenPresenter::beginGPFrame() {
    // The gp queue waits until the graphics queue has finished reading the
    // textures of the previous frame.
    UT_V_GP(m_interop->commitGPQueueWait(m_gpQueue, nvrhi::CommandQueue::Graphics));
}

void ScreenPresenter::endGPFrame() {
    std::vector<gp::GraphicsInteropKeyedMutexWaitParams> waits;
    std::vector<gp::GraphicsInteropKeyedMutexSignalParams> signals;
    for (auto &tp : m_targets) {
        Target &t = *tp;
        if (!t.textureGP) continue;
        waits.push_back({t.textureGP.Get(), t.mutexKey, ~0u});
        signals.push_back({t.textureGP.Get(), t.mutexKey + 1});
    }
    if (!waits.empty()) UT_V_GP(m_interop->acquireGPResourceKeyedMutexes(m_gpQueue, waits.data(), uint32_t(waits.size())));

    for (auto &tp : m_targets) {
        Target &t = *tp;
        if (!t.textureGP || !t.renderBuffer) continue;
        gp::TextureCopyLocation src = {};
        src.type = gp::TextureCopyType::PlacedFootprint;
        src.resource = t.renderBuffer;
        src.placeFootprint.offset = 0;
        src.placeFootprint.format = gp::Format::RGBA8_UNORM;
        src.placeFootprint.width = t.width;
        src.placeFootprint.height = t.height;
        src.placeFootprint.depth = 1;
        src.placeFootprint.rowPitch = t.width * 4;
        gp::TextureCopyLocation dst = {};
        dst.type = gp::TextureCopyType::SubresourceIndex;
        dst.resource = t.textureGP;
        dst.subresourceIndex = 0;
        UT_V_GP(m_gpQueue->copyTextureRegion(dst, 0, 0, 0, src, nullptr));
    }

    if (!signals.empty())
        UT_V_GP(m_interop->releaseGPResourceKeyedMutexes(m_gpQueue, signals.data(), uint32_t(signals.size())));
    for (auto &tp : m_targets) tp->mutexKey += 1;   // graphics acquires with the released key

    UT_V_GP(m_interop->commitGPQueueSignal(m_gpQueue, nvrhi::CommandQueue::Graphics));
}

void ScreenPresenter::beginGraphics(nvrhi::ICommandList *commandList) {
    UT_V_GP(m_interop->commitNVRHIQueueWait(nvrhi::CommandQueue::Graphics));
    for (auto &tp : m_targets) {
        if (!tp->texture) continue;
        UT_V_GP(m_interop->acquireNVRHITextureKeyedMutex(tp->texture, tp->mutexKey, ~0u));
    }
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

void ScreenPresenter::endGraphics(nvrhi::ICommandList *commandList) {
    for (auto &tp : m_targets) {
        if (!tp->texture) continue;
        UT_V_GP(m_interop->releaseNVRHITextureKeyedMutex(tp->texture, tp->mutexKey + 1));
        tp->mutexKey += 1;   // the gp queue acquires with this key next frame
    }
}

void ScreenPresenter::endGraphicsFrame() {
    UT_V_GP(m_interop->commitNVRHIQueueSignal(nvrhi::CommandQueue::Graphics));
}

}  // namespace SampleUtils
