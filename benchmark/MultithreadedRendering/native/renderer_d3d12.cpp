#include "renderer_d3d12.h"

#include "frame_scheduler.h"

#include <algorithm>
#include <cstring>

// fxc output (CMake custom commands): g_VSMain, g_PSMain
#include "mt_scene_vs.h"
#include "mt_scene_ps.h"

namespace MtNative
{

using namespace MtScene;

namespace
{

constexpr DXGI_FORMAT kSwapChainFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kRenderTargetFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; // the sample's sRGB back buffer view
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
constexpr DXGI_FORMAT kShadowResourceFormat = DXGI_FORMAT_R32_TYPELESS;
constexpr DXGI_FORMAT kShadowDsvFormat = DXGI_FORMAT_D32_FLOAT;
constexpr DXGI_FORMAT kShadowSrvFormat = DXGI_FORMAT_R32_FLOAT;

// Root parameters of the sample (RootSignatureGenerator in CreatePSOs)
enum RootParam : UINT
{
    RootCbPerObject = 0, // b0
    RootCbPerLight,      // b1 (pixel)
    RootCbPerScene,      // b2
    RootTableDiffuse,    // t0 (pixel)
    RootTableNormal,     // t1 (pixel)
    RootTableShadow      // t2 (pixel)
};

// The two input layouts of the sample.
const D3D12_INPUT_ELEMENT_DESC kCompressedLayout[] = {
    { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "NORMAL", 0, DXGI_FORMAT_R10G10B10A2_UNORM, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "TANGENT", 0, DXGI_FORMAT_R10G10B10A2_UNORM, 0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
};
const D3D12_INPUT_ELEMENT_DESC kUncompressedLayout[] = {
    { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
};

D3D12_BLEND_DESC DefaultBlend()
{
    D3D12_BLEND_DESC desc = {};
    desc.AlphaToCoverageEnable = FALSE;
    desc.IndependentBlendEnable = FALSE;
    for (auto& rt : desc.RenderTarget)
    {
        rt.BlendEnable = FALSE;
        rt.LogicOpEnable = FALSE;
        rt.SrcBlend = D3D12_BLEND_ONE;
        rt.DestBlend = D3D12_BLEND_ZERO;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ZERO;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rt.LogicOp = D3D12_LOGIC_OP_NOOP;
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    return desc;
}

D3D12_RASTERIZER_DESC DefaultRaster()
{
    D3D12_RASTERIZER_DESC desc = {};
    desc.FillMode = D3D12_FILL_MODE_SOLID;
    desc.CullMode = D3D12_CULL_MODE_BACK;
    desc.FrontCounterClockwise = FALSE;
    desc.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    desc.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    desc.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    desc.DepthClipEnable = TRUE;
    desc.MultisampleEnable = FALSE;
    desc.AntialiasedLineEnable = FALSE;
    desc.ForcedSampleCount = 0;
    desc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    return desc;
}

D3D12_DEPTH_STENCIL_DESC DefaultDepthStencil()
{
    D3D12_DEPTH_STENCIL_DESC desc = {};
    desc.DepthEnable = TRUE;
    desc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    desc.StencilEnable = FALSE;
    desc.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    desc.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    const D3D12_DEPTH_STENCILOP_DESC op = { D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS };
    desc.FrontFace = op;
    desc.BackFace = op;
    return desc;
}

D3D12_PRIMITIVE_TOPOLOGY Topology(uint32_t sdkmeshPrimitiveType)
{
    switch (sdkmeshPrimitiveType)
    {
        case SDKMESH_PT_TRIANGLE_LIST: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        case SDKMESH_PT_TRIANGLE_STRIP: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        case SDKMESH_PT_LINE_LIST: return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
        case SDKMESH_PT_LINE_STRIP: return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
        case SDKMESH_PT_POINT_LIST: return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
        case SDKMESH_PT_TRIANGLE_LIST_ADJ: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ;
        case SDKMESH_PT_TRIANGLE_STRIP_ADJ: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ;
        case SDKMESH_PT_LINE_LIST_ADJ: return D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ;
        case SDKMESH_PT_LINE_STRIP_ADJ: return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ;
        default: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
}

std::wstring Widen(const char* s)
{
    wchar_t buffer[MAX_PATH]{};
    MultiByteToWideChar(CP_ACP, 0, s, -1, buffer, MAX_PATH - 1);
    return buffer;
}

} // namespace

RendererD3D12::RendererD3D12(const RendererDesc& desc, const SdkMesh& mesh)
    : m_Mode(desc.mode)
    , m_Threads(std::clamp(desc.threads, 1u, kMaxThreads))
    , m_Window(desc.window)
    , m_AllowTearing(desc.allowTearing)
{
    CreateDevice(desc);
    CreateSwapChain(desc);

    // Descriptor heaps: the shader-visible heap holds the shadow map SRVs first (the sample copies the
    // model's descriptors behind them), then every texture.
    D3D12_DESCRIPTOR_HEAP_DESC heap = {};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap.NumDescriptors = kSwapChainBuffers;
    ThrowIfFailed(m_Device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&m_RtvHeap)), "CreateDescriptorHeap (RTV)");
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    heap.NumDescriptors = 1 + kNumShadows;
    ThrowIfFailed(m_Device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&m_DsvHeap)), "CreateDescriptorHeap (DSV)");
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = kNumShadows + 2 * mesh.NumMaterials() + 2;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(m_Device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&m_SrvHeap)), "CreateDescriptorHeap (SRV)");
    m_RtvSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_DsvSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    m_SrvSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    CreateShadowMaps();
    CreateSizeDependentResources(desc.width, desc.height);
    CreateRootSignature();
    CreatePipelines();
    CreateFrameResources();
    LoadModel(mesh);

    if (m_Threads > 1)
        m_Workers = std::make_unique<WorkerPool>(m_Threads - 1);
}

RendererD3D12::~RendererD3D12()
{
    WaitForGpu();
    DrainDebugMessages();
    m_Workers.reset();
    if (m_FenceEvent) CloseHandle(m_FenceEvent);
}

void RendererD3D12::CreateDevice(const RendererDesc& desc)
{
    UINT factoryFlags = 0;
    if (desc.debugLayer)
    {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        {
            debug->EnableDebugLayer();
            factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }
    ThrowIfFailed(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_Factory)), "CreateDXGIFactory2");

    ComPtr<IDXGIAdapter1> adapter = desc.adapter;
    if (!adapter)
    {
        // The first hardware adapter that supports D3D12 (the sample asked for the high-performance one).
        ComPtr<IDXGIFactory6> factory6;
        if (SUCCEEDED(m_Factory.As(&factory6)))
        {
            for (UINT i = 0; SUCCEEDED(factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter))); ++i)
            {
                DXGI_ADAPTER_DESC1 d{};
                adapter->GetDesc1(&d);
                if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
                    break;
                adapter = nullptr;
            }
        }
        if (!adapter)
        {
            for (UINT i = 0; SUCCEEDED(m_Factory->EnumAdapters1(i, &adapter)); ++i)
            {
                DXGI_ADAPTER_DESC1 d{};
                adapter->GetDesc1(&d);
                if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
                    break;
                adapter = nullptr;
            }
        }
    }
    ThrowIfFailed(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_Device)), "D3D12CreateDevice");
    if (desc.debugLayer)
        m_DebugMessages.Init(m_Device.Get()); // the messages are read per frame (DrainDebugMessages) and printed

    D3D12_COMMAND_QUEUE_DESC queue = {};
    queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ThrowIfFailed(m_Device->CreateCommandQueue(&queue, IID_PPV_ARGS(&m_Queue)), "CreateCommandQueue");
    ThrowIfFailed(m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_Fence)), "CreateFence");
    m_FenceEvent = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);

    Benchmark::SetActualAdapterOrFail(Benchmark::LuidToU64(m_Device->GetAdapterLuid()));
}

void RendererD3D12::CreateSwapChain(const RendererDesc& desc)
{
    DXGI_SWAP_CHAIN_DESC1 sc = {};
    sc.Width = desc.width;
    sc.Height = desc.height;
    sc.Format = kSwapChainFormat;
    sc.SampleDesc.Count = 1;
    sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc.BufferCount = kSwapChainBuffers;
    sc.Scaling = DXGI_SCALING_STRETCH;
    sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sc.Flags = m_AllowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

    ComPtr<IDXGISwapChain1> swapChain1;
    ThrowIfFailed(m_Factory->CreateSwapChainForHwnd(m_Queue.Get(), m_Window, &sc, nullptr, nullptr, &swapChain1), "CreateSwapChainForHwnd");
    ThrowIfFailed(m_Factory->MakeWindowAssociation(m_Window, DXGI_MWA_NO_ALT_ENTER), "MakeWindowAssociation");
    ThrowIfFailed(swapChain1.As(&m_SwapChain), "IDXGISwapChain3");
}

void RendererD3D12::CreateSizeDependentResources(uint32_t width, uint32_t height)
{
    m_Width = std::max(1u, width);
    m_Height = std::max(1u, height);

    for (uint32_t i = 0; i < kSwapChainBuffers; ++i)
    {
        ThrowIfFailed(m_SwapChain->GetBuffer(i, IID_PPV_ARGS(&m_BackBuffers[i])), "IDXGISwapChain::GetBuffer");
        D3D12_RENDER_TARGET_VIEW_DESC rtv = {};
        rtv.Format = kRenderTargetFormat;
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        m_Device->CreateRenderTargetView(m_BackBuffers[i].Get(), &rtv, Offset(m_RtvHeap->GetCPUDescriptorHandleForHeapStart(), i, m_RtvSize));
    }

    D3D12_CLEAR_VALUE clear = {};
    clear.Format = kDepthFormat;
    clear.DepthStencil.Depth = 1.0f;
    clear.DepthStencil.Stencil = 0;
    const D3D12_HEAP_PROPERTIES heap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    const D3D12_RESOURCE_DESC depth = Tex2DDesc(kDepthFormat, m_Width, m_Height, 1, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    ThrowIfFailed(m_Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &depth, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
                                                    IID_PPV_ARGS(&m_DepthBuffer)),
                  "CreateCommittedResource (depth buffer)");
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
    dsv.Format = kDepthFormat;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    m_Device->CreateDepthStencilView(m_DepthBuffer.Get(), &dsv, DepthDsv());
}

void RendererD3D12::ReleaseSizeDependentResources()
{
    for (auto& b : m_BackBuffers) b.Reset();
    m_DepthBuffer.Reset();
}

void RendererD3D12::Resize(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0 || (width == m_Width && height == m_Height)) return;
    WaitForGpu();
    ReleaseSizeDependentResources();
    ThrowIfFailed(m_SwapChain->ResizeBuffers(kSwapChainBuffers, width, height, kSwapChainFormat,
                                             m_AllowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0),
                  "IDXGISwapChain::ResizeBuffers");
    CreateSizeDependentResources(width, height);
}

void RendererD3D12::CreateShadowMaps()
{
    D3D12_CLEAR_VALUE clear = {};
    clear.Format = kShadowDsvFormat;
    clear.DepthStencil.Depth = 1.0f;
    const D3D12_HEAP_PROPERTIES heap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    const D3D12_RESOURCE_DESC desc = Tex2DDesc(kShadowResourceFormat, kShadowMapSize, kShadowMapSize, 1, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    for (int i = 0; i < kNumShadows; ++i)
    {
        ThrowIfFailed(m_Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
                                                        IID_PPV_ARGS(&m_ShadowMaps[i])),
                      "CreateCommittedResource (shadow map)");
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
        dsv.Format = kShadowDsvFormat;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        m_Device->CreateDepthStencilView(m_ShadowMaps[i].Get(), &dsv, ShadowDsv(i));
        AddShaderResourceView(m_ShadowMaps[i].Get(), kShadowSrvFormat, 1); // SRV slots 0..kNumShadows-1
    }
}

uint32_t RendererD3D12::AddShaderResourceView(ID3D12Resource* texture, DXGI_FORMAT format, uint32_t mipLevels)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = mipLevels;
    const uint32_t index = m_SrvCount++;
    m_Device->CreateShaderResourceView(texture, &srv, Offset(m_SrvHeap->GetCPUDescriptorHandleForHeapStart(), index, m_SrvSize));
    return index;
}

void RendererD3D12::CreateRootSignature()
{
    D3D12_DESCRIPTOR_RANGE1 ranges[3] = {};
    for (UINT i = 0; i < 3; ++i)
    {
        ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[i].NumDescriptors = 1;
        ranges[i].BaseShaderRegister = i;
        ranges[i].RegisterSpace = 0;
        ranges[i].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
        ranges[i].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    }

    D3D12_ROOT_PARAMETER1 params[6] = {};
    auto cbv = [](D3D12_ROOT_PARAMETER1& p, UINT reg, D3D12_SHADER_VISIBILITY vis) {
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p.Descriptor.ShaderRegister = reg;
        p.Descriptor.RegisterSpace = 0;
        p.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
        p.ShaderVisibility = vis;
    };
    auto table = [](D3D12_ROOT_PARAMETER1& p, const D3D12_DESCRIPTOR_RANGE1* range) {
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p.DescriptorTable.NumDescriptorRanges = 1;
        p.DescriptorTable.pDescriptorRanges = range;
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    };
    cbv(params[RootCbPerObject], 0, D3D12_SHADER_VISIBILITY_ALL);
    cbv(params[RootCbPerLight], 1, D3D12_SHADER_VISIBILITY_PIXEL);
    cbv(params[RootCbPerScene], 2, D3D12_SHADER_VISIBILITY_ALL);
    table(params[RootTableDiffuse], &ranges[0]);
    table(params[RootTableNormal], &ranges[1]);
    table(params[RootTableShadow], &ranges[2]);

    // s0: point clamp, s1: linear (clamp, as the sample declared it although the shader calls it "wrap")
    D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
    for (UINT i = 0; i < 2; ++i)
    {
        samplers[i].Filter = i == 0 ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplers[i].AddressU = samplers[i].AddressV = samplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].MaxAnisotropy = 16;
        samplers[i].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        samplers[i].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
        samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[i].ShaderRegister = i;
        samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc = {};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters = 6;
    desc.Desc_1_1.pParameters = params;
    desc.Desc_1_1.NumStaticSamplers = 2;
    desc.Desc_1_1.pStaticSamplers = samplers;
    desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> blob, error;
    const HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, &blob, &error);
    if (FAILED(hr) && error)
        fprintf(stderr, "root signature: %s\n", static_cast<const char*>(error->GetBufferPointer()));
    ThrowIfFailed(hr, "D3D12SerializeVersionedRootSignature");
    ThrowIfFailed(m_Device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_RootSignature)),
                  "CreateRootSignature");
}

void RendererD3D12::CreatePipelines()
{
    const D3D12_SHADER_BYTECODE vs = { g_VSMain, sizeof(g_VSMain) };
    const D3D12_SHADER_BYTECODE ps = { g_PSMain, sizeof(g_PSMain) };

    auto base = [&](bool withPixelShader, const D3D12_INPUT_ELEMENT_DESC* layout, UINT layoutCount) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
        d.pRootSignature = m_RootSignature.Get();
        d.VS = vs;
        if (withPixelShader) d.PS = ps;
        d.BlendState = DefaultBlend();
        d.SampleMask = UINT_MAX;
        d.RasterizerState = DefaultRaster();
        d.DepthStencilState = DefaultDepthStencil();
        d.InputLayout = { layout, layoutCount };
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        // The sample declared its vertex-only pipelines with NumRenderTargets = 0 while the back buffer
        // stayed bound; D3D12 calls that a render target format mismatch and may drop the draws (it did
        // here). They declare the bound format and write no colour instead, which is what the sample meant.
        d.NumRenderTargets = 1;
        d.RTVFormats[0] = kRenderTargetFormat;
        if (!withPixelShader) d.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
        d.DSVFormat = kDepthFormat;
        d.SampleDesc.Count = 1;
        return d;
    };
    auto create = [&](int index, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) {
        ThrowIfFailed(m_Device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&m_Pipelines[index])), "CreateGraphicsPipelineState");
    };

    // Shadow map: depth only, biased.
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = base(false, kCompressedLayout, UINT(std::size(kCompressedLayout)));
        d.BlendState.RenderTarget[0].BlendEnable = TRUE; // the sample's "no color write"
        d.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
        d.RasterizerState.DepthBias = 1 << 8;
        d.RasterizerState.SlopeScaledDepthBias = 1.E-4f;
        d.RasterizerState.DepthBiasClamp = 1.E-2f;
        d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        d.NumRenderTargets = 0; // rendered with no render target bound
        d.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
        d.DSVFormat = kShadowDsvFormat;
        create(PipelineShadow, d);
    }

    // The direct view.
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = base(true, kCompressedLayout, UINT(std::size(kCompressedLayout)));
        d.NumRenderTargets = 1;
        d.RTVFormats[0] = kRenderTargetFormat;
        create(PipelineDirect, d);
    }

    // Mirrored area stencil write: depth test, no depth write, stencil := bit.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC stencilWrite = base(false, kUncompressedLayout, UINT(std::size(kUncompressedLayout)));
    stencilWrite.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    stencilWrite.DepthStencilState.StencilEnable = TRUE;
    stencilWrite.DepthStencilState.FrontFace = { D3D12_STENCIL_OP_REPLACE, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_REPLACE, D3D12_COMPARISON_FUNC_ALWAYS };
    stencilWrite.DepthStencilState.BackFace = stencilWrite.DepthStencilState.FrontFace;

    // Clear the depth inside the stencil area (depth written where the stencil bit matches).
    D3D12_GRAPHICS_PIPELINE_STATE_DESC clearDepth = base(false, kUncompressedLayout, UINT(std::size(kUncompressedLayout)));
    clearDepth.DepthStencilState.StencilEnable = TRUE;
    clearDepth.DepthStencilState.FrontFace = { D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_EQUAL };
    clearDepth.DepthStencilState.BackFace = { D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_NEVER };

    // The reflected scene: stencil test, depth test and write, flipped winding.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC mirrored = base(true, kCompressedLayout, UINT(std::size(kCompressedLayout)));
    mirrored.RasterizerState.FrontCounterClockwise = TRUE;
    mirrored.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    mirrored.DepthStencilState.StencilEnable = TRUE;
    mirrored.DepthStencilState.FrontFace = { D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_EQUAL };
    mirrored.DepthStencilState.BackFace = { D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_NEVER };
    mirrored.NumRenderTargets = 1;
    mirrored.RTVFormats[0] = kRenderTargetFormat;

    // The mirror surface: overwrite depth, clear the stencil bit.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC surface = base(false, kUncompressedLayout, UINT(std::size(kUncompressedLayout)));
    surface.DepthStencilState.StencilEnable = TRUE;
    surface.DepthStencilState.FrontFace = { D3D12_STENCIL_OP_ZERO, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_ZERO, D3D12_COMPARISON_FUNC_EQUAL };
    surface.DepthStencilState.BackFace = { D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_NEVER };

    for (int i = 0; i < 8; ++i)
    {
        const UINT8 bit = UINT8(1u << i);
        stencilWrite.DepthStencilState.StencilReadMask = 0;
        stencilWrite.DepthStencilState.StencilWriteMask = bit;
        clearDepth.DepthStencilState.StencilReadMask = bit;
        clearDepth.DepthStencilState.StencilWriteMask = 0;
        mirrored.DepthStencilState.StencilReadMask = bit;
        mirrored.DepthStencilState.StencilWriteMask = 0;
        surface.DepthStencilState.StencilReadMask = bit;
        surface.DepthStencilState.StencilWriteMask = bit;
        create(PipelineMirrorStencil + i, stencilWrite);
        create(PipelineMirrorClearDepth + i, clearDepth);
        create(PipelineMirroredScene + i, mirrored);
        create(PipelineMirrorSurface + i, surface);
    }
}

void RendererD3D12::CreateFrameResources()
{
    const uint32_t chunks = m_Mode == Mode::PerChunk ? m_Threads : 1;
    for (uint32_t f = 0; f < kFramesInFlight; ++f)
    {
        Frame& frame = m_Frames[f];
        frame.constants.Initialize(m_Device.Get(), 1 << 14); // the sample's 16 KB blocks
        for (int pass = 0; pass < kNumPasses; ++pass)
            for (uint32_t chunk = 0; chunk < m_Threads; ++chunk)
            {
                ThrowIfFailed(m_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame.allocators[pass][chunk])),
                              "CreateCommandAllocator");
                if (f == 0)
                {
                    ThrowIfFailed(m_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frame.allocators[pass][chunk].Get(), nullptr,
                                                              IID_PPV_ARGS(&m_Lists[pass][chunk])),
                                  "CreateCommandList");
                    m_Lists[pass][chunk]->Close();
                }
            }
    }
    (void)chunks;
}

ComPtr<ID3D12Resource> RendererD3D12::CreateTexture(ID3D12GraphicsCommandList* uploadList, std::vector<ComPtr<ID3D12Resource>>& uploads,
                                                    const DdsImage& image, bool srgb, const char* name)
{
    const DXGI_FORMAT format = srgb ? DdsImage::MakeSRGB(image.Format()) : image.Format();
    const D3D12_RESOURCE_DESC desc = Tex2DDesc(format, image.Width(), image.Height(), UINT16(image.MipLevels()));
    const D3D12_HEAP_PROPERTIES defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> texture;
    ThrowIfFailed(m_Device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&texture)),
                  name);

    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(image.MipLevels());
    std::vector<UINT> rows(image.MipLevels());
    std::vector<UINT64> rowSizes(image.MipLevels());
    UINT64 totalBytes = 0;
    m_Device->GetCopyableFootprints(&desc, 0, image.MipLevels(), 0, footprints.data(), rows.data(), rowSizes.data(), &totalBytes);

    const D3D12_HEAP_PROPERTIES uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_RESOURCE_DESC uploadDesc = BufferDesc(totalBytes);
    ComPtr<ID3D12Resource> upload;
    ThrowIfFailed(m_Device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(&upload)),
                  "CreateCommittedResource (texture upload)");
    uint8_t* mapped = nullptr;
    ThrowIfFailed(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "Map (texture upload)");
    for (uint32_t mip = 0; mip < image.MipLevels(); ++mip)
    {
        const DdsSubresource& src = image.Mip(mip);
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp = footprints[mip];
        const size_t rowBytes = std::min<size_t>(src.rowPitch, size_t(rowSizes[mip]));
        for (UINT row = 0; row < rows[mip] && row < src.rows; ++row)
            memcpy(mapped + fp.Offset + UINT64(row) * fp.Footprint.RowPitch, src.data + size_t(row) * src.rowPitch, rowBytes);

        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = texture.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = mip;
        D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
        srcLoc.pResource = upload.Get();
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLoc.PlacedFootprint = fp;
        uploadList->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);
    }
    upload->Unmap(0, nullptr);
    uploads.push_back(upload);

    const D3D12_RESOURCE_BARRIER barrier = Transition(texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    uploadList->ResourceBarrier(1, &barrier);
    return texture;
}

ComPtr<ID3D12Resource> RendererD3D12::CreateSolidTexture(ID3D12GraphicsCommandList* uploadList, std::vector<ComPtr<ID3D12Resource>>& uploads,
                                                         uint32_t rgba, const char* name)
{
    const D3D12_RESOURCE_DESC desc = Tex2DDesc(DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 1);
    const D3D12_HEAP_PROPERTIES defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> texture;
    ThrowIfFailed(m_Device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&texture)),
                  name);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT64 total = 0;
    m_Device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
    const D3D12_HEAP_PROPERTIES uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_RESOURCE_DESC uploadDesc = BufferDesc(total);
    ComPtr<ID3D12Resource> upload;
    ThrowIfFailed(m_Device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(&upload)),
                  "CreateCommittedResource (solid texture upload)");
    uint8_t* mapped = nullptr;
    ThrowIfFailed(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "Map");
    memcpy(mapped + fp.Offset, &rgba, 4);
    upload->Unmap(0, nullptr);
    uploads.push_back(upload);
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = texture.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = fp;
    uploadList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    const D3D12_RESOURCE_BARRIER barrier = Transition(texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    uploadList->ResourceBarrier(1, &barrier);
    return texture;
}

void RendererD3D12::CreateMirrorGeometry(ID3D12GraphicsCommandList* uploadList, std::vector<ComPtr<ID3D12Resource>>& uploads)
{
    // The four mirror quads share one vertex buffer (the sample's CreateMirrorModels); the vertex
    // data comes from the scene description.
    MtScene::Scene scene;
    MirrorVertex vertices[4 * kNumMirrors];
    for (int i = 0; i < kNumMirrors; ++i)
        memcpy(&vertices[4 * i], scene.GetMirror(i).vertices, sizeof(MirrorVertex) * 4);

    const D3D12_HEAP_PROPERTIES defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    const D3D12_HEAP_PROPERTIES uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_RESOURCE_DESC desc = BufferDesc(sizeof(vertices));
    ThrowIfFailed(m_Device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&m_MirrorVertexBuffer)),
                  "CreateCommittedResource (mirror vertices)");
    ComPtr<ID3D12Resource> upload;
    ThrowIfFailed(m_Device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(&upload)),
                  "CreateCommittedResource (mirror upload)");
    void* mapped = nullptr;
    ThrowIfFailed(upload->Map(0, nullptr, &mapped), "Map");
    memcpy(mapped, vertices, sizeof(vertices));
    upload->Unmap(0, nullptr);
    uploads.push_back(upload);
    uploadList->CopyBufferRegion(m_MirrorVertexBuffer.Get(), 0, upload.Get(), 0, sizeof(vertices));
    const D3D12_RESOURCE_BARRIER barrier =
        Transition(m_MirrorVertexBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    uploadList->ResourceBarrier(1, &barrier);

    for (int i = 0; i < kNumMirrors; ++i)
    {
        m_MirrorVbv[i].BufferLocation = m_MirrorVertexBuffer->GetGPUVirtualAddress() + UINT64(i) * sizeof(MirrorVertex) * 4;
        m_MirrorVbv[i].SizeInBytes = sizeof(MirrorVertex) * 4;
        m_MirrorVbv[i].StrideInBytes = sizeof(MirrorVertex);
    }
}

void RendererD3D12::LoadModel(const SdkMesh& mesh)
{
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ThrowIfFailed(m_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "CreateCommandAllocator (upload)");
    ThrowIfFailed(m_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "CreateCommandList (upload)");
    std::vector<ComPtr<ID3D12Resource>> uploads;

    // One default-heap buffer per sdkmesh vertex / index buffer, as DXUT's loader creates them.
    const D3D12_HEAP_PROPERTIES defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    const D3D12_HEAP_PROPERTIES uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    auto createBuffer = [&](const void* data, uint64_t size, D3D12_RESOURCE_STATES finalState, const char* name) {
        const D3D12_RESOURCE_DESC desc = BufferDesc(size);
        ComPtr<ID3D12Resource> buffer, upload;
        ThrowIfFailed(m_Device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                        IID_PPV_ARGS(&buffer)),
                      name);
        ThrowIfFailed(m_Device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                        IID_PPV_ARGS(&upload)),
                      "CreateCommittedResource (buffer upload)");
        void* mapped = nullptr;
        ThrowIfFailed(upload->Map(0, nullptr, &mapped), "Map");
        memcpy(mapped, data, size_t(size));
        upload->Unmap(0, nullptr);
        uploads.push_back(upload);
        list->CopyBufferRegion(buffer.Get(), 0, upload.Get(), 0, size);
        const D3D12_RESOURCE_BARRIER barrier = Transition(buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, finalState);
        list->ResourceBarrier(1, &barrier);
        return buffer;
    };

    std::vector<ComPtr<ID3D12Resource>> vertexBuffers(mesh.NumVertexBuffers()), indexBuffers(mesh.NumIndexBuffers());
    for (uint32_t i = 0; i < mesh.NumVertexBuffers(); ++i)
        vertexBuffers[i] = createBuffer(mesh.VertexData(i), mesh.VertexBuffer(i).SizeBytes, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, "vertex buffer");
    for (uint32_t i = 0; i < mesh.NumIndexBuffers(); ++i)
        indexBuffers[i] = createBuffer(mesh.IndexData(i), mesh.IndexBuffer(i).SizeBytes, D3D12_RESOURCE_STATE_INDEX_BUFFER, "index buffer");

    m_MeshBuffers.resize(mesh.NumMeshes());
    for (uint32_t i = 0; i < mesh.NumMeshes(); ++i)
    {
        const SdkMeshMesh& m = mesh.Mesh(i);
        MeshBuffers& b = m_MeshBuffers[i];
        const SdkMeshVertexBufferHeader& vb = mesh.VertexBuffer(m.VertexBuffers[0]);
        const SdkMeshIndexBufferHeader& ib = mesh.IndexBuffer(m.IndexBuffer);
        b.vertexBuffer = vertexBuffers[m.VertexBuffers[0]];
        b.indexBuffer = indexBuffers[m.IndexBuffer];
        b.vbv.BufferLocation = b.vertexBuffer->GetGPUVirtualAddress();
        b.vbv.SizeInBytes = UINT(vb.SizeBytes);
        b.vbv.StrideInBytes = UINT(vb.StrideBytes);
        b.ibv.BufferLocation = b.indexBuffer->GetGPUVirtualAddress();
        b.ibv.SizeInBytes = UINT(ib.SizeBytes);
        b.ibv.Format = ib.IndexType == SDKMESH_IT_32BIT ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    }

    // Draw order and subsets.
    for (uint32_t meshIndex : mesh.RenderOrder())
    {
        const SdkMeshMesh& m = mesh.Mesh(meshIndex);
        MeshDraw draw;
        draw.buffers = meshIndex;
        draw.firstSubset = uint32_t(m_Subsets.size());
        draw.subsetCount = m.NumSubsets;
        for (uint32_t s = 0; s < m.NumSubsets; ++s)
        {
            const SdkMeshSubset& subset = mesh.Subset(m, s);
            m_Subsets.push_back({ Topology(subset.PrimitiveType), subset.MaterialID, uint32_t(subset.IndexCount), uint32_t(subset.IndexStart),
                                  uint32_t(subset.VertexStart) });
        }
        m_RenderOrder.push_back(draw);
    }

    // Textures: diffuse maps as sRGB, normal maps linear (DXUT's LoadMaterials). Missing files get a
    // 1x1 fallback so that every material binds something.
    const ComPtr<ID3D12Resource> whiteTexture = CreateSolidTexture(list.Get(), uploads, 0xffffffffu, "white");
    const ComPtr<ID3D12Resource> flatNormal = CreateSolidTexture(list.Get(), uploads, 0xffff8080u, "flat normal");
    const D3D12_GPU_DESCRIPTOR_HANDLE whiteSrv = Offset(m_SrvHeap->GetGPUDescriptorHandleForHeapStart(), AddShaderResourceView(whiteTexture.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 1), m_SrvSize);
    const D3D12_GPU_DESCRIPTOR_HANDLE flatSrv = Offset(m_SrvHeap->GetGPUDescriptorHandleForHeapStart(), AddShaderResourceView(flatNormal.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 1), m_SrvSize);
    m_Textures.push_back(whiteTexture);
    m_Textures.push_back(flatNormal);

    struct Loaded { std::wstring name; bool srgb; D3D12_GPU_DESCRIPTOR_HANDLE srv; };
    std::vector<Loaded> cache;
    auto load = [&](const char* fileName, bool srgb, D3D12_GPU_DESCRIPTOR_HANDLE fallback) {
        if (!fileName[0]) return fallback;
        const std::wstring name = Widen(fileName);
        for (const Loaded& l : cache)
            if (l.name == name && l.srgb == srgb) return l.srv;
        DdsImage image;
        std::string error;
        if (!image.Load(mesh.Directory() / name, error))
        {
            fprintf(stderr, "warning: %s\n", error.c_str());
            cache.push_back({ name, srgb, fallback });
            return fallback;
        }
        ComPtr<ID3D12Resource> texture = CreateTexture(list.Get(), uploads, image, srgb, fileName);
        const DXGI_FORMAT viewFormat = srgb ? DdsImage::MakeSRGB(image.Format()) : image.Format();
        const D3D12_GPU_DESCRIPTOR_HANDLE srv =
            Offset(m_SrvHeap->GetGPUDescriptorHandleForHeapStart(), AddShaderResourceView(texture.Get(), viewFormat, image.MipLevels()), m_SrvSize);
        m_Textures.push_back(texture);
        cache.push_back({ name, srgb, srv });
        return srv;
    };
    m_Materials.resize(mesh.NumMaterials());
    for (uint32_t i = 0; i < mesh.NumMaterials(); ++i)
    {
        const SdkMeshMaterial& mat = mesh.Material(i);
        m_Materials[i].diffuse = load(mat.DiffuseTexture, true, whiteSrv);
        m_Materials[i].normal = load(mat.NormalTexture, false, flatSrv);
    }

    CreateMirrorGeometry(list.Get(), uploads);

    ThrowIfFailed(list->Close(), "Close (upload)");
    ID3D12CommandList* lists[] = { list.Get() };
    m_Queue->ExecuteCommandLists(1, lists);
    WaitForGpu();
}

bool RendererD3D12::SaveBackBuffer(const std::filesystem::path& path, std::string& error)
{
    WaitForGpu();
    ID3D12Resource* backBuffer = m_BackBuffers[m_Params.backBuffer].Get(); // the frame rendered last
    const D3D12_RESOURCE_DESC desc = backBuffer->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 total = 0;
    m_Device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);

    const D3D12_HEAP_PROPERTIES readbackHeap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
    const D3D12_RESOURCE_DESC bufferDesc = BufferDesc(total);
    ComPtr<ID3D12Resource> readback;
    ThrowIfFailed(m_Device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&readback)),
                  "CreateCommittedResource (readback)");

    ID3D12CommandAllocator* allocator = m_Frames[m_FrameIndex].allocators[0][0].Get();
    ID3D12GraphicsCommandList* list = List(0, 0);
    ThrowIfFailed(allocator->Reset(), "Reset (readback)");
    ThrowIfFailed(list->Reset(allocator, nullptr), "Reset (readback list)");
    D3D12_RESOURCE_BARRIER toCopy = Transition(backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &toCopy);
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = backBuffer;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER toPresent = Transition(backBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    list->ResourceBarrier(1, &toPresent);
    ThrowIfFailed(list->Close(), "Close (readback)");
    ID3D12CommandList* lists[] = { list };
    m_Queue->ExecuteCommandLists(1, lists);
    WaitForGpu();

    void* mapped = nullptr;
    ThrowIfFailed(readback->Map(0, nullptr, &mapped), "Map (readback)");
    const bool ok = Benchmark::WriteBmp(path, static_cast<const uint8_t*>(mapped) + footprint.Offset, int(desc.Width), int(desc.Height),
                                        footprint.Footprint.RowPitch, false, error);
    const D3D12_RANGE none{ 0, 0 };
    readback->Unmap(0, &none);
    return ok;
}

void RendererD3D12::WaitForGpu()
{
    ThrowIfFailed(m_Queue->Signal(m_Fence.Get(), ++m_FenceValue), "Signal");
    ThrowIfFailed(m_Fence->SetEventOnCompletion(m_FenceValue, m_FenceEvent), "SetEventOnCompletion");
    WaitForSingleObject(m_FenceEvent, INFINITE);
}

D3D12_CPU_DESCRIPTOR_HANDLE RendererD3D12::BackBufferRtv() const
{
    return Offset(m_RtvHeap->GetCPUDescriptorHandleForHeapStart(), m_Params.backBuffer, m_RtvSize);
}
D3D12_CPU_DESCRIPTOR_HANDLE RendererD3D12::DepthDsv() const
{
    return m_DsvHeap->GetCPUDescriptorHandleForHeapStart();
}
D3D12_CPU_DESCRIPTOR_HANDLE RendererD3D12::ShadowDsv(int shadow) const
{
    return Offset(m_DsvHeap->GetCPUDescriptorHandleForHeapStart(), 1 + shadow, m_DsvSize);
}

// ---------------------------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------------------------

void RendererD3D12::RenderFrame(const Scene& scene, Benchmark::FrameTimes& times)
{
    m_FrameIndex = uint32_t(m_FrameCounter % kFramesInFlight);
    Frame& frame = m_Frames[m_FrameIndex];

    // Wait until the GPU finished the frame that used these allocators and constant buffers.
    {
        Benchmark::ScopedTimer wait(times.wait);
        if (m_Fence->GetCompletedValue() < frame.fenceValue)
        {
            ThrowIfFailed(m_Fence->SetEventOnCompletion(frame.fenceValue, m_FenceEvent), "SetEventOnCompletion");
            WaitForSingleObject(m_FenceEvent, INFINITE);
        }
    }
    frame.constants.Clear();

    // Everything the passes need, decided once on the main thread.
    {
        Benchmark::ScopedTimer update(times.update);
        for (int s = 0; s < kNumShadows; ++s) m_Params.shadow[s] = scene.ShadowPass(s);
        for (int m = 0; m < kNumMirrors; ++m) m_Params.mirror[m] = scene.MirrorPassParams(m);
        m_Params.direct = scene.DirectPass();
        scene.FillLightConstants(m_Params.lights);
        m_Params.backBuffer = m_SwapChain->GetCurrentBackBufferIndex();
        memset(m_ListStats, 0, sizeof(m_ListStats));
    }

    const uint32_t chunkCount = m_Mode == Mode::PerChunk ? m_Threads : 1;
    static WorkerPool noWorkers(0);
    RunFrame(*this, m_Mode, chunkCount, m_Workers ? *m_Workers : noWorkers, times);

    {
        Benchmark::ScopedTimer present(times.present);
        ThrowIfFailed(m_SwapChain->Present(0, m_AllowTearing ? DXGI_PRESENT_ALLOW_TEARING : 0), "Present");
    }
    ThrowIfFailed(m_Queue->Signal(m_Fence.Get(), ++m_FenceValue), "Signal");
    frame.fenceValue = m_FenceValue;
    ++m_FrameCounter;
    DrainDebugMessages();

    m_LastDraws = m_LastIndices = 0;
    for (int pass = 0; pass < kNumPasses; ++pass)
        for (uint32_t chunk = 0; chunk < m_Threads; ++chunk)
        {
            m_LastDraws += m_ListStats[pass][chunk].draws;
            m_LastIndices += m_ListStats[pass][chunk].indices;
        }
}

void RendererD3D12::OpenList(int pass, int chunk)
{
    ID3D12CommandAllocator* allocator = m_Frames[m_FrameIndex].allocators[pass][chunk].Get();
    ThrowIfFailed(allocator->Reset(), "ID3D12CommandAllocator::Reset");
    ThrowIfFailed(List(pass, chunk)->Reset(allocator, nullptr), "ID3D12GraphicsCommandList::Reset");
    ID3D12DescriptorHeap* heaps[] = { m_SrvHeap.Get() };
    List(pass, chunk)->SetDescriptorHeaps(1, heaps);
}

void RendererD3D12::CloseList(int pass, int chunk)
{
    ThrowIfFailed(List(pass, chunk)->Close(), "ID3D12GraphicsCommandList::Close");
}

void RendererD3D12::ExecuteLists(int pass, int chunkCount)
{
    m_ExecuteScratch.clear();
    for (int chunk = 0; chunk < chunkCount; ++chunk)
        m_ExecuteScratch.push_back(List(pass, chunk));
    m_Queue->ExecuteCommandLists(UINT(m_ExecuteScratch.size()), m_ExecuteScratch.data());
}

void RendererD3D12::ExecuteAllPasses()
{
    m_ExecuteScratch.clear();
    for (int pass = 0; pass < kNumPasses; ++pass)
        m_ExecuteScratch.push_back(List(pass, 0));
    m_Queue->ExecuteCommandLists(UINT(m_ExecuteScratch.size()), m_ExecuteScratch.data());
}

void RendererD3D12::SetViewportAndScissor(ID3D12GraphicsCommandList* list, uint32_t width, uint32_t height, const RECT& scissor)
{
    D3D12_VIEWPORT viewport = { 0.f, 0.f, float(width), float(height), 0.f, 1.f };
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
}

void RendererD3D12::RecordPass(int pass, int chunk, int chunkCount, int listPass)
{
    ID3D12GraphicsCommandList* list = List(listPass, chunk);
    ListStats& stats = m_ListStats[pass][chunk];
    const bool first = chunk == 0;
    const bool last = chunk == chunkCount - 1;
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = BackBufferRtv();
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = DepthDsv();

    if (pass < kNumShadows)
    {
        const int shadow = pass;
        const SceneDraw& draw = m_Params.shadow[shadow];
        if (first)
        {
            // Frame begin: the back buffer becomes a render target and is cleared with the depth
            // buffer; the shadow map becomes writable and is cleared.
            if (shadow == 0)
            {
                const D3D12_RESOURCE_BARRIER toRt =
                    Transition(m_BackBuffers[m_Params.backBuffer].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
                list->ResourceBarrier(1, &toRt);
                list->ClearRenderTargetView(rtv, &Scene::kClearColor.x, 0, nullptr);
                list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
            }
            const D3D12_RESOURCE_BARRIER toDepth =
                Transition(m_ShadowMaps[shadow].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            list->ResourceBarrier(1, &toDepth);
            list->ClearDepthStencilView(ShadowDsv(shadow), D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        }
        const D3D12_CPU_DESCRIPTOR_HANDLE shadowDsv = ShadowDsv(shadow);
        list->OMSetRenderTargets(0, nullptr, FALSE, &shadowDsv);
        RenderScene(list, draw, chunk, chunkCount, stats);
        if (last)
        {
            const D3D12_RESOURCE_BARRIER toSrv =
                Transition(m_ShadowMaps[shadow].Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            list->ResourceBarrier(1, &toSrv);
        }
    }
    else if (pass < kPassDirect)
    {
        const MirrorPass& p = m_Params.mirror[pass - kNumShadows];
        if (!p.visible)
            return; // back-facing: the list stays empty
        list->OMSetRenderTargets(1, &rtv, TRUE, &dsv);
        if (first)
        {
            DrawMirrorQuad(list, PipelineMirrorStencil + (p.mirror % 8), p.mirror, p.mirrorObject, p.stencilScene, p.stencilBit, p.scissor);
            DrawMirrorQuad(list, PipelineMirrorClearDepth + (p.mirror % 8), p.mirror, p.mirrorObject, p.depthClearScene, p.stencilBit, p.scissor);
        }
        RenderScene(list, p.reflected, chunk, chunkCount, stats);
        if (last)
            DrawMirrorQuad(list, PipelineMirrorSurface + (p.mirror % 8), p.mirror, p.mirrorObject, p.stencilScene, p.stencilBit, p.scissor);
    }
    else
    {
        list->OMSetRenderTargets(1, &rtv, TRUE, &dsv);
        RenderScene(list, m_Params.direct, chunk, chunkCount, stats);
        if (last)
        {
            const D3D12_RESOURCE_BARRIER toPresent =
                Transition(m_BackBuffers[m_Params.backBuffer].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
            list->ResourceBarrier(1, &toPresent);
        }
    }
}

void RendererD3D12::DrawMirrorQuad(ID3D12GraphicsCommandList* list, int pipeline, int mirror, const CbPerObject& object, const CbPerScene& scene,
                                   uint8_t stencilRef, const RECT& scissor)
{
    UploadBufferStack& constants = m_Frames[m_FrameIndex].constants;
    SetViewportAndScissor(list, m_Width, m_Height, scissor);
    list->SetPipelineState(m_Pipelines[pipeline].Get());
    list->SetGraphicsRootSignature(m_RootSignature.Get());
    list->OMSetStencilRef(stencilRef);
    list->SetGraphicsRootConstantBufferView(RootCbPerObject, constants.Push(&object, sizeof(object)));
    list->SetGraphicsRootConstantBufferView(RootCbPerScene, constants.Push(&scene, sizeof(scene)));
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    list->IASetVertexBuffers(0, 1, &m_MirrorVbv[mirror]);
    list->DrawInstanced(4, 1, 0, 0);
}

void RendererD3D12::RenderScene(ID3D12GraphicsCommandList* list, const SceneDraw& draw, int chunk, int chunkCount, ListStats& stats)
{
    UploadBufferStack& constants = m_Frames[m_FrameIndex].constants;

    int pipeline = PipelineDirect;
    uint8_t stencilRef = 0;
    switch (draw.kind)
    {
        case SceneKind::Shadow: pipeline = PipelineShadow; break;
        case SceneKind::Mirrored:
            pipeline = PipelineMirroredScene + (draw.mirror % 8);
            stencilRef = uint8_t(1u << (draw.mirror % 8));
            break;
        case SceneKind::Direct: pipeline = PipelineDirect; break;
    }

    list->SetPipelineState(m_Pipelines[pipeline].Get());
    list->SetGraphicsRootSignature(m_RootSignature.Get());
    SetViewportAndScissor(list, draw.viewportWidth, draw.viewportHeight, draw.scissor);
    list->OMSetStencilRef(stencilRef);

    if (draw.kind != SceneKind::Shadow)
        list->SetGraphicsRootDescriptorTable(RootTableShadow, m_SrvHeap->GetGPUDescriptorHandleForHeapStart());

    list->SetGraphicsRootConstantBufferView(RootCbPerScene, constants.Push(&draw.scene, sizeof(draw.scene)));
    if (draw.hasLights)
        list->SetGraphicsRootConstantBufferView(RootCbPerLight, constants.Push(&m_Params.lights, sizeof(m_Params.lights)));
    list->SetGraphicsRootConstantBufferView(RootCbPerObject, constants.Push(&draw.object, sizeof(draw.object)));

    const uint32_t count = uint32_t(m_RenderOrder.size());
    for (uint32_t i = 0; i < count; ++i)
    {
        if (!ChunkOwnsDraw(i, uint32_t(chunk), uint32_t(chunkCount)))
            continue;
        const MeshDraw& mesh = m_RenderOrder[i];
        const MeshBuffers& buffers = m_MeshBuffers[mesh.buffers];
        list->IASetVertexBuffers(0, 1, &buffers.vbv);
        list->IASetIndexBuffer(&buffers.ibv);
        for (uint32_t s = 0; s < mesh.subsetCount; ++s)
        {
            const Subset& subset = m_Subsets[mesh.firstSubset + s];
            list->IASetPrimitiveTopology(subset.topology);
            const Material& material = m_Materials[subset.material];
            list->SetGraphicsRootDescriptorTable(RootTableDiffuse, material.diffuse);
            list->SetGraphicsRootDescriptorTable(RootTableNormal, material.normal);
            list->DrawIndexedInstanced(subset.indexCount, 1, subset.indexStart, INT(subset.vertexStart), 0);
            ++stats.draws;
            stats.indices += subset.indexCount;
        }
    }
}

} // namespace MtNative
