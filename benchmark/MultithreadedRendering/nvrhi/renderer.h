#pragma once
// mt_rendering_nvrhi: the MultithreadedRendering workload on Donut + nvrhi (D3D12, Vulkan). Same
// scene, passes, draw order and thread orchestration as the native renderer (core/); only the
// graphics API layer differs. The frame loop is Donut's DeviceManager::RunMessageLoop, and the
// benchmark columns are taken at its pipeline callbacks (see InstallFrameCallbacks).

#include <donut/app/DeviceManager.h>
#include <donut/engine/ShaderFactory.h>
#include <nvrhi/nvrhi.h>

#include <memory>
#include <string>
#include <vector>

#include "benchmark.h"
#include "d3d12_debug_messages.h"
#include "scene.h"
#include "sdkmesh.h"
#include "workers.h"

struct RendererDesc
{
    MtScene::Mode mode = MtScene::Mode::SingleThreaded;
    uint32_t threads = 1;
    bool tightMirrorScissor = false;
    // Sensitivity switch: record every list with nvrhi's automatic barriers off and place the
    // resource transitions explicitly where the native renderer places them (default: automatic).
    bool explicitBarriers = false;
    bool debug = false; // -debug: D3D12 debug layer messages are drained after every frame
    // -freeze T: render the scripted frame at time T (parity screenshots); negative = off
    float freezeTime = -1.f;
    // -screenshot FILE.bmp: save the 30th frame as a BMP and close the window
    std::filesystem::path screenshot;
};

class MtRenderer final : public donut::app::IRenderPass
{
    NVRHI_INHERIT_INTERFACE_TABLE()
public:
    MtRenderer(donut::app::DeviceManager* deviceManager, const RendererDesc& desc, MtScene::Scene* scene);
    ~MtRenderer() override;

    bool Init(donut::engine::ShaderFactory& shaderFactory, const MtScene::SdkMesh& mesh);
    void InstallFrameCallbacks();
    void Shutdown();
    void FillRunInfo(Benchmark::RunInfo& info) const;

    void SetMode(MtScene::Mode mode) { m_Mode = mode; }
    MtScene::Mode GetMode() const { return m_Mode; }
    uint32_t Threads() const { return m_Threads; }
    uint64_t LastFrameDraws() const { return m_LastDraws; }
    uint64_t LastFrameIndices() const { return m_LastIndices; }
    uint32_t TextureCount() const { return uint32_t(m_Textures.size()); }

    // IRenderPass
    bool ShouldAnimateUnfocused() override { return true; }
    bool ShouldRenderUnfocused() override { return true; }
    bool SupportsDepthBuffer() override { return true; } // Donut's swap chain depth buffer (D24S8)
    void Animate(float elapsedTimeSeconds) override;
    void Render(nvrhi::IFramebuffer* framebuffer) override;
    void BackBufferResizing() override;
    void BackBufferResized(uint32_t width, uint32_t height, uint32_t sampleCount) override;
    bool KeyboardUpdate(int key, int scancode, int action, int mods) override;
    bool MousePosUpdate(double xpos, double ypos) override;
    bool MouseButtonUpdate(int button, int action, int mods) override;
    bool MouseScrollUpdate(double xoffset, double yoffset) override;

    // MtScene::RunFrame interface
    void OpenList(int pass, int chunk);
    void RecordPass(int pass, int chunk, int chunkCount, int listPass);
    void CloseList(int pass, int chunk);
    void ExecuteLists(int pass, int chunkCount);
    void ExecuteAllPasses();

private:
    enum Pipeline : int
    {
        PipelineShadow = 0,
        PipelineDirect,
        PipelineMirrorStencil,
        PipelineMirrorClearDepth = PipelineMirrorStencil + 8,
        PipelineMirroredScene = PipelineMirrorClearDepth + 8,
        PipelineMirrorSurface = PipelineMirroredScene + 8,
        PipelineCount = PipelineMirrorSurface + 8
    };

    struct MeshBuffers
    {
        nvrhi::BufferHandle vertexBuffer;
        nvrhi::BufferHandle indexBuffer;
        nvrhi::Format indexFormat = nvrhi::Format::R16_UINT;
    };

    struct Subset
    {
        nvrhi::PrimitiveType primType;
        uint32_t material;
        uint32_t indexCount, indexStart, vertexStart;
    };

    struct MeshDraw
    {
        uint32_t buffers;
        uint32_t firstSubset, subsetCount;
    };

    struct ListStats
    {
        uint64_t draws = 0, indices = 0;
    };

    // Per recording chunk: its command lists (one per pass) and its own volatile constant buffers,
    // bound through binding set 0 together with the shadow map and the samplers.
    struct alignas(64) ChunkContext
    {
        nvrhi::CommandListHandle lists[MtScene::kNumPasses];
        nvrhi::BufferHandle cbObject, cbLight, cbScene;
        nvrhi::BindingSetHandle set0;         // b0, b1, b2, shadow map, samplers (pipelines with a pixel shader)
        nvrhi::BindingSetHandle set0NoLights; // b0, b2 (depth / stencil only pipelines: no light data is written)
        ListStats stats[MtScene::kNumPasses];
    };

    struct FrameParams
    {
        MtScene::SceneDraw shadow[MtScene::kNumShadows];
        MtScene::MirrorPass mirror[MtScene::kNumMirrors];
        MtScene::SceneDraw direct;
        MtScene::CbPerLight lights;
    };

    void CreateShadowMaps();
    void CreateChunkContexts();
    void CreatePipelines(nvrhi::IFramebuffer* framebuffer);
    bool LoadModel(const MtScene::SdkMesh& mesh, nvrhi::ICommandList* commandList);
    nvrhi::TextureHandle CreateSolidTexture(nvrhi::ICommandList* commandList, uint32_t rgba, const char* name);
    nvrhi::IFramebuffer* ShadowFramebuffer(int shadow) { return m_ShadowFramebuffers[shadow]; }

    bool SaveBackBuffer(nvrhi::IFramebuffer* framebuffer, const std::filesystem::path& path, std::string& error);
    void RenderScene(nvrhi::ICommandList* list, ChunkContext& ctx, const MtScene::SceneDraw& draw, int chunk, int chunkCount, ListStats& stats);
    void DrawMirrorQuad(nvrhi::ICommandList* list, ChunkContext& ctx, int pipeline, int mirror, const MtScene::CbPerObject& object,
                        const MtScene::CbPerScene& scene, const RECT& scissor);
    nvrhi::ViewportState Viewport(uint32_t width, uint32_t height, const RECT& scissor) const;

    // DeviceManager pipeline callbacks
    void OnBeforeFrame();
    void OnAfterAnimate();
    void OnBeforeRender();
    void OnBeforePresent();
    void OnAfterPresent();

    RendererDesc m_Desc;
    MtScene::Mode m_Mode;
    uint32_t m_Threads;
    nvrhi::DeviceHandle m_Device;
    nvrhi::GraphicsAPI m_Api;
    MtScene::Scene* m_Scene;

    // Static scene
    std::vector<MeshBuffers> m_MeshBuffers;
    std::vector<Subset> m_Subsets;
    std::vector<MeshDraw> m_RenderOrder;
    std::vector<nvrhi::BindingSetHandle> m_MaterialSets; // set 1: diffuse + normal map per material
    std::vector<nvrhi::IBindingSet*> m_MaterialSetPtrs;
    std::vector<nvrhi::TextureHandle> m_Textures;
    nvrhi::BufferHandle m_MirrorVertexBuffer;
    nvrhi::SamplerHandle m_PointClampSampler, m_LinearClampSampler;

    nvrhi::ShaderHandle m_VertexShader, m_PixelShader;
    nvrhi::InputLayoutHandle m_CompressedLayout, m_UncompressedLayout;
    nvrhi::BindingLayoutHandle m_Layout0, m_Layout0NoLights, m_Layout1;
    nvrhi::GraphicsPipelineHandle m_Pipelines[PipelineCount];
    nvrhi::FramebufferInfoEx m_PipelineFramebufferInfo;

    nvrhi::TextureHandle m_ShadowMaps[MtScene::kNumShadows];
    nvrhi::FramebufferHandle m_ShadowFramebuffers[MtScene::kNumShadows];

    std::vector<std::unique_ptr<ChunkContext>> m_Chunks;
    std::vector<nvrhi::ICommandList*> m_ExecuteScratch;
    std::unique_ptr<MtScene::WorkerPool> m_Workers;

    // Frame state
    FrameParams m_Params;
    nvrhi::IFramebuffer* m_FrameFramebuffer = nullptr;
    uint32_t m_Width = 0, m_Height = 0;
    float m_Time = 0.f;
    int64_t m_StartStamp = 0;
    uint64_t m_LastDraws = 0, m_LastIndices = 0;
    uint64_t m_RenderedFrames = 0;

    // interactive camera
    bool m_Dragging = false;
    double m_MouseX = 0, m_MouseY = 0;

    Benchmark::D3D12DebugMessages m_DebugMessages; // -debug, D3D12 only

    // Benchmark bookkeeping (see InstallFrameCallbacks)
    bool m_Done = false;
    int64_t m_WaitStamp = 0, m_PresentStamp = 0, m_AfterPresentStamp = 0;
    bool m_HaveAfterPresent = false;
};
