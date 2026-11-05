/*
* Copyright (c) 2026, ethereal contributors. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/

#include <donut/render/BlitPass.h>
#include <donut/render/BloomPass.h>
#include <donut/render/CascadedShadowMap.h>
#include <donut/render/DeferredLightingPass.h>
#include <donut/render/DepthPass.h>
#include <donut/render/DrawStrategy.h>
#include <donut/render/GBuffer.h>
#include <donut/render/GBufferFillPass.h>
#include <donut/render/RenderGraph.h>
#include <donut/render/SkyPass.h>
#include <donut/render/SsaoPass.h>
#include <donut/render/TemporalAntiAliasingPass.h>
#include <donut/render/ToneMappingPasses.h>
#include <donut/app/ApplicationBase.h>
#include <donut/app/Camera.h>
#include <donut/app/DeviceManager.h>
#include <donut/app/RenderGraphLayer.h>
#include <donut/app/RenderGraphVisualizer.h>
#include <donut/engine/BindingCache.h>
#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/FramebufferFactory.h>
#include <donut/engine/Scene.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/TextureCache.h>
#include <donut/core/log.h>
#include <donut/core/math/math.h>
#include <donut/core/vfs/VFS.h>
#include <nvrhi/utils.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

using namespace donut;
using namespace donut::math;

#include "../sample_harness/SampleHarness.h"
#include "../sample_harness/GraphRecording.h"

static const char* g_WindowTitle = "Donut Example: Async Compute (render graph)";

// The bloom/SSAO graph of the render-graph plan (aggregate docs/plans/2026-11-05-render-graph.md, step P2.3): Sponza,
// deferred, with SSAO on the compute queue beside the shadow cascades on the graphics queue.
//   1. GBufferFill: the G-buffer and depth, transients;
//   2. SSAO from depth and normals, PassFlags::AsyncCompute (-sync: the graphics queue);
//   3. ShadowDepth: the sun's four cascades into the CascadedShadowMap's texture, an import. It needs nothing SSAO
//      writes, so it overlaps SSAO. Each cascade has its own DepthPass, pass context and draw strategy, so that a
//      parallel recorder can record the cascades on separate threads (a pass object's uses share one task, 10.1);
//   4. DeferredLighting, with the ambient occlusion and the shadow map; 5. Sky;
//   6. TAA: motion vectors from depth, then the resolve against its history pair;
//   7. Bloom on the resolved colour; 8. ToneMapping, with its histogram and exposure; 9. a blit to the back buffer.
// After DeferredLighting the G-buffer is dead, and the motion vectors, the resolved colour, the bloom chain and the
// tone-mapped colour reuse its memory: the graph's reusable transients (10.3).
//
// -immediate records the same frame without a graph, on one command list with nvrhi's automatic barriers and SSAO on
// the graphics queue: the GPU-time reference ("the original", 10.3).
//
// Beside the harness's options:
//     -sync            SSAO on the graphics queue
//     -nosplit         BackendProfile::splitBarriers = false
//     -placed          CompileOptions::placedResources: the Placed tier where the profile has it (D3D12)
//     -asyncgain N     LatticePolicy::asyncOverlapGain = N (default 16): a large value keeps SSAO on the compute queue
//                      where the default demotes it (Vulkan with a separate compute family, 6.8)
//     -nocache, -notimers, -graphstats F   as deferred_shading_rdg; the statistics add the plan's queues, barriers
//                      and memory
//     -passtimes F     per stage, the median GPU time of the frames after the warm-up, as JSON: the graph's per-pass
//                      timers (not with -notimers), or timer queries around the immediate frame's stages
//     -size WxH        the back buffer's size (default 1280x720)
//     -parallel N, -granularity G   parallel recording (sample_harness/GraphRecording.h, step P2.4)

// The stages of -passtimes: a graph pass counts towards the stage its name starts with, up to a '/'.
static const char* const c_Stages[] = { "GBufferFill", "SSAO", "ShadowDepth", "DeferredLighting", "Sky", "MotionVectors",
                                        "TemporalAA", "Bloom", "ToneMapping", "Blit" };
constexpr uint32_t c_StageCount = uint32_t(std::size(c_Stages));
constexpr uint32_t c_TimedFrames = 4;   // immediate timer queries in flight
constexpr uint32_t c_Cascades = 4;

class AsyncComputeGraph;

// The layer that records each frame; the sample, an ApplicationBase for its scene loading, renders through it.
class FrameGraphLayer : public app::RenderGraphLayer
{
    NVRHI_INHERIT_INTERFACE_TABLE()
public:
    FrameGraphLayer(app::DeviceManager* deviceManager, AsyncComputeGraph* sample)
        : RenderGraphLayer(deviceManager), m_Sample(sample)
    { }
    void BuildGraph(render::IRenderGraph* graph, render::FrameData& frameData, render::ResourceId backBuffer) override;
private:
    AsyncComputeGraph* m_Sample;
};

// The immediate frame's targets and the passes bound to them.
struct ImmediateFrame
{
    uint2 size = uint2::zero();
    render::GBufferRenderTargets gbuffer;
    nvrhi::TextureHandle ambientOcclusion;
    nvrhi::TextureHandle hdrColor;
    nvrhi::TextureHandle motionVectors;
    nvrhi::TextureHandle resolvedColor;
    nvrhi::TextureHandle feedback1;
    nvrhi::TextureHandle feedback2;
    nvrhi::TextureHandle ldrColor;
    nvrhi::AutoPtr<engine::FramebufferFactory> hdrFramebuffer;        // colour and depth: the sky
    nvrhi::AutoPtr<engine::FramebufferFactory> resolvedFramebuffer;   // bloom
    nvrhi::AutoPtr<engine::FramebufferFactory> ldrFramebuffer;        // tone mapping
    nvrhi::AutoPtr<engine::FramebufferFactory> shadowFramebuffer;
    nvrhi::AutoPtr<render::SsaoPass> ssao;
    nvrhi::AutoPtr<render::SkyPass> sky;
    nvrhi::AutoPtr<render::TemporalAntiAliasingPass> temporal;
    nvrhi::AutoPtr<render::BloomPass> bloom;
    nvrhi::AutoPtr<render::ToneMappingPass> toneMapping;
    bool feedbackValid = false;
    bool exposureValid = false;
};

class AsyncComputeGraph : public app::ApplicationBase
{
    NVRHI_INHERIT_INTERFACE_TABLE()
private:
    nvrhi::AutoPtr<vfs::RootFileSystem> m_RootFS;
    nvrhi::AutoPtr<engine::ShaderFactory> m_ShaderFactory;
    nvrhi::AutoPtr<engine::Scene> m_Scene;
    std::unique_ptr<engine::BindingCache> m_BindingCache;
    nvrhi::CommandListHandle m_CommandList;
    nvrhi::AutoPtr<FrameGraphLayer> m_Graph;

    app::FirstPersonCamera m_Camera;
    engine::PlanarView m_View;
    engine::PlanarView m_ViewPrevious;
    bool m_PreviousViewValid = false;
    nvrhi::AutoPtr<engine::DirectionalLight> m_SunLight;
    nvrhi::AutoPtr<render::CascadedShadowMap> m_ShadowMap;
    nvrhi::AutoPtr<render::InstancedOpaqueDrawStrategy> m_GBufferStrategy;
    nvrhi::AutoPtr<render::InstancedOpaqueDrawStrategy> m_ShadowStrategies[c_Cascades];
    nvrhi::Format m_DepthFormat = nvrhi::Format::UNKNOWN;
    float m_FrameTime = 0.f;

    // The passes. The geometry and lighting passes serve both modes; the others have an immediate construction,
    // bound to the immediate frame's targets, and a graph construction.
    nvrhi::AutoPtr<render::GBufferFillPass> m_GBufferPass;
    nvrhi::AutoPtr<render::DepthPass> m_ShadowDepthPasses[c_Cascades];   // one per cascade; the immediate frame uses the first
    nvrhi::AutoPtr<render::DeferredLightingPass> m_LightingPass;
    render::GBufferFillPass::Context m_GBufferContext;
    render::DepthPass::Context m_ShadowContexts[c_Cascades];
    nvrhi::AutoPtr<render::SsaoPass> m_SsaoPass;
    nvrhi::AutoPtr<render::SkyPass> m_SkyPass;
    nvrhi::AutoPtr<render::TemporalAntiAliasingPass> m_TemporalPass;
    nvrhi::AutoPtr<render::BloomPass> m_BloomPass;
    nvrhi::AutoPtr<render::ToneMappingPass> m_ToneMappingPass;
    nvrhi::AutoPtr<render::BlitPass> m_BlitPass;
    std::unique_ptr<ImmediateFrame> m_Immediate;

    render::SsaoParameters m_SsaoParams;
    render::SkyParameters m_SkyParams;
    render::TemporalAntiAliasingParameters m_TemporalParams;
    render::ToneMappingParameters m_ToneMappingParams;
    render::TemporalAntiAliasingPass::CreateParameters m_TemporalCreateParams;
    render::ToneMappingPass::CreateParameters m_ToneMappingCreateParams;
    float m_BloomSigma = 32.f;
    float m_BloomAlpha = 0.05f;

    // Switches.
    bool m_ImmediateMode = false;
    bool m_AsyncSsao = true;

    // Measurements.
    uint32_t m_Warmup = 100;
    uint32_t m_Frame = 0;
    std::vector<double> m_FrameCostMs;
    std::vector<double> m_ExecuteMs;
    std::vector<double> m_RecordMs;
    std::vector<uint32_t> m_Barriers;
    render::RunStatistics m_LastRun;
    sample_harness::GraphRecording m_Recording;
    uint64_t m_ObjectsAtWarmup = 0;    // RegistryStatistics::objectsCreated when the warm-up ends
    bool m_PassTimes = false;
    std::vector<double> m_StageMs[c_StageCount + 1];   // the last: the frame's sum
    uint64_t m_LastTimedFrame = ~0ull;
    uint32_t m_ImmediateFrames = 0;
    nvrhi::TimerQueryHandle m_StageTimers[c_TimedFrames][c_StageCount];

    static int StageOf(const char* name)
    {
        for (uint32_t i = 0; i < c_StageCount; ++i)
        {
            const size_t length = std::strlen(c_Stages[i]);
            if (!std::strncmp(name, c_Stages[i], length) && (name[length] == 0 || name[length] == '/'))
                return int(i);
        }
        return -1;
    }

    void AddStageTimes(const double (&stages)[c_StageCount])
    {
        double sum = 0.0;
        for (uint32_t i = 0; i < c_StageCount; ++i)
        {
            m_StageMs[i].push_back(stages[i]);
            sum += stages[i];
        }
        m_StageMs[c_StageCount].push_back(sum);
    }

    static double Median(std::vector<double> samples)
    {
        if (samples.empty())
            return 0.0;
        std::sort(samples.begin(), samples.end());
        return samples[samples.size() / 2];
    }

public:
    using ApplicationBase::ApplicationBase;

    bool Init(int argc, const char* const* argv, uint32_t warmup)
    {
        m_Graph = MAKE_RC_OBJ_PTR(FrameGraphLayer, GetDeviceManager(), this);
        m_Warmup = warmup;
        bool timers = true;
        for (int i = 1; i < argc; ++i)
        {
            if (!std::strcmp(argv[i], "-immediate"))
                m_ImmediateMode = true;
            else if (!std::strcmp(argv[i], "-sync"))
                m_AsyncSsao = false;
            else if (!std::strcmp(argv[i], "-nosplit"))
                m_Graph->GetProfile().splitBarriers = false;
            else if (!std::strcmp(argv[i], "-placed"))
                m_Graph->GetCompileOptions().placedResources = true;
            else if (!std::strcmp(argv[i], "-asyncgain") && i + 1 < argc)
                m_Graph->GetProfile().lattice.asyncOverlapGain = uint16_t(std::atoi(argv[++i]));
            else if (!std::strcmp(argv[i], "-nocache"))
                m_Graph->GetCompileOptions().maxCachedPlans = 0;
            else if (!std::strcmp(argv[i], "-notimers"))
                timers = false;
            else if (!std::strcmp(argv[i], "-passtimes"))
                m_PassTimes = true;
        }
        m_Graph->GetExecuteDesc().timers = timers;
        m_Recording.Configure(argc, argv, m_Graph);

        std::filesystem::path sceneFileName = app::GetDirectoryWithExecutable().parent_path() / "media/glTF-Sample-Assets/Models/Sponza/glTF/Sponza.gltf";
        std::filesystem::path frameworkShaderPath = app::GetDirectoryWithExecutable() / "shaders/framework" / app::GetShaderTypeName(GetDevice()->getGraphicsAPI());

        m_RootFS = MAKE_RC_OBJ_PTR(vfs::RootFileSystem);
        m_RootFS->mount("/shaders/donut", frameworkShaderPath);
        m_ShaderFactory = MAKE_RC_OBJ_PTR(engine::ShaderFactory, GetDevice(), m_RootFS, "/shaders");
        m_CommonPasses = MAKE_RC_OBJ_PTR(engine::CommonRenderPasses, GetDevice(), m_ShaderFactory);
        m_BindingCache = std::make_unique<engine::BindingCache>(GetDevice());

        auto nativeFS = MAKE_RC_OBJ_PTR(vfs::NativeFileSystem);
        m_TextureCache = MAKE_RC_OBJ_PTR(engine::TextureCache, GetDevice(), nativeFS, nullptr);

        SetAsynchronousLoadingEnabled(false);
        BeginLoadingScene(nativeFS, sceneFileName);
        if (!m_Scene)
            return false;

        m_SunLight = MAKE_RC_OBJ_PTR(engine::DirectionalLight);
        m_Scene->GetSceneGraph()->AttachLeafNode(m_Scene->GetSceneGraph()->GetRootNode(), m_SunLight);
        m_SunLight->SetDirection(double3(0.1, -1.0, 0.15));
        m_SunLight->SetName("Sun");
        m_SunLight->angularSize = 0.53f;
        m_SunLight->irradiance = 2.f;
        m_Scene->FinishedLoading(GetFrameIndex());

        m_ShadowMap = MAKE_RC_OBJ_PTR(render::CascadedShadowMap, GetDevice(), 2048, int(c_Cascades), 0, nvrhi::Format::D24S8);
        m_SunLight->shadowMap = m_ShadowMap;

        m_Camera.LookAt(float3(-8.f, 1.8f, 0.f), float3(1.f, 2.5f, 0.f));
        m_Camera.SetMoveSpeed(3.f);

        m_GBufferStrategy = MAKE_RC_OBJ_PTR(render::InstancedOpaqueDrawStrategy);
        for (nvrhi::AutoPtr<render::InstancedOpaqueDrawStrategy>& strategy : m_ShadowStrategies)
            strategy = MAKE_RC_OBJ_PTR(render::InstancedOpaqueDrawStrategy);

        // The depth format GBufferRenderTargets chooses.
        const nvrhi::Format depthFormats[] = { nvrhi::Format::D24S8, nvrhi::Format::D32S8, nvrhi::Format::D32, nvrhi::Format::D16 };
        m_DepthFormat = nvrhi::utils::ChooseFormat(GetDevice(),
            nvrhi::FormatSupport::Texture | nvrhi::FormatSupport::DepthStencil | nvrhi::FormatSupport::ShaderLoad,
            depthFormats, std::size(depthFormats));

        m_GBufferPass = MAKE_RC_OBJ_PTR(render::GBufferFillPass, GetDevice(), m_CommonPasses);
        render::GBufferFillPass::CreateParameters gbufferParams;
        gbufferParams.enableMotionVectors = false;
        m_GBufferPass->Init(*m_ShaderFactory, gbufferParams);
        render::DepthPass::CreateParameters shadowParams;
        shadowParams.depthBias = 100;
        shadowParams.slopeScaledDepthBias = 4.f;
        for (nvrhi::AutoPtr<render::DepthPass>& pass : m_ShadowDepthPasses)
        {
            pass = MAKE_RC_OBJ_PTR(render::DepthPass, GetDevice(), m_CommonPasses);
            pass->Init(*m_ShaderFactory, shadowParams);
        }
        m_LightingPass = MAKE_RC_OBJ_PTR(render::DeferredLightingPass, GetDevice(), m_CommonPasses);
        m_LightingPass->Init(m_ShaderFactory);

        m_TemporalCreateParams.useCatmullRomFilter = true;
        if (!m_ImmediateMode)
        {
            m_SsaoPass = MAKE_RC_OBJ_PTR(render::SsaoPass, GetDevice(), m_ShaderFactory, m_CommonPasses);
            m_SkyPass = MAKE_RC_OBJ_PTR(render::SkyPass, GetDevice(), m_ShaderFactory, m_CommonPasses);
            m_TemporalPass = MAKE_RC_OBJ_PTR(render::TemporalAntiAliasingPass, GetDevice(), m_ShaderFactory, m_CommonPasses,
                                             m_TemporalCreateParams);
            m_BloomPass = MAKE_RC_OBJ_PTR(render::BloomPass, GetDevice(), m_ShaderFactory, m_CommonPasses);
            m_ToneMappingPass = MAKE_RC_OBJ_PTR(render::ToneMappingPass, GetDevice(), m_ShaderFactory, m_CommonPasses,
                                                m_ToneMappingCreateParams);
            m_BlitPass = MAKE_RC_OBJ_PTR(render::BlitPass, GetDevice(), m_CommonPasses);
        }

        GetDevice()->createCommandList(nvrhi::CommandListParameters(), &m_CommandList);
        GetDevice()->waitForIdle();
        return true;
    }

    FrameGraphLayer* GetGraphLayer() const { return m_Graph; }
    engine::ShaderFactory* GetShaderFactory() const { return m_ShaderFactory; }
    bool IsImmediate() const { return m_ImmediateMode; }

    bool LoadScene(vfs::IFileSystem* fs, const std::filesystem::path& sceneFileName) override
    {
        nvrhi::AutoPtr<engine::Scene> scene = MAKE_RC_OBJ_PTR(engine::Scene, GetDevice(),
            m_ShaderFactory, fs, m_TextureCache, nullptr, nullptr);
        if (scene->Load(sceneFileName))
        {
            m_Scene = std::move(scene);
            return true;
        }
        return false;
    }

    bool KeyboardUpdate(int key, int scancode, int action, int mods) override
    {
        m_Camera.KeyboardUpdate(key, scancode, action, mods);
        return true;
    }

    bool MousePosUpdate(double xpos, double ypos) override
    {
        m_Camera.MousePosUpdate(xpos, ypos);
        return true;
    }

    bool MouseButtonUpdate(int button, int action, int mods) override
    {
        m_Camera.MouseButtonUpdate(button, action, mods);
        return true;
    }

    void Animate(float fElapsedTimeSeconds) override
    {
        fElapsedTimeSeconds = sample_harness::Step(fElapsedTimeSeconds);
        m_FrameTime = fElapsedTimeSeconds;
        m_Camera.Animate(fElapsedTimeSeconds);
        GetDeviceManager()->SetInformativeWindowTitle(g_WindowTitle);
    }

    void BackBufferResizing() override
    {
        m_BindingCache->Clear();
        m_Immediate = nullptr;
        m_PreviousViewValid = false;
        if (m_Graph)
            m_Graph->BackBufferResizing();
    }

    // The camera's view, the previous frame's (this one's on the first frame: no motion), and the cascades.
    void SetupViews(uint32_t width, uint32_t height)
    {
        const nvrhi::Viewport viewport = nvrhi::Viewport(float(width), float(height));
        m_View.SetViewport(viewport);
        m_View.SetMatrices(m_Camera.GetWorldToViewMatrix(), perspProjD3DStyleReverse(dm::PI_f * 0.25f, viewport.width() / viewport.height(), 0.1f));
        m_View.UpdateCache();
        if (!m_PreviousViewValid)
        {
            m_ViewPrevious.SetViewport(viewport);
            m_ViewPrevious.SetMatrices(m_View.GetViewMatrix(), m_View.GetProjectionMatrix(false));
            m_ViewPrevious.UpdateCache();
        }
        m_ShadowMap->SetupForPlanarView(*m_SunLight, m_View.GetViewFrustum(), 40.f, 40.f, 40.f);
    }

    void EndFrame()
    {
        m_ViewPrevious.SetViewport(m_View.GetViewport());
        m_ViewPrevious.SetMatrices(m_View.GetViewMatrix(), m_View.GetProjectionMatrix(false));
        m_ViewPrevious.SetPixelOffset(m_View.GetPixelOffset());
        m_ViewPrevious.UpdateCache();
        m_PreviousViewValid = true;
    }

    void Render(nvrhi::IFramebuffer* framebuffer) override
    {
        if (m_ImmediateMode)
        {
            RenderImmediate(framebuffer);
            EndFrame();
            return;
        }
        const auto start = std::chrono::steady_clock::now();
        m_Graph->Render(framebuffer);
        const double cost = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        m_Graph->GetRegistry()->GetRunStatistics(m_LastRun);
        if (m_PassTimes)
            CollectGraphPassTimes();
        if (m_Frame == m_Warmup)
        {
            render::RegistryStatistics registry;
            m_Graph->GetRegistry()->GetRegistryStatistics(registry);
            m_ObjectsAtWarmup = registry.objectsCreated;
        }
        if (m_Frame++ >= m_Warmup)
        {
            m_FrameCostMs.push_back(cost);
            m_ExecuteMs.push_back(double(m_LastRun.recordMilliseconds) + double(m_LastRun.submitMilliseconds));
            m_RecordMs.push_back(double(m_LastRun.recordMilliseconds));
            m_Barriers.push_back(m_LastRun.barriers);
        }
        EndFrame();
    }

    // The registry holds the timings of the last frame that completed.
    void CollectGraphPassTimes()
    {
        render::IResourceRegistry* registry = m_Graph->GetRegistry();
        double stages[c_StageCount] = {};
        uint64_t frameIndex = ~0ull;
        for (uint32_t i = 0; i < registry->GetPassTimingCount(); ++i)
        {
            render::PassTiming timing;
            if (registry->GetPassTiming(i, timing) != nvrhi::FS_OK)
                continue;
            frameIndex = timing.frameIndex;
            const int stage = StageOf(timing.name);
            if (stage >= 0)
                stages[stage] += double(timing.gpuMilliseconds);
        }
        if (frameIndex == ~0ull || frameIndex == m_LastTimedFrame)
            return;
        m_LastTimedFrame = frameIndex;
        if (frameIndex >= m_Warmup)
            AddStageTimes(stages);
    }

    // The immediate frame's timer queries: the slot of a frame is read back c_TimedFrames frames later.
    void BeginStage(nvrhi::ICommandList* commandList, uint32_t stage)
    {
        if (m_PassTimes)
            commandList->beginTimerQuery(m_StageTimers[m_ImmediateFrames % c_TimedFrames][stage]);
    }

    void EndStage(nvrhi::ICommandList* commandList, uint32_t stage)
    {
        if (m_PassTimes)
            commandList->endTimerQuery(m_StageTimers[m_ImmediateFrames % c_TimedFrames][stage]);
    }

    void CollectImmediateStageTimes()
    {
        if (!m_PassTimes)
            return;
        nvrhi::TimerQueryHandle (&slot)[c_StageCount] = m_StageTimers[m_ImmediateFrames % c_TimedFrames];
        if (!slot[0])
        {
            for (nvrhi::TimerQueryHandle& timer : slot)
                GetDevice()->createTimerQuery(&timer);
            return;
        }
        // The slot was used c_TimedFrames frames ago.
        double stages[c_StageCount] = {};
        for (uint32_t i = 0; i < c_StageCount; ++i)
        {
            stages[i] = double(GetDevice()->getTimerQueryTime(slot[i])) * 1000.0;
            GetDevice()->resetTimerQuery(slot[i]);
        }
        if (m_ImmediateFrames - c_TimedFrames >= m_Warmup)
            AddStageTimes(stages);
    }

    void WritePassTimes(const char* path)
    {
        FILE* file = std::fopen(path, "w");
        if (!file)
            return;
        std::fprintf(file, "{\n  \"mode\": \"%s\",\n  \"frames\": %u", m_ImmediateMode ? "immediate" : "graph",
                     uint32_t(m_StageMs[c_StageCount].size()));
        for (uint32_t i = 0; i <= c_StageCount; ++i)
            std::fprintf(file, ",\n  \"%s\": %.4f", i < c_StageCount ? c_Stages[i] : "sum", Median(m_StageMs[i]));
        std::fprintf(file, "\n}\n");
        std::fclose(file);
    }

    void BuildGraph(render::IRenderGraph* graph, render::ResourceId backBuffer)
    {
        nvrhi::TextureDesc backBufferDesc;
        if (graph->GetTextureDesc(backBuffer, backBufferDesc) != nvrhi::FS_OK)
            return;
        const uint32_t width = backBufferDesc.width;
        const uint32_t height = backBufferDesc.height;
        SetupViews(width, height);
        engine::SceneGraphNode* root = m_Scene->GetSceneGraph()->GetRootNode();

        // Transients; render targets with their optimized clear value (0), which lets D3D12 clear them fast. The names
        // fit the short string buffer, so that recording allocates nothing.
        auto texture = [&](nvrhi::Format format, bool renderTarget, bool uav, const char* name) {
            nvrhi::TextureDesc desc = nvrhi::TextureDesc().setWidth(width).setHeight(height).setFormat(format)
                .setIsRenderTarget(renderTarget).setIsUAV(uav).setDebugName(name);
            if (renderTarget)
                desc.setClearValue(nvrhi::Color(0.f));
            return graph->CreateTexture(desc, render::ResourceFlags::None);
        };

        // 1. The G-buffer (GBufferRenderTargets' formats, without motion vectors).
        render::GeometryGraphInputs gbuffer;
        gbuffer.name = "GBufferFill";
        gbuffer.view = &m_View;
        gbuffer.viewPrev = &m_View;
        gbuffer.rootNode = root;
        gbuffer.drawStrategy = m_GBufferStrategy;
        gbuffer.passContext = &m_GBufferContext;
        gbuffer.colorTargets[0] = texture(nvrhi::Format::SRGBA8_UNORM, true, false, "GBufferDiffuse");
        gbuffer.colorTargets[1] = texture(nvrhi::Format::SRGBA8_UNORM, true, false, "GBufferSpecular");
        gbuffer.colorTargets[2] = texture(nvrhi::Format::RGBA16_SNORM, true, false, "GBufferNormals");
        gbuffer.colorTargets[3] = texture(nvrhi::Format::RGBA16_FLOAT, true, false, "GBufferEmissive");
        gbuffer.colorTargetCount = 4;
        gbuffer.depth = graph->CreateTexture(nvrhi::TextureDesc().setWidth(width).setHeight(height).setFormat(m_DepthFormat)
            .setIsRenderTarget(true).setIsTypeless(true).setClearValue(nvrhi::Color(0.f)).setDebugName("GBufferDepth"),
            render::ResourceFlags::None);
        gbuffer.clearTargets = true;
        gbuffer.depthClear.depth = 0.f;   // reverse projection
        const render::GeometryGraphOutputs filled = m_GBufferPass->AddToGraph(graph, gbuffer);

        // 2. SSAO, on the compute queue unless -sync.
        render::SsaoGraphInputs ssao;
        ssao.view = &m_View;
        ssao.depth = filled.depth;
        ssao.normals = filled.colorTargets[2];
        ssao.output = texture(nvrhi::Format::R8_UNORM, false, true, "AmbientOcclus");
        ssao.params = m_SsaoParams;
        ssao.asyncCompute = m_AsyncSsao;
        const render::ResourceId ambientOcclusion = m_SsaoPass->AddToGraph(graph, ssao).output;

        // 3. The shadow cascades, into the shadow map's texture: it keeps its initial state, so it is host-kept and
        // ends the frame in that state.
        render::ResourceId shadowMap = graph->ImportHostKeptTexture(m_ShadowMap->GetTexture());
        for (uint32_t cascade = 0; cascade < c_Cascades; ++cascade)
        {
            render::GeometryGraphInputs shadow;
            shadow.name = "ShadowDepth";
            shadow.view = m_ShadowMap->GetCascadeView(cascade);
            shadow.rootNode = root;
            shadow.drawStrategy = m_ShadowStrategies[cascade];
            shadow.passContext = &m_ShadowContexts[cascade];
            shadow.depth = shadowMap;
            shadow.clearTargets = true;   // the cascade's slice, as CascadedShadowMap::Clear
            shadow.depthClear.depth = 1.f;
            shadowMap = m_ShadowDepthPasses[cascade]->AddToGraph(graph, shadow).depth;
        }

        // 4. Lighting; 5. the sky where the depth is clear.
        render::DeferredLightingGraphInputs lighting;
        lighting.view = &m_View;
        lighting.depth = filled.depth;
        lighting.gbufferDiffuse = filled.colorTargets[0];
        lighting.gbufferSpecular = filled.colorTargets[1];
        lighting.gbufferNormals = filled.colorTargets[2];
        lighting.gbufferEmissive = filled.colorTargets[3];
        lighting.ambientOcclusion = ambientOcclusion;
        lighting.shadowMap = shadowMap;
        lighting.output = texture(nvrhi::Format::RGBA16_FLOAT, true, true, "HdrColor");
        lighting.lights = &m_Scene->GetSceneGraph()->GetLights();
        lighting.ambientColorTop = AmbientTop();
        lighting.ambientColorBottom = AmbientBottom();
        render::ResourceId hdr = m_LightingPass->AddToGraph(graph, lighting).output;

        render::SkyGraphInputs sky;
        sky.view = &m_View;
        sky.color = hdr;
        sky.depth = filled.depth;
        sky.light = m_SunLight;
        sky.params = m_SkyParams;
        hdr = m_SkyPass->AddToGraph(graph, sky).color;

        // 6. TAA: motion vectors from depth, then the resolve.
        render::TemporalAntiAliasingGraphInputs taa;
        taa.view = &m_View;
        taa.viewPrevious = &m_ViewPrevious;
        taa.depth = filled.depth;
        taa.motionVectors = texture(nvrhi::Format::RG16_FLOAT, true, false, "MotionVectors");
        taa.unresolvedColor = hdr;
        taa.resolvedColor = texture(nvrhi::Format::RGBA16_FLOAT, true, true, "ResolvedColor");
        taa.params = m_TemporalParams;
        render::ResourceId color = m_TemporalPass->AddToGraph(graph, taa).resolvedColor;

        // 7. Bloom; 8. tone mapping; 9. the blit.
        render::BloomGraphInputs bloom;
        bloom.view = &m_View;
        bloom.color = color;
        bloom.sigmaInPixels = m_BloomSigma;
        bloom.blendFactor = m_BloomAlpha;
        color = m_BloomPass->AddToGraph(graph, bloom).color;

        render::ToneMappingGraphInputs toneMapping;
        toneMapping.view = &m_View;
        toneMapping.source = color;
        toneMapping.output = texture(nvrhi::Format::RGBA8_UNORM, true, false, "LdrColor");
        toneMapping.params = m_ToneMappingParams;
        toneMapping.frameTime = m_FrameTime;
        const render::ResourceId ldr = m_ToneMappingPass->AddToGraph(graph, toneMapping).output;

        render::BlitGraphInputs blit;
        blit.source = ldr;
        blit.target = backBuffer;
        m_BlitPass->AddToGraph(graph, blit);
    }

    // The same frame without a graph: one command list, nvrhi's automatic barriers, SSAO on the graphics queue.
    void RenderImmediate(nvrhi::IFramebuffer* framebuffer)
    {
        const nvrhi::FramebufferInfoEx& info = framebuffer->getFramebufferInfo();
        const uint2 size(info.width, info.height);
        if (!m_Immediate || any(m_Immediate->size != size))
            CreateImmediateFrame(size);
        ImmediateFrame& frame = *m_Immediate;
        SetupViews(size.x, size.y);
        engine::SceneGraphNode* root = m_Scene->GetSceneGraph()->GetRootNode();

        CollectImmediateStageTimes();
        nvrhi::ICommandList* commandList = m_CommandList;
        commandList->open();

        BeginStage(commandList, 0);
        frame.gbuffer.Clear(commandList);
        render::RenderCompositeView(commandList, &m_View, &m_View, frame.gbuffer.GBufferFramebuffer, root,
                                    *m_GBufferStrategy, *m_GBufferPass, m_GBufferContext, "GBufferFill");
        EndStage(commandList, 0);

        BeginStage(commandList, 1);
        frame.ssao->Render(commandList, m_SsaoParams, m_View);
        EndStage(commandList, 1);

        BeginStage(commandList, 2);
        m_ShadowMap->Clear(commandList);
        render::RenderCompositeView(commandList, &m_ShadowMap->GetView(), nullptr, frame.shadowFramebuffer, root,
                                    *m_ShadowStrategies[0], *m_ShadowDepthPasses[0], m_ShadowContexts[0], "ShadowDepth");
        EndStage(commandList, 2);

        render::DeferredLightingPass::Inputs lighting;
        lighting.SetGBuffer(frame.gbuffer);
        lighting.ambientOcclusion = frame.ambientOcclusion;
        lighting.output = frame.hdrColor;
        lighting.lights = &m_Scene->GetSceneGraph()->GetLights();
        lighting.ambientColorTop = AmbientTop();
        lighting.ambientColorBottom = AmbientBottom();
        BeginStage(commandList, 3);
        m_LightingPass->Render(commandList, m_View, lighting);
        EndStage(commandList, 3);

        BeginStage(commandList, 4);
        frame.sky->Render(commandList, m_View, *m_SunLight, m_SkyParams);
        EndStage(commandList, 4);

        BeginStage(commandList, 5);
        frame.temporal->RenderMotionVectors(commandList, m_View, m_ViewPrevious);
        EndStage(commandList, 5);
        BeginStage(commandList, 6);
        frame.temporal->TemporalResolve(commandList, m_TemporalParams, frame.feedbackValid, m_View, m_View);
        EndStage(commandList, 6);
        frame.temporal->AdvanceFrame();
        frame.feedbackValid = true;

        BeginStage(commandList, 7);
        frame.bloom->Render(commandList, frame.resolvedFramebuffer, &m_View, frame.resolvedColor, m_BloomSigma, m_BloomAlpha);
        EndStage(commandList, 7);

        // As the graph's first frame: a new exposure is not adapted from.
        render::ToneMappingParameters toneMapping = m_ToneMappingParams;
        if (!frame.exposureValid)
            toneMapping.eyeAdaptationSpeedUp = toneMapping.eyeAdaptationSpeedDown = 0.f;
        frame.exposureValid = true;
        frame.toneMapping->AdvanceFrame(m_FrameTime);
        BeginStage(commandList, 8);
        frame.toneMapping->SimpleRender(commandList, toneMapping, m_View, frame.resolvedColor);
        EndStage(commandList, 8);

        BeginStage(commandList, 9);
        m_CommonPasses->BlitTexture(commandList, framebuffer, frame.ldrColor, m_BindingCache.get());
        EndStage(commandList, 9);

        commandList->close();
        GetDevice()->executeCommandList(commandList);
        ++m_ImmediateFrames;
    }

    void CreateImmediateFrame(uint2 size)
    {
        GetDevice()->waitForIdle();
        m_BindingCache->Clear();
        m_Immediate = std::make_unique<ImmediateFrame>();
        ImmediateFrame& frame = *m_Immediate;
        frame.size = size;
        frame.gbuffer.Init(GetDevice(), size, 1, false, true);

        auto texture = [&](nvrhi::Format format, bool renderTarget, bool uav, const char* name) {
            nvrhi::TextureDesc desc = nvrhi::TextureDesc().setWidth(size.x).setHeight(size.y).setFormat(format)
                .setIsRenderTarget(renderTarget).setIsUAV(uav).setDebugName(name)
                .setInitialState(renderTarget ? nvrhi::ResourceStates::RenderTarget : nvrhi::ResourceStates::UnorderedAccess)
                .setKeepInitialState(true);
            if (renderTarget)
                desc.setClearValue(nvrhi::Color(0.f));
            nvrhi::TextureHandle handle;
            GetDevice()->createTexture(desc, &handle);
            return handle;
        };
        frame.ambientOcclusion = texture(nvrhi::Format::R8_UNORM, false, true, "AmbientOcclus");
        frame.hdrColor = texture(nvrhi::Format::RGBA16_FLOAT, true, true, "HdrColor");
        frame.motionVectors = texture(nvrhi::Format::RG16_FLOAT, true, false, "MotionVectors");
        frame.resolvedColor = texture(nvrhi::Format::RGBA16_FLOAT, true, true, "ResolvedColor");
        frame.feedback1 = texture(nvrhi::Format::RGBA16_FLOAT, true, true, "Feedback1");
        frame.feedback2 = texture(nvrhi::Format::RGBA16_FLOAT, true, true, "Feedback2");
        frame.ldrColor = texture(nvrhi::Format::RGBA8_UNORM, true, false, "LdrColor");

        frame.hdrFramebuffer = MAKE_RC_OBJ_PTR(engine::FramebufferFactory, GetDevice());
        frame.hdrFramebuffer->RenderTargets = { frame.hdrColor };
        frame.hdrFramebuffer->DepthTarget = frame.gbuffer.Depth;
        frame.resolvedFramebuffer = MAKE_RC_OBJ_PTR(engine::FramebufferFactory, GetDevice());
        frame.resolvedFramebuffer->RenderTargets = { frame.resolvedColor };
        frame.ldrFramebuffer = MAKE_RC_OBJ_PTR(engine::FramebufferFactory, GetDevice());
        frame.ldrFramebuffer->RenderTargets = { frame.ldrColor };
        frame.shadowFramebuffer = MAKE_RC_OBJ_PTR(engine::FramebufferFactory, GetDevice());
        frame.shadowFramebuffer->DepthTarget = m_ShadowMap->GetTexture();

        // The view the passes size themselves from.
        SetupViews(size.x, size.y);
        frame.ssao = MAKE_RC_OBJ_PTR(render::SsaoPass, GetDevice(), m_ShaderFactory, m_CommonPasses,
                                     frame.gbuffer.Depth, frame.gbuffer.GBufferNormals, frame.ambientOcclusion);
        frame.sky = MAKE_RC_OBJ_PTR(render::SkyPass, GetDevice(), m_ShaderFactory, m_CommonPasses, frame.hdrFramebuffer, m_View);
        render::TemporalAntiAliasingPass::CreateParameters taaParams = m_TemporalCreateParams;
        taaParams.sourceDepth = frame.gbuffer.Depth;
        taaParams.motionVectors = frame.motionVectors;
        taaParams.unresolvedColor = frame.hdrColor;
        taaParams.resolvedColor = frame.resolvedColor;
        taaParams.feedback1 = frame.feedback1;
        taaParams.feedback2 = frame.feedback2;
        frame.temporal = MAKE_RC_OBJ_PTR(render::TemporalAntiAliasingPass, GetDevice(), m_ShaderFactory, m_CommonPasses,
                                         m_View, taaParams);
        frame.bloom = MAKE_RC_OBJ_PTR(render::BloomPass, GetDevice(), m_ShaderFactory, m_CommonPasses,
                                      frame.resolvedFramebuffer, m_View);
        frame.toneMapping = MAKE_RC_OBJ_PTR(render::ToneMappingPass, GetDevice(), m_ShaderFactory, m_CommonPasses,
                                            frame.ldrFramebuffer, m_View, m_ToneMappingCreateParams);
    }

    static float3 AmbientTop() { return float3(0.2f); }
    static float3 AmbientBottom() { return float3(0.2f) * float3(0.3f, 0.4f, 0.3f); }

    void WriteGraphStats(const char* path)
    {
        FILE* file = std::fopen(path, "w");
        if (!file)
            return;
        uint32_t barriersMin = UINT32_MAX, barriersMax = 0;
        for (uint32_t barriers : m_Barriers)
        {
            barriersMin = std::min(barriersMin, barriers);
            barriersMax = std::max(barriersMax, barriers);
        }
        render::RegistryStatistics registry;
        m_Graph->GetRegistry()->GetRegistryStatistics(registry);
        render::PlanStatistics plan;
        uint32_t computePasses = 0;
        if (render::ICompiledRenderGraph* last = m_Graph->GetLastPlan())
        {
            last->GetStatistics(plan);
            for (uint32_t i = 0; i < last->GetScheduledPassCount(); ++i)
            {
                render::ScheduledPass pass;
                if (last->GetScheduledPass(i, pass) == nvrhi::FS_OK && pass.queue == nvrhi::CommandQueue::Compute)
                    ++computePasses;
            }
        }
        const render::BackendProfile& profile = m_Graph->GetProfile();
        const uint64_t peak = plan.placedPeakBytes + plan.unplacedTransientBytes;
        std::fprintf(file, "{\n  \"frames\": %u,\n  \"warmup\": %u,\n  \"cache\": %s,\n  \"timers\": %s,\n"
            "  \"async_ssao\": %s,\n  \"split_barriers\": %s,\n  \"placed\": %s,\n  \"queue_ownership_transfer\": %s,\n"
            "  \"median_layer_frame_ms\": %.4f,\n  \"median_execute_ms\": %.4f,\n  \"median_record_ms\": %.4f,\n"
            "  \"barriers_min\": %u,\n  \"barriers_max\": %u,\n  \"threads\": %u,\n  \"record_tasks\": %u,\n"
            "  \"passes\": %u,\n  \"segments\": %u,\n  \"compute_queue_passes\": %u,\n  \"plan_barriers\": %u,\n"
            "  \"plan_transitions\": %u,\n  \"plan_split_barriers\": %u,\n  \"plan_aliasing_barriers\": %u,\n"
            "  \"plan_queue_transfers\": %u,\n  \"plan_fence_waits\": %u,\n  \"heaps\": %u,\n"
            "  \"poolable_transient_bytes\": %llu,\n  \"placed_peak_bytes\": %llu,\n  \"unplaced_transient_bytes\": %llu,\n"
            "  \"peak_ratio\": %.4f,\n  \"registry_heap_bytes\": %llu,\n  \"registry_placed_object_bytes\": %llu,\n"
            "  \"objects_created_after_warmup\": %llu,\n  \"plan_hits\": %llu,\n  \"plan_misses\": %llu\n}\n",
            m_Frame, m_Warmup, m_Graph->GetCompileOptions().maxCachedPlans ? "true" : "false",
            m_Graph->GetExecuteDesc().timers ? "true" : "false", m_AsyncSsao ? "true" : "false",
            profile.splitBarriers ? "true" : "false", m_Graph->GetCompileOptions().placedResources ? "true" : "false",
            profile.queueOwnershipTransfer ? "true" : "false",
            Median(m_FrameCostMs), Median(m_ExecuteMs), Median(m_RecordMs), m_Barriers.empty() ? 0u : barriersMin, barriersMax,
            m_Recording.GetThreadCount(), m_LastRun.recordTasks, m_LastRun.passes, m_LastRun.segments, computePasses, plan.barriers, plan.transitions, plan.splitBarriers,
            plan.aliasingBarriers, plan.queueTransfers, plan.fenceWaits, plan.heaps,
            (unsigned long long)plan.poolableTransientBytes, (unsigned long long)plan.placedPeakBytes,
            (unsigned long long)plan.unplacedTransientBytes,
            plan.poolableTransientBytes ? double(peak) / double(plan.poolableTransientBytes) : 0.0,
            (unsigned long long)registry.heapBytes, (unsigned long long)registry.placedObjectBytes,
            (unsigned long long)(registry.objectsCreated - m_ObjectsAtWarmup),
            (unsigned long long)registry.planHits, (unsigned long long)registry.planMisses);
        std::fclose(file);
    }
};

void FrameGraphLayer::BuildGraph(render::IRenderGraph* graph, render::FrameData&, render::ResourceId backBuffer)
{
    m_Sample->BuildGraph(graph, backBuffer);
}

#ifdef WIN32
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
#else
int main(int __argc, const char** __argv)
#endif
{
    nvrhi::GraphicsAPI api = app::GetGraphicsAPIFromCommandLine(__argc, __argv);
    auto deviceManager = nvrhi::TakeOver(app::DeviceManager::Create(api));

    app::DeviceCreationParameters deviceParams;
    deviceParams.enableComputeQueue = true;
    for (int i = 1; i + 1 < __argc; ++i)
    {
        unsigned width = 0, height = 0;
        if (!std::strcmp(__argv[i], "-size") && std::sscanf(__argv[i + 1], "%ux%u", &width, &height) == 2 && width && height)
        {
            deviceParams.backBufferWidth = width;
            deviceParams.backBufferHeight = height;
        }
    }
#ifdef _DEBUG
    deviceParams.enableDebugRuntime = true;
    deviceParams.enableNvrhiValidationLayer = true;
#endif

    if (!deviceManager->CreateWindowDeviceAndSwapChain(deviceParams, g_WindowTitle))
    {
        log::error("Cannot initialize a graphics device with the requested parameters");
        return 1;
    }

    {
        sample_harness::Harness harness(deviceManager, sample_harness::ParseOptions(__argc, __argv));
        AsyncComputeGraph example(deviceManager);
        if (example.Init(__argc, __argv, harness.GetOptions().warmup))
        {
            // -immediate renders the sample's own layer; otherwise it records through its graph layer.
            deviceManager->AddLayerToBack(&example);
            nvrhi::AutoPtr<app::RenderGraphVisualizer> visualizer;
            if (!harness.GetOptions().frames && !example.IsImmediate())
            {
                visualizer = MAKE_RC_OBJ_PTR(app::RenderGraphVisualizer, deviceManager, example.GetGraphLayer());
                if (visualizer->Init(example.GetShaderFactory()))
                    deviceManager->AddLayerToBack(visualizer);
                else
                    visualizer = nullptr;
            }
            harness.Attach();
            deviceManager->RunMessageLoop();
            harness.Detach();
            for (int i = 1; i + 1 < __argc; ++i)
            {
                if (!std::strcmp(__argv[i], "-graphstats") && !example.IsImmediate())
                    example.WriteGraphStats(__argv[i + 1]);
                if (!std::strcmp(__argv[i], "-passtimes"))
                    example.WritePassTimes(__argv[i + 1]);
            }
            if (visualizer)
                deviceManager->RemoveLayer(visualizer);
            deviceManager->RemoveLayer(&example);
        }
    }

    deviceManager->Shutdown();

    return 0;
}
