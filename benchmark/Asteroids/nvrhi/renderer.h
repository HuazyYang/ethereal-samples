// asteroids_nvrhi: the Asteroids workload of the reference sample rendered through Donut + nvrhi.
// The renderer is a Donut render pass; the frame loop is Donut's DeviceManager::RunMessageLoop and the
// benchmark columns are taken at the DeviceManager pipeline callbacks (see InstallFrameCallbacks).
#pragma once

#include <donut/app/DeviceManager.h>
#include <donut/engine/ShaderFactory.h>
#include <nvrhi/nvrhi.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "benchmark.h"

// Reference sample (DiligentSamples/Samples/Asteroids/src)
#include "camera.h"
#include "settings.h"
#include "simulation.h"

// docs/PLAN.md "Binding modes" (dyn is not expressible with nvrhi: binding sets are immutable).
enum class BindingMode
{
    Mut,      // one binding set per asteroid (50,000) + volatile constant buffer
    TexMut,   // one binding set per texture (10) + volatile constant buffer
    TexMutPC, // one binding set per texture (10) + push constants
    Bindless  // descriptor table with all textures + per-instance structured buffer
};

struct RendererDesc
{
    BindingMode binding       = BindingMode::TexMut;
    uint32_t    threads       = 1;
    bool        gpuTiming     = false;

    // Sensitivity switches. The defaults are what a competent nvrhi application does for this workload.
    bool trackLiveness  = false; // -track_liveness: BindingSetDesc::trackLiveness = true (nvrhi's own default)
    bool alwaysSetState = false; // -always_set_state: setGraphicsState before every draw instead of only when the
                                 // texture binding set changes (what Diligent's CommitShaderResources per draw does)
    bool noAutoBarriers = false; // -no_auto_barriers: setEnableAutomaticBarriers(false) after the first
                                 // setGraphicsState of every command list, re-enabled after the draw loop

    // Placement of the nvrhi D3D12 command list objects in memory (docs/LIMITATIONS.md, "page-split store").
    // nvrhi copies a 776-byte array into a member of its D3D12 CommandList on every setGraphicsState that
    // changes a binding set. When a 4 KB page boundary happens to lie inside that member, one store of the copy
    // straddles two pages, and on this CPU such a store costs about 80 ns whenever write-combined memory (the
    // upload chunk of a volatile constant buffer) was written shortly before. Where the heap puts the object is
    // chance (about 19 % of the placements are affected) and fixed for the life of a process.
    //   Natural: use the command list the heap hands out (default; the run records what it got).
    //   Avoid / Force: create command lists until one does not / does have a page boundary inside that member.
    enum class CbPageSplit { Natural, Avoid, Force };
    CbPageSplit cbPageSplit = CbPageSplit::Natural; // -cb_page_split_avoid | -cb_page_split_force (D3D12 only)
};

class AsteroidsRenderer final : public donut::app::IRenderPass
{
    NVRHI_INHERIT_INTERFACE_TABLE()
public:
    AsteroidsRenderer(donut::app::DeviceManager* deviceManager, const RendererDesc& desc, AsteroidsSimulation* simulation,
                      OrbitCamera* camera, const Settings* settings);
    ~AsteroidsRenderer() override;

    // Creates every GPU object that does not depend on the back buffer. Errors are reported through
    // donut::log (fatal for this application).
    bool Init(donut::engine::ShaderFactory& shaderFactory, const std::filesystem::path& mediaDirectory);

    // Hooks the benchmark recorder into DeviceManager::m_callbacks and opens the first frame.
    void InstallFrameCallbacks();

    // Joins the worker threads and releases all GPU objects. Call before DeviceManager::Shutdown.
    void Shutdown();

    void FillRunInfo(Benchmark::RunInfo& info) const;

    // IRenderPass
    bool ShouldAnimateUnfocused() override { return true; }
    bool ShouldRenderUnfocused() override { return true; }
    bool SupportsDepthBuffer() override { return false; } // the pass owns its depth buffer (reverse Z, clear value 0)
    void Animate(float elapsedTimeSeconds) override;
    void Render(nvrhi::IFramebuffer* framebuffer) override;
    void BackBufferResizing() override;
    void BackBufferResized(uint32_t width, uint32_t height, uint32_t sampleCount) override;

    // Layout of the per-asteroid shader data (AsteroidData in shaders/asteroid_vs.hlsl).
    struct AsteroidData
    {
        DirectX::XMFLOAT4X4 world;
        DirectX::XMFLOAT3   surfaceColor;
        float               unused0;
        DirectX::XMFLOAT3   deepColor;
        uint32_t            textureIndex;
    };

private:
    enum class Job { Update, Render, Exit };

    // Everything one recording thread touches while recording. Thread 0 is the main thread.
    struct alignas(64) ThreadContext
    {
        nvrhi::CommandListHandle commandList;
        nvrhi::BufferHandle      constantBuffer; // volatile: per draw (mut, tex_mut) or per command list (tex_mut_pc, bindless)
        nvrhi::BufferHandle      instanceData;   // bindless: StructuredBuffer<AsteroidData> of this subset
        nvrhi::BindingSetHandle  bindingSet0;
        nvrhi::GraphicsState     state;          // everything but the framebuffer stays constant
        std::vector<AsteroidData> staging;       // bindless: CPU copy handed to ICommandList::writeBuffer
        nvrhi::CommandListHandle pendingCommandList; // -cb_page_split_*: replaces commandList before the next frame
        int      cbPageSplit = -1;               // D3D12: 1 = a page boundary lies inside the command list's volatile
                                                 // constant buffer state, 0 = not, -1 = unknown
        uint32_t first = 0, count = 0;           // asteroid range of this subset
        uint64_t draws = 0, indices = 0;         // statistics of the last recorded frame
        std::thread thread;
    };

    void CreateThreadResources();
    void CreatePipelines(nvrhi::IFramebuffer* framebuffer);
    void WorkerMain(uint32_t threadIndex);
    void Dispatch(Job job);
    void JoinWorkers();

    void RecordAsteroids(ThreadContext& ctx);
    void RecordSkybox(nvrhi::ICommandList* commandList);
    void BeginGpuTimer(nvrhi::ICommandList* commandList);
    void EndGpuTimer(nvrhi::ICommandList* commandList);
    void PollGpuTimers();
    void ProbeCommandListPlacement(ThreadContext& main);
    void ApplyPendingCommandLists();
    void PrepareCapture();
    void FinishCapture();

    // DeviceManager pipeline callbacks
    void OnBeforeFrame();
    void OnAfterAnimate();
    void OnBeforeRender();
    void OnBeforePresent();
    void OnAfterPresent();

    RendererDesc         m_Desc;
    nvrhi::DeviceHandle  m_Device;
    nvrhi::GraphicsAPI   m_Api;
    AsteroidsSimulation* m_Simulation;
    OrbitCamera*         m_Camera;
    const Settings*      m_Settings;

    // Static scene resources
    nvrhi::BufferHandle  m_VertexBuffer, m_IndexBuffer, m_SkyboxVertexBuffer, m_InstanceIdBuffer;
    nvrhi::TextureHandle m_Textures[NUM_UNIQUE_TEXTURES];
    nvrhi::TextureHandle m_SkyboxTexture;
    nvrhi::SamplerHandle m_Sampler;

    nvrhi::ShaderHandle      m_AsteroidVS, m_AsteroidPS, m_SkyboxVS, m_SkyboxPS;
    nvrhi::InputLayoutHandle m_AsteroidInputLayout, m_SkyboxInputLayout;
    nvrhi::BindingLayoutHandle m_Layout0, m_Layout1, m_SkyboxLayout;
    nvrhi::GraphicsPipelineHandle m_AsteroidPipeline, m_SkyboxPipeline;

    // mut: one set per asteroid, tex_mut / tex_mut_pc: one set per texture (bound at layout index 1)
    std::vector<nvrhi::BindingSetHandle> m_TextureSets;
    std::vector<nvrhi::IBindingSet*>     m_TextureSetPtrs; // raw pointers for the draw loop
    nvrhi::DescriptorTableHandle         m_TextureTable;   // bindless

    nvrhi::BufferHandle     m_SkyboxConstantBuffer;
    nvrhi::BindingSetHandle m_SkyboxBindingSet;
    nvrhi::GraphicsState    m_SkyboxState;

    // Back-buffer dependent
    nvrhi::TextureHandle                  m_DepthBuffer;
    std::vector<nvrhi::FramebufferHandle> m_Framebuffers;
    uint32_t m_Width = 0, m_Height = 0;

    // Threads: m_Threads[0] is recorded on the main thread, the others on persistent workers.
    std::vector<std::unique_ptr<ThreadContext>> m_Threads;
    nvrhi::CommandListHandle        m_PostCommandList; // skybox, only with more than one thread
    std::vector<nvrhi::ICommandList*> m_ExecuteList;
    std::mutex              m_Mutex;
    std::condition_variable m_Signal;
    uint64_t                m_Generation = 0;
    Job                     m_Job = Job::Update;
    std::atomic<int>        m_Completed{0};

    // Frame state shared with the workers (written by the main thread before Dispatch)
    float                m_FrameDelta = 0.f;
    nvrhi::IFramebuffer* m_FrameFramebuffer = nullptr;
    DirectX::XMFLOAT4X4  m_ViewProjection{};

    // Benchmark bookkeeping
    bool    m_Done = false;
    bool    m_RenderedThisFrame = false;
    float   m_SmoothedDelta = 0.f;
    int64_t m_FrameStartStamp = 0, m_WaitStamp = 0, m_PresentStamp = 0;
    std::vector<double> m_GcSamples; // ms between the end of Present and the next frame (Donut's garbage collection)
    size_t  m_UploadChunkSize = 0;
    nvrhi::CommandListParameters m_CommandListParams;

    // Command list placement (RendererDesc::cbPageSplit); D3D12 only.
    bool     m_PlacementProbed = false;
    bool     m_PlacementPending = false;
    int64_t  m_CbStateOffset = -1;      // offset of the volatile constant buffer state inside nvrhi's D3D12 CommandList
    uint32_t m_PlacementCandidates = 0; // command lists created to satisfy -cb_page_split_avoid / _force
    std::vector<nvrhi::CommandListHandle> m_RetiredCommandLists; // replaced lists: kept alive, the GPU may still use them
    std::string m_UploadHeapProtection; // page protection of a mapped D3D12 upload buffer (write-combined or not)
    uint64_t m_DrawConstantsAddress = 0;
    uint32_t m_VolatileVersions = 0; // largest maxVersions of the per-thread volatile constant buffers

    // GPU timing (nvrhi timer queries)
    std::vector<nvrhi::TimerQueryHandle> m_FreeQueries;
    std::deque<std::pair<uint64_t, nvrhi::TimerQueryHandle>> m_PendingQueries;
    nvrhi::TimerQueryHandle m_ActiveQuery;
    uint64_t m_GpuFramesSkipped = 0;

    // Frame capture (-capture_frame): the frame is rendered into an offscreen copy of the back buffer
    nvrhi::TextureHandle        m_CaptureTexture;
    nvrhi::FramebufferHandle    m_CaptureFramebuffer;
    nvrhi::StagingTextureHandle m_CaptureStaging;
    bool m_CapturePending = false;
};
