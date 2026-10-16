#pragma once
// The D3D12 renderer of the MultithreadedRendering sample (D3D12-Trial/MultithreadedRendering),
// reduced to the frame work that is measured: the sample's root signature, its 34 pipeline states,
// the shadow map, the four stencil mirrors and the direct view, recorded single-threaded, one list
// per scene (shadow / mirror / direct) or split into per-thread chunks of the draw list. DXUT, the
// sample's framework (D3D12RendererContext, ImGui, DirectXTex, D3D12MA) and the runtime shader
// compiler are replaced by a minimal host; the GPU work per frame is unchanged.

#include "d3d12_helpers.h"
#include "d3d12_debug_messages.h"

#include <array>
#include <filesystem>
#include <memory>
#include <vector>

#include "dds.h"
#include "scene.h"
#include "sdkmesh.h"
#include "workers.h"

namespace MtNative
{

struct RendererDesc
{
    MtScene::Mode mode = MtScene::Mode::SingleThreaded;
    uint32_t threads = 1;        // recording threads including the main thread
    HWND window = nullptr;
    uint32_t width = 1280, height = 720;
    IDXGIAdapter1* adapter = nullptr; // nullptr: the first hardware adapter
    bool allowTearing = true;
    bool debugLayer = false;
};

class RendererD3D12
{
public:
    static constexpr uint32_t kFramesInFlight = 3;
    static constexpr uint32_t kSwapChainBuffers = 3;

    RendererD3D12(const RendererDesc& desc, const MtScene::SdkMesh& mesh);
    ~RendererD3D12();

    void Resize(uint32_t width, uint32_t height);
    void SetMode(MtScene::Mode mode) { m_Mode = mode; }
    MtScene::Mode GetMode() const { return m_Mode; }
    uint32_t Threads() const { return m_Threads; }

    // Waits for the frame's resources, records and executes every pass, presents.
    void RenderFrame(const MtScene::Scene& scene, Benchmark::FrameTimes& times);
    void WaitForGpu();

    // Reads the last presented back buffer back and writes it as a 24-bit BMP (parity screenshots).
    bool SaveBackBuffer(const std::filesystem::path& path, std::string& error);

    // -debug: prints the D3D12 debug layer messages of the frame to stderr (once per message id).
    void DrainDebugMessages() { m_DebugMessages.Drain(); }
    const Benchmark::D3D12DebugMessages& DebugMessages() const { return m_DebugMessages; }

    uint64_t LastFrameDraws() const { return m_LastDraws; }
    uint64_t LastFrameIndices() const { return m_LastIndices; }
    uint32_t TextureCount() const { return uint32_t(m_Textures.size()); }
    ID3D12Device* Device() const { return m_Device.Get(); }

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
        PipelineMirrorStencil,        // + stencil bit index 0..7: writes the mirror's stencil bit
        PipelineMirrorClearDepth = PipelineMirrorStencil + 8,
        PipelineMirroredScene = PipelineMirrorClearDepth + 8,
        PipelineMirrorSurface = PipelineMirroredScene + 8,
        PipelineCount = PipelineMirrorSurface + 8
    };

    struct MeshBuffers
    {
        ComPtr<ID3D12Resource> vertexBuffer;
        ComPtr<ID3D12Resource> indexBuffer;
        D3D12_VERTEX_BUFFER_VIEW vbv = {};
        D3D12_INDEX_BUFFER_VIEW ibv = {};
    };

    struct Material
    {
        D3D12_GPU_DESCRIPTOR_HANDLE diffuse = {};
        D3D12_GPU_DESCRIPTOR_HANDLE normal = {};
    };

    struct Subset
    {
        D3D12_PRIMITIVE_TOPOLOGY topology;
        uint32_t material;
        uint32_t indexCount, indexStart, vertexStart;
    };

    struct MeshDraw
    {
        uint32_t buffers; // index into m_MeshBuffers (one sdkmesh mesh)
        uint32_t firstSubset, subsetCount;
    };

    struct ListStats
    {
        uint64_t draws = 0, indices = 0;
    };

    struct Frame
    {
        ComPtr<ID3D12CommandAllocator> allocators[MtScene::kNumPasses][MtScene::kMaxThreads];
        UploadBufferStack constants;
        uint64_t fenceValue = 0;
    };

    // Per-frame pass parameters, computed on the main thread before any list is recorded.
    struct FrameParams
    {
        MtScene::SceneDraw shadow[MtScene::kNumShadows];
        MtScene::MirrorPass mirror[MtScene::kNumMirrors];
        MtScene::SceneDraw direct;
        MtScene::CbPerLight lights;
        uint32_t backBuffer = 0;
    };

    void CreateDevice(const RendererDesc& desc);
    void CreateSwapChain(const RendererDesc& desc);
    void CreateSizeDependentResources(uint32_t width, uint32_t height);
    void ReleaseSizeDependentResources();
    void CreateShadowMaps();
    void CreateRootSignature();
    void CreatePipelines();
    void CreateFrameResources();
    void LoadModel(const MtScene::SdkMesh& mesh);
    void CreateMirrorGeometry(ID3D12GraphicsCommandList* uploadList, std::vector<ComPtr<ID3D12Resource>>& uploads);
    ComPtr<ID3D12Resource> CreateTexture(ID3D12GraphicsCommandList* uploadList, std::vector<ComPtr<ID3D12Resource>>& uploads,
                                         const MtScene::DdsImage& image, bool srgb, const char* name);
    ComPtr<ID3D12Resource> CreateSolidTexture(ID3D12GraphicsCommandList* uploadList, std::vector<ComPtr<ID3D12Resource>>& uploads,
                                              uint32_t rgba, const char* name);
    uint32_t AddShaderResourceView(ID3D12Resource* texture, DXGI_FORMAT format, uint32_t mipLevels);

    ID3D12GraphicsCommandList* List(int pass, int chunk) { return m_Lists[pass][chunk].Get(); }
    void RenderScene(ID3D12GraphicsCommandList* list, const MtScene::SceneDraw& draw, int chunk, int chunkCount, ListStats& stats);
    void DrawMirrorQuad(ID3D12GraphicsCommandList* list, int pipeline, int mirror, const MtScene::CbPerObject& object,
                        const MtScene::CbPerScene& scene, uint8_t stencilRef, const RECT& scissor);
    void SetViewportAndScissor(ID3D12GraphicsCommandList* list, uint32_t width, uint32_t height, const RECT& scissor);

    D3D12_CPU_DESCRIPTOR_HANDLE BackBufferRtv() const;
    D3D12_CPU_DESCRIPTOR_HANDLE DepthDsv() const;
    D3D12_CPU_DESCRIPTOR_HANDLE ShadowDsv(int shadow) const;

    MtScene::Mode m_Mode;
    uint32_t m_Threads;
    HWND m_Window = nullptr;
    bool m_AllowTearing = false;
    uint32_t m_Width = 0, m_Height = 0;

    ComPtr<IDXGIFactory4> m_Factory;
    ComPtr<ID3D12Device> m_Device;
    ComPtr<ID3D12CommandQueue> m_Queue;
    ComPtr<IDXGISwapChain3> m_SwapChain;
    ComPtr<ID3D12Fence> m_Fence;
    Benchmark::D3D12DebugMessages m_DebugMessages; // -debug only
    HANDLE m_FenceEvent = nullptr;
    uint64_t m_FenceValue = 0;

    ComPtr<ID3D12DescriptorHeap> m_RtvHeap;      // swap chain buffers
    ComPtr<ID3D12DescriptorHeap> m_DsvHeap;      // depth buffer + shadow maps
    ComPtr<ID3D12DescriptorHeap> m_SrvHeap;      // shader visible: shadow maps, then the textures
    uint32_t m_RtvSize = 0, m_DsvSize = 0, m_SrvSize = 0;
    uint32_t m_SrvCount = 0;

    ComPtr<ID3D12Resource> m_BackBuffers[kSwapChainBuffers];
    ComPtr<ID3D12Resource> m_DepthBuffer;
    ComPtr<ID3D12Resource> m_ShadowMaps[MtScene::kNumShadows];

    ComPtr<ID3D12RootSignature> m_RootSignature;
    ComPtr<ID3D12PipelineState> m_Pipelines[PipelineCount];

    std::vector<MeshBuffers> m_MeshBuffers;
    std::vector<Subset> m_Subsets;
    std::vector<MeshDraw> m_RenderOrder; // draw order of the sample (frame tree)
    std::vector<Material> m_Materials;
    std::vector<ComPtr<ID3D12Resource>> m_Textures;
    ComPtr<ID3D12Resource> m_MirrorVertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_MirrorVbv[MtScene::kNumMirrors] = {};

    Frame m_Frames[kFramesInFlight];
    uint32_t m_FrameIndex = 0;
    uint64_t m_FrameCounter = 0;
    ComPtr<ID3D12GraphicsCommandList> m_Lists[MtScene::kNumPasses][MtScene::kMaxThreads];
    ListStats m_ListStats[MtScene::kNumPasses][MtScene::kMaxThreads];
    std::vector<ID3D12CommandList*> m_ExecuteScratch;

    FrameParams m_Params;
    std::unique_ptr<MtScene::WorkerPool> m_Workers;
    uint64_t m_LastDraws = 0, m_LastIndices = 0;
};

} // namespace MtNative
