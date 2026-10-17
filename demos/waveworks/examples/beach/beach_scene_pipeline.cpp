#include <cstdint>
#include "beach_scene_pipeline.h"

#include "beach_renderer.h"
#include "beach_lighting.h"
#include "main_state.h"
#include "ocean_renderer.h"
#include "sky_renderer.h"
#include "asset_utils.h"
#include "ui_renderer.h"

#include <shaders/bytecode/BeachFullscreenVS.dxbc.h>
#include <shaders/bytecode/BeachToneMapPS.dxbc.h>
#include <shaders/bytecode/BeachFullscreenVS.spirv.h>
#include <shaders/bytecode/BeachToneMapPS.spirv.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

BeachScenePipeline::BeachScenePipeline(donut::app::DeviceManager* manager, MainState* state,
                                       SkyRenderer* sky, OceanRenderer* ocean,
                                       UIRenderer* ui)
    : IExampleScenePipeline(manager),
      m_device(manager->GetDevice()),
      m_backend(manager->GetGraphicsAPI()),
      m_state(state),
      m_ownedBeach(MAKE_RC_OBJ_PTR(BeachRenderer, manager, state)),
      m_lighting(std::make_unique<BeachLighting>(manager, state)),
      m_beach(m_ownedBeach.Get()),
      m_sky(sky),
      m_ocean(ocean),
      m_ui(ui) {}

BeachScenePipeline::~BeachScenePipeline() = default;

bool BeachScenePipeline::Init(uint32_t width, uint32_t height, uint32_t sampleCount) {
    if (!m_device || !m_state || !m_beach || !m_sky || !m_ocean || !m_ui)
        return false;
    m_device->createCommandList(nvrhi::CommandListParameters(), &m_commands);
    if (!m_commands || !CreateSceneTargets(width, height, sampleCount))
        return false;

    m_ocean->SetBeachSceneTargets(m_targets.opaqueHdr, m_targets.linearDepth);
    std::fprintf(stderr, "WaveWorks: initializing beach lighting\n");
    if (!m_lighting->Init()) return false;
    std::fprintf(stderr, "WaveWorks: initializing UI\n");
    if (!m_ui->Init()) return false;
    std::fprintf(stderr, "WaveWorks: initializing sky\n");
    if (!m_sky->Init()) return false;
    std::fprintf(stderr, "WaveWorks: initializing beach terrain\n");
    if (!m_beach->Init()) return false;
    const auto& bathymetry = m_beach->GetBathymetryMetadata();
    m_ocean->SetBeachBathymetry(
        m_beach->GetBathymetryTexture(), {bathymetry.originX, bathymetry.originZ},
        {1.f / bathymetry.worldToTextureUScale, 1.f / bathymetry.worldToTextureVScale});
    std::fprintf(stderr, "WaveWorks: initializing ocean\n");
    if (!m_ocean->Init())
        return false;
    m_beach->BackBufferResized(width, height, sampleCount);
    if (!CreatePostResources())
        return false;

    for (TimingFrame& frame : m_timingRing) {
        for (nvrhi::TimerQueryHandle& query : frame.queries) {
            m_device->createTimerQuery(&query);
            if (query)
                m_device->resetTimerQuery(query);
        }
    }
    m_capturePath = ReadProcessEnvironmentVariable("WAVEWORKS_CAPTURE_FRAME");
    const std::string frameText =
        ReadProcessEnvironmentVariable("WAVEWORKS_CAPTURE_FRAME_NUMBER");
    if (!frameText.empty()) {
        char* end = nullptr;
        const char* begin = frameText.c_str();
        const unsigned long long frame = std::strtoull(begin, &end, 10);
        if (end != begin && end && *end == '\0')
            m_captureFrameNumber = frame;
    }
    m_initialized = true;
    return true;
}

bool BeachScenePipeline::CreateSceneTargets(uint32_t width, uint32_t height,
                                            uint32_t sampleCount) {
    if (width == 0 || height == 0 || sampleCount != 1)
        return false;

    SceneRenderTargets targets;
    targets.size = {width, height, sampleCount};

    nvrhi::TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.sampleCount = sampleCount;
    desc.format = nvrhi::Format::RGBA16_FLOAT;
    desc.isRenderTarget = true;
    desc.debugName = "Beach opaque HDR";
    m_device->createTexture(desc, &targets.opaqueHdr);
    desc.debugName = "Beach composite HDR";
    m_device->createTexture(desc, &targets.compositeHdr);

    desc.format = nvrhi::Format::R32_FLOAT;
    desc.debugName = "Beach terrain linear eye depth";
    desc.clearValue = nvrhi::Color(m_state->fCameraClipFar);
    desc.useClearValue = true;
    m_device->createTexture(desc, &targets.linearDepth);

    desc.format = nvrhi::Format::D24S8;
    desc.debugName = "Beach shared depth stencil";
    desc.clearValue = nvrhi::Color(1.f);
    desc.useClearValue = true;
    desc.isTypeless = true;
    m_device->createTexture(desc, &targets.depthStencil);
    if (targets.opaqueHdr && targets.compositeHdr) {
        m_device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(targets.opaqueHdr),
                                   &targets.opaqueFramebuffer);
        m_device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(targets.compositeHdr),
                                   &targets.compositeFramebuffer);
    }
    if (!targets.IsComplete())
        return false;

    m_targets = targets;
    m_state->hDepthBufferTexture = m_targets.depthStencil;
    m_targetsUsed = false;
    m_postBindings = nullptr;
    if (m_postLayout && m_linearClamp) {
        waveworks::graphics::GraphicsBindingSetDesc bindings;
        bindings.PS = {nvrhi::BindingSetItem::Texture_SRV(0, m_targets.compositeHdr,
                                                          nvrhi::Format::RGBA16_FLOAT),
                       nvrhi::BindingSetItem::Sampler(0, m_linearClamp)};
        m_postBindings = waveworks::graphics::createBindingSet(m_device, bindings, m_postLayout);
    }
    if (m_ocean)
        m_ocean->SetBeachSceneTargets(m_targets.opaqueHdr, m_targets.linearDepth);
    return true;
}

bool BeachScenePipeline::CreatePostResources() {
    const bool vk = m_backend == nvrhi::GraphicsAPI::VULKAN;
    const void* vs = vk ? static_cast<const void*>(g_BeachFullscreenVS_spirv)
                        : g_BeachFullscreenVS_dxbc;
    const size_t vsSize =
        vk ? sizeof(g_BeachFullscreenVS_spirv) : sizeof(g_BeachFullscreenVS_dxbc);
    const void* ps = vk ? static_cast<const void*>(g_BeachToneMapPS_spirv)
                        : g_BeachToneMapPS_dxbc;
    const size_t psSize =
        vk ? sizeof(g_BeachToneMapPS_spirv) : sizeof(g_BeachToneMapPS_dxbc);
    m_fullscreenVs = waveworks::graphics::createGraphicsShader(m_device, 
        nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Vertex), vs, vsSize);
    m_toneMapPs = waveworks::graphics::createGraphicsShader(m_device, nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Pixel),
                                         ps, psSize);

    nvrhi::SamplerDesc sampler;
    sampler.minFilter = true;
    sampler.magFilter = true;
    sampler.mipFilter = true;
    sampler.addressU = nvrhi::SamplerAddressMode::Clamp;
    sampler.addressV = nvrhi::SamplerAddressMode::Clamp;
    sampler.addressW = nvrhi::SamplerAddressMode::Clamp;
    m_device->createSampler(sampler, &m_linearClamp);

    waveworks::graphics::GraphicsBindingLayoutDesc layout;
    layout.PS = {nvrhi::BindingLayoutItem::Texture_SRV(0),
                 nvrhi::BindingLayoutItem::Sampler(0)};
    m_postLayout = waveworks::graphics::createBindingLayout(m_device, layout);
    if (!m_fullscreenVs || !m_toneMapPs || !m_linearClamp || !m_postLayout)
        return false;

    waveworks::graphics::GraphicsBindingSetDesc bindings;
    bindings.PS = {nvrhi::BindingSetItem::Texture_SRV(0, m_targets.compositeHdr,
                                                      nvrhi::Format::RGBA16_FLOAT),
                   nvrhi::BindingSetItem::Sampler(0, m_linearClamp)};
    m_postBindings = waveworks::graphics::createBindingSet(m_device, bindings, m_postLayout);
    return bool(m_postBindings);
}

void BeachScenePipeline::ReleaseSceneTargets() {
    m_postFramebuffers.clear();
    m_postPipeline = nullptr;
    m_postBindings = nullptr;
    if (m_ocean)
        m_ocean->SetBeachSceneTargets(nullptr, nullptr);
    if (m_state && m_state->hDepthBufferTexture == m_targets.depthStencil)
        m_state->hDepthBufferTexture = nullptr;
    m_targets = {};
    m_targetsUsed = false;
}

void BeachScenePipeline::Shutdown() {
    if (m_device)
        m_device->waitForIdle();
    ReleaseSceneTargets();
    for (TimingFrame& frame : m_timingRing) {
        frame.pending = false;
        for (nvrhi::TimerQueryHandle& query : frame.queries)
            query = nullptr;
    }
    m_postLayout = nullptr;
    m_linearClamp = nullptr;
    m_toneMapPs = nullptr;
    m_fullscreenVs = nullptr;
    m_captureStaging = nullptr;
    m_capturePath.clear();
    m_commands = nullptr;
    if (m_ocean)
        m_ocean->SetBeachBathymetry(nullptr, {}, {});
    if (m_beach)
        m_beach->Shutdown();
    if (m_lighting)
        m_lighting->Shutdown();
    m_initialized = false;
}

void BeachScenePipeline::ClearFrameTargets() {
    m_commands->open();
    const auto oldColorState = m_targetsUsed ? nvrhi::ResourceStates::ShaderResource
                                             : nvrhi::ResourceStates::Common;
    const auto oldDepthState =
        m_targetsUsed ? nvrhi::ResourceStates::DepthRead : nvrhi::ResourceStates::Common;
    m_commands->beginTrackingTextureState(m_targets.opaqueHdr, nvrhi::AllSubresources,
                                          oldColorState);
    m_commands->beginTrackingTextureState(m_targets.compositeHdr, nvrhi::AllSubresources,
                                          oldColorState);
    m_commands->beginTrackingTextureState(m_targets.linearDepth, nvrhi::AllSubresources,
                                          oldColorState);
    m_commands->beginTrackingTextureState(m_targets.depthStencil, nvrhi::AllSubresources,
                                          oldDepthState);
    m_commands->clearTextureFloat(m_targets.opaqueHdr, nvrhi::AllSubresources,
                                  nvrhi::Color(0.055f, 0.11f, 0.18f, 1.f));
    m_commands->clearTextureFloat(m_targets.compositeHdr, nvrhi::AllSubresources,
                                  nvrhi::Color(0.055f, 0.11f, 0.18f, 1.f));
    m_commands->clearTextureFloat(m_targets.linearDepth, nvrhi::AllSubresources,
                                  nvrhi::Color(m_state->fCameraClipFar));
    m_commands->clearDepthStencilTexture(m_targets.depthStencil, nvrhi::AllSubresources,
                                         true, 1.f, true, 0);
    m_commands->setTextureState(m_targets.opaqueHdr, nvrhi::AllSubresources, nvrhi::ResourceStates::RenderTarget);
    m_commands->commitBarriers();
    m_commands->setTextureState(m_targets.compositeHdr, nvrhi::AllSubresources, nvrhi::ResourceStates::RenderTarget);
    m_commands->commitBarriers();
    m_commands->setTextureState(m_targets.linearDepth, nvrhi::AllSubresources, nvrhi::ResourceStates::RenderTarget);
    m_commands->commitBarriers();
    m_commands->setTextureState(m_targets.depthStencil, nvrhi::AllSubresources, nvrhi::ResourceStates::DepthWrite);
    m_commands->commitBarriers();
    m_commands->close();
    m_device->executeCommandList(m_commands);
}

void BeachScenePipeline::CaptureOpaqueScene() {
    m_commands->open();
    m_commands->beginTrackingTextureState(m_targets.opaqueHdr, nvrhi::AllSubresources,
                                          nvrhi::ResourceStates::RenderTarget);
    m_commands->beginTrackingTextureState(m_targets.compositeHdr, nvrhi::AllSubresources,
                                          nvrhi::ResourceStates::RenderTarget);
    m_commands->setTextureState(m_targets.opaqueHdr, nvrhi::AllSubresources, nvrhi::ResourceStates::CopySource);
    m_commands->commitBarriers();
    m_commands->setTextureState(m_targets.compositeHdr, nvrhi::AllSubresources, nvrhi::ResourceStates::CopyDest);
    m_commands->commitBarriers();
    m_commands->copyTexture1(m_targets.compositeHdr, nvrhi::TextureSlice(), m_targets.opaqueHdr, nvrhi::TextureSlice());
    m_commands->setTextureState(m_targets.opaqueHdr, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    m_commands->commitBarriers();
    m_commands->setTextureState(m_targets.compositeHdr, nvrhi::AllSubresources, nvrhi::ResourceStates::RenderTarget);
    m_commands->commitBarriers();
    m_commands->close();
    m_device->executeCommandList(m_commands);
}

bool BeachScenePipeline::EnsurePostFramebuffer(nvrhi::ITexture* renderTarget,
                                               uint32_t swapChainIndex) {
    if (swapChainIndex >= m_postFramebuffers.size())
        m_postFramebuffers.resize(swapChainIndex + 1);
    if (!m_postFramebuffers[swapChainIndex])
        m_device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(renderTarget), &m_postFramebuffers[swapChainIndex]);
    if (!m_postFramebuffers[swapChainIndex])
        return false;
    if (!m_postPipeline) {
        nvrhi::GraphicsPipelineDesc pipeline;
        pipeline.VS = m_fullscreenVs;
        pipeline.PS = m_toneMapPs;
        pipeline.primType = nvrhi::PrimitiveType::TriangleList;
        pipeline.bindingLayouts = (m_postLayout).layouts;
        pipeline.renderState.depthStencilState.depthTestEnable = false;
        pipeline.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
        m_device->createGraphicsPipeline1(pipeline, m_postFramebuffers[swapChainIndex]->getFramebufferInfo().getInfo(), &m_postPipeline);
    }
    return bool(m_postPipeline);
}

void BeachScenePipeline::ToneMap(nvrhi::ITexture* renderTarget, uint32_t swapChainIndex,
                                 nvrhi::ITimerQuery* timerQuery) {
    if (!EnsurePostFramebuffer(renderTarget, swapChainIndex))
        return;
    m_commands->open();
    m_commands->beginTrackingTextureState(m_targets.compositeHdr, nvrhi::AllSubresources,
                                          nvrhi::ResourceStates::RenderTarget);
    if (!renderTarget->getDesc().keepInitialState)
        m_commands->beginTrackingTextureState(renderTarget, nvrhi::AllSubresources,
                                              nvrhi::ResourceStates::RenderTarget);
    nvrhi::GraphicsState state;
    state.pipeline = m_postPipeline;
    state.framebuffer = m_postFramebuffers[swapChainIndex];
    state.bindings = (m_postBindings).sets();
    state.viewport.viewports = {
        nvrhi::Viewport(float(m_targets.size.width), float(m_targets.size.height))};
    state.viewport.scissorRects = {
        nvrhi::Rect(m_targets.size.width, m_targets.size.height)};
    if (timerQuery)
        m_commands->beginTimerQuery(timerQuery);
    m_commands->setGraphicsState(state);
    nvrhi::DrawArguments draw;
    draw.vertexCount = 3;
    m_commands->draw(draw);
    if (timerQuery)
        m_commands->endTimerQuery(timerQuery);
    m_commands->setTextureState(m_targets.compositeHdr, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    m_commands->commitBarriers();
    m_commands->setTextureState(renderTarget, nvrhi::AllSubresources, nvrhi::ResourceStates::RenderTarget);
    m_commands->commitBarriers();
    m_commands->close();
    m_device->executeCommandList(m_commands);
}

void BeachScenePipeline::CaptureTestFrame(nvrhi::ITexture* renderTarget) {
    // This opt-in readback exists for automated backend gates only. Normal
    // example runs never allocate a staging texture or synchronize the GPU.
    if (m_captureAttempted || m_capturePath.empty() || m_frameNumber < m_captureFrameNumber)
        return;
    m_captureAttempted = true;
    std::printf(
        "Beach render GPU ms: opaque+shadow+sky %.3f, water %.3f, foam %.3f, post %.3f\n",
        m_timings.opaqueSkyMilliseconds, m_timings.waterMilliseconds,
        m_timings.foamMilliseconds, m_timings.postMilliseconds);
    std::printf("Beach SWE frame: %.3f ms CPU, physical time %.3f s, debt %.6f s, CFL %.3f\n",
        m_state->fBeachShallowMilliseconds, m_state->fBeachShallowTime,
        m_state->fBeachShallowDebt, m_state->fBeachShallowCfl);
    std::fflush(stdout);

    const nvrhi::TextureDesc sourceDesc = renderTarget->getDesc();
    if (sourceDesc.sampleCount != 1 || (sourceDesc.format != nvrhi::Format::RGBA8_UNORM &&
                                        sourceDesc.format != nvrhi::Format::BGRA8_UNORM &&
                                        sourceDesc.format != nvrhi::Format::SRGBA8_UNORM &&
                                        sourceDesc.format != nvrhi::Format::SBGRA8_UNORM))
        return;

    nvrhi::TextureDesc stagingDesc = sourceDesc;
    stagingDesc.isRenderTarget = false;
    stagingDesc.isUAV = false;
    stagingDesc.isTypeless = false;
    stagingDesc.useClearValue = false;
    stagingDesc.initialState = nvrhi::ResourceStates::Common;
    stagingDesc.keepInitialState = false;
    stagingDesc.debugName = "Beach frame readback";
    m_device->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read, &m_captureStaging);
    if (!m_captureStaging)
        return;

    // Vulkan swapchain images need not support transfer-source usage. Render
    // the same tone-mapped frame into an owned image for portable readback.
    nvrhi::TextureDesc captureDesc = stagingDesc;
    captureDesc.isRenderTarget = true;
    captureDesc.debugName = "Beach frame capture";
    nvrhi::TextureHandle capture;
    m_device->createTexture(captureDesc, &capture);
    if (!capture)
        return;
    nvrhi::FramebufferHandle framebuffer;
    m_device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(capture), &framebuffer);
    if (!framebuffer)
        return;
    m_commands->open();
    m_commands->beginTrackingTextureState(capture, nvrhi::AllSubresources,
                                          nvrhi::ResourceStates::Common);
    m_commands->beginTrackingTextureState(m_targets.compositeHdr, nvrhi::AllSubresources,
                                          nvrhi::ResourceStates::ShaderResource);
    nvrhi::GraphicsState state;
    state.pipeline = m_postPipeline;
    state.framebuffer = framebuffer;
    state.bindings = m_postBindings.sets();
    state.viewport.viewports = {
        nvrhi::Viewport(float(sourceDesc.width), float(sourceDesc.height))};
    state.viewport.scissorRects = {nvrhi::Rect(sourceDesc.width, sourceDesc.height)};
    m_commands->setGraphicsState(state);
    nvrhi::DrawArguments draw;
    draw.vertexCount = 3;
    m_commands->draw(draw);
    m_commands->copyTexture2(m_captureStaging, nvrhi::TextureSlice(), capture, nvrhi::TextureSlice());
    m_commands->close();
    m_device->executeCommandList(m_commands);
    m_device->waitForIdle();

    size_t rowPitch = 0;
    const auto* pixels = static_cast<const uint8_t*>(m_device->mapStagingTexture(
        m_captureStaging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, rowPitch));
    if (!pixels)
        return;

    FILE* output = nullptr;
#ifdef WIN32
    fopen_s(&output, m_capturePath.c_str(), "wb");
#else
    output = std::fopen(m_capturePath.c_str(), "wb");
#endif
    if (output) {
        std::fprintf(output, "P6\n%u %u\n255\n", sourceDesc.width, sourceDesc.height);
        std::vector<uint8_t> rgb(size_t(sourceDesc.width) * 3u);
        const bool blueFirst = sourceDesc.format == nvrhi::Format::BGRA8_UNORM ||
                               sourceDesc.format == nvrhi::Format::SBGRA8_UNORM;
        for (uint32_t y = 0; y < sourceDesc.height; ++y) {
            const uint8_t* source = pixels + size_t(y) * rowPitch;
            for (uint32_t x = 0; x < sourceDesc.width; ++x) {
                rgb[size_t(x) * 3u] = source[size_t(x) * 4u + (blueFirst ? 2u : 0u)];
                rgb[size_t(x) * 3u + 1u] = source[size_t(x) * 4u + 1u];
                rgb[size_t(x) * 3u + 2u] = source[size_t(x) * 4u + (blueFirst ? 0u : 2u)];
            }
            std::fwrite(rgb.data(), 1, rgb.size(), output);
        }
        std::fclose(output);
    }
    m_device->unmapStagingTexture(m_captureStaging);
}

void BeachScenePipeline::ResolveTimingFrame(TimingFrame& frame) {
    if (!frame.pending)
        return;
    for (const nvrhi::TimerQueryHandle& query : frame.queries)
        if (query && !m_device->pollTimerQuery(query))
            return;
    float* values[] = {&m_timings.opaqueSkyMilliseconds, &m_timings.waterMilliseconds,
                       &m_timings.foamMilliseconds, &m_timings.postMilliseconds};
    for (uint32_t i = 0; i < TimingStageCount; ++i) {
        if (!frame.queries[i])
            continue;
        const float milliseconds = m_device->getTimerQueryTime(frame.queries[i]) * 1000.f;
        if (milliseconds >= 0.f && milliseconds < 10000.f)
            *values[i] =
                *values[i] == 0.f ? milliseconds : *values[i] * 0.9f + milliseconds * 0.1f;
        m_device->resetTimerQuery(frame.queries[i]);
    }
    frame.pending = false;
}

BeachScenePipeline::TimingFrame* BeachScenePipeline::BeginTimingFrame() {
    TimingFrame& frame = m_timingRing[m_frameNumber % m_timingRing.size()];
    ResolveTimingFrame(frame);
    return frame.pending ? nullptr : &frame;
}

void BeachScenePipeline::Render(nvrhi::IFramebuffer* framebuffer) {
    auto* renderTarget = framebuffer->getDesc().colorAttachments[0].texture;
    const uint32_t swapChainIndex = GetDeviceManager()->GetCurrentBackBufferIndex();
    if (!m_initialized || !renderTarget)
        return;
    const nvrhi::TextureDesc targetDesc = renderTarget->getDesc();
    if (!m_targets.size.Matches(targetDesc.width, targetDesc.height,
                                targetDesc.sampleCount)) {
        m_device->waitForIdle();
        m_beach->BackBufferResizing();
        m_sky->BackBufferResizing();
        m_ocean->BackBufferResizing();
        m_ui->BackBufferResizing();
        ReleaseSceneTargets();
        if (!CreateSceneTargets(targetDesc.width, targetDesc.height,
                                targetDesc.sampleCount))
            return;
        m_beach->BackBufferResized(targetDesc.width, targetDesc.height,
                                   targetDesc.sampleCount);
        m_sky->BackBufferResized(targetDesc.width, targetDesc.height,
                                 targetDesc.sampleCount);
        m_ocean->BackBufferResized(targetDesc.width, targetDesc.height,
                                   targetDesc.sampleCount);
        m_ui->BackBufferResized(targetDesc.width, targetDesc.height,
                                targetDesc.sampleCount);
    }

    // Resize callbacks may update the projection. The aerial volume and every
    // surface consuming it must use that same, current camera frustum.
    if (!m_lighting->Update())
        return;

    TimingFrame* timing = BeginTimingFrame();
    nvrhi::ITimerQuery* opaqueQuery = timing ? timing->queries[OpaqueSky] : nullptr;
    nvrhi::ITimerQuery* waterQuery = timing ? timing->queries[Water] : nullptr;
    nvrhi::ITimerQuery* foamQuery = timing ? timing->queries[Foam] : nullptr;
    nvrhi::ITimerQuery* postQuery = timing ? timing->queries[Post] : nullptr;

    ClearFrameTargets();
    // Bracket both opaque contributors with one timestamp pair. The marker
    // command lists are submitted on the same graphics queue as the component
    // command lists, so the elapsed value includes terrain and sky in their
    // actual execution order without forcing a readback or a GPU wait.
    if (opaqueQuery) {
        m_commands->open();
        m_commands->beginTimerQuery(opaqueQuery);
        m_commands->close();
        m_device->executeCommandList(m_commands);
    }
    m_beach->RenderOpaque(m_targets.opaqueHdr, m_targets.linearDepth,
                          m_targets.depthStencil);
    m_sky->Render(m_targets.opaqueFramebuffer);
    if (opaqueQuery) {
        m_commands->open();
        m_commands->endTimerQuery(opaqueQuery);
        m_commands->close();
        m_device->executeCommandList(m_commands);
    }
    CaptureOpaqueScene();
    m_ocean->SetBeachPassTimerQueries(waterQuery, foamQuery);
    m_ocean->Render(m_targets.compositeFramebuffer);
    m_ocean->SetBeachPassTimerQueries(nullptr, nullptr);
    ToneMap(renderTarget, swapChainIndex, postQuery);
    CaptureTestFrame(renderTarget);
    m_ui->Render(framebuffer);
    if (timing)
        timing->pending = true;
    m_targetsUsed = true;
    ++m_frameNumber;
}

void BeachScenePipeline::Animate(float elapsedTimeSeconds) {
    m_ui->Animate(elapsedTimeSeconds);
    m_sky->Animate(elapsedTimeSeconds);
    m_beach->Animate(elapsedTimeSeconds);
    if (!m_state->bBeachShallowSimulation && m_state->bBeachSurfForcing && m_state->bUpdateLocalWaves &&
        m_state->bSimulateWater && !m_state->bNeedToResetLocalWavesSimulation &&
        !m_state->bNeedToUpdateLocalWavesSimulationProperties) {
        BeachSurfSimulationSettings forcing;
        forcing.domainGridSize =
            m_state->OceanLocalSimulationSettings.simulation_domain_grid_size;
        forcing.domainWorldspaceSize =
            m_state->OceanLocalSimulationSettings.simulation_domain_worldspace_size;
        forcing.domainCenterX =
            m_state->OceanLocalSimulationSettings.simulation_domain_center.x;
        forcing.domainCenterZ =
            m_state->OceanLocalSimulationSettings.simulation_domain_center.y;
        std::vector<gfsdk_float4> disturbances;
        if (BuildBeachSurfDisturbances(forcing, elapsedTimeSeconds,
                                       m_state->dSimulationTime, disturbances) &&
            !disturbances.empty())
            GFSDK_WaveWorks_Local_Waves_Simulation_AddDisturbances(
                m_state->hOceanLocalSimulation, disturbances.data(),
                uint32_t(disturbances.size()));
    }
    m_ocean->Animate(elapsedTimeSeconds);
}

void BeachScenePipeline::BackBufferResizing() {
    if (!m_initialized)
        return;
    m_device->waitForIdle();
    m_beach->BackBufferResizing();
    m_sky->BackBufferResizing();
    m_ocean->BackBufferResizing();
    m_ui->BackBufferResizing();
    ReleaseSceneTargets();
}

void BeachScenePipeline::BackBufferResized(uint32_t width, uint32_t height,
                                           uint32_t sampleCount) {
    // DeviceManager can issue BackBufferResized without a preceding resize
    // notification on some backend recreation paths. Avoid leaking the old
    // scene targets or creating duplicate resources in that case.
    if (m_targets.IsComplete())
        ReleaseSceneTargets();
    if (!CreateSceneTargets(width, height, sampleCount))
        return;
    m_beach->BackBufferResized(width, height, sampleCount);
    m_sky->BackBufferResized(width, height, sampleCount);
    m_ocean->BackBufferResized(width, height, sampleCount);
    m_ui->BackBufferResized(width, height, sampleCount);
}

bool BeachScenePipeline::KeyboardUpdate(int key, int scancode, int action, int mods) {
    if (m_ui->KeyboardUpdate(key, scancode, action, mods))
        return true;
    return m_ocean->KeyboardUpdate(key, scancode, action, mods);
}

bool BeachScenePipeline::MousePosUpdate(double xpos, double ypos) {
    if (m_ui->MousePosUpdate(xpos, ypos))
        return true;
    return m_ocean->MousePosUpdate(xpos, ypos);
}

bool BeachScenePipeline::MouseScrollUpdate(double xoffset, double yoffset) {
    if (m_ui->MouseScrollUpdate(xoffset, yoffset))
        return true;
    return m_ocean->MouseScrollUpdate(xoffset, yoffset);
}

bool BeachScenePipeline::MouseButtonUpdate(int button, int action, int mods) {
    if (m_ui->MouseButtonUpdate(button, action, mods))
        return true;
    return m_ocean->MouseButtonUpdate(button, action, mods);
}

bool BeachScenePipeline::KeyboardCharInput(unsigned int character, int mods) {
    return m_ui->KeyboardCharInput(character, mods);
}
