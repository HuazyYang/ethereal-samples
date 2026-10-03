#include "renderer.h"

#include <donut/core/log.h>
#include <donut/core/vfs/VFS.h>
#include <donut/engine/DDSFile.h>
#include <nvrhi/utils.h>

#include <dxgi1_5.h>

#include <algorithm>
#include <cstring>

#include "mesh.h"

using namespace DirectX;

namespace
{

struct DrawConstants // cbuffer of BINDING_MODE 0: one volatile constant buffer version per draw
{
    XMFLOAT4X4                      viewProjection;
    AsteroidsRenderer::AsteroidData data;
};

struct FrameConstants // cbuffer of BINDING_MODE 1 and 2, and of the skybox
{
    XMFLOAT4X4 viewProjection;
};

static_assert(sizeof(AsteroidsRenderer::AsteroidData) == 96, "must match AsteroidData in asteroid_vs.hlsl");
static_assert(sizeof(AsteroidsRenderer::AsteroidData) <= nvrhi::c_MaxPushConstantSize, "push constants are limited to 128 bytes");
static_assert(sizeof(DrawConstants) == 160, "must match DrawConstants in asteroid_vs.hlsl");
static_assert(sizeof(IndexType) == 2, "expecting a 16-bit index buffer");

// A volatile constant buffer needs one "version" per write on Vulkan, and a version is only recycled
// after the GPU finished the command list that used it. Frames in flight (Donut: maxFramesInFlight = 3),
// plus the frame being recorded, plus slack for the polling granularity of the garbage collection.
constexpr uint32_t kVolatileFrames = 8;

constexpr uint32_t kGpuTimerQueries = 16;

// ---- page-split stores (docs/LIMITATIONS.md) ----
// nvrhi's D3D12 CommandList keeps the currently bound volatile constant buffers in the member
//     static_vector<VolatileConstantBufferBinding, c_MaxVolatileConstantBuffers> m_CurrentGraphicsVolatileCBs
// (Donut/nvrhi/src/d3d12/d3d12-backend.h) and overwrites all of it on every setGraphicsState that changes a
// binding set (d3d12-resource-bindings.cpp, setGraphicsBindings: "m_CurrentGraphicsVolatileCBs = newVolatileCBs").
// One element is { uint32_t bindingPoint; Buffer* buffer; D3D12_GPU_VIRTUAL_ADDRESS address; } = 24 bytes.
constexpr size_t kCbStateElement = 24;
constexpr size_t kCbStateArray   = kCbStateElement * nvrhi::c_MaxVolatileConstantBuffers; // 768: the copied elements
constexpr size_t kPageSize       = 4096;
constexpr uint32_t kPlacementCandidatesMax = 512;

// Distance from `begin` to the page boundary strictly inside (begin, begin + size), or 0 when there is none.
size_t PageBoundaryInside(uint64_t begin, size_t size)
{
    const uint64_t boundary = (begin + kPageSize) & ~uint64_t(kPageSize - 1); // first boundary above begin
    return boundary < begin + size ? size_t(boundary - begin) : 0;
}

// 1: a store of nvrhi's copy straddles a page boundary; 0: none does. The copy uses 32- and 16-byte stores that
// start at multiples of 16 bytes from the start of the member, so a boundary at such a multiple is treated as
// harmless (it splits no 16-byte store; whether it splits a 32-byte one depends on the compiler's loop).
int CbStatePageSplit(nvrhi::ICommandList* commandList, int64_t stateOffset)
{
    if (stateOffset < 0 || !commandList) return -1;
    const size_t at = PageBoundaryInside(uint64_t(reinterpret_cast<uintptr_t>(commandList)) + uint64_t(stateOffset), kCbStateArray);
    return at != 0 && at % 16 != 0 ? 1 : 0;
}

// No page boundary inside the member or within 64 bytes of it: the condition of -cb_page_split_avoid.
bool CbStateFarFromPageBoundary(nvrhi::ICommandList* commandList, int64_t stateOffset)
{
    const uint64_t begin = uint64_t(reinterpret_cast<uintptr_t>(commandList)) + uint64_t(stateOffset);
    return PageBoundaryInside(begin - 64, kCbStateArray + 8 + 128) == 0;
}

const char* BindingModeName(BindingMode mode)
{
    switch (mode)
    {
        case BindingMode::Mut: return "mut";
        case BindingMode::TexMut: return "tex_mut";
        case BindingMode::TexMutPC: return "tex_mut_pc";
        case BindingMode::Bindless: return "bindless";
    }
    return "";
}

// Creates a buffer with immutable contents and gives it a permanent state, so that command lists never
// track it (CommandListResourceStateTracker::requireBufferState returns immediately).
nvrhi::BufferHandle CreateStaticBuffer(nvrhi::IDevice* device, nvrhi::ICommandList* commandList, nvrhi::BufferDesc desc,
                                       const void* data, nvrhi::ResourceStates state)
{
    nvrhi::BufferHandle buffer;
    if (NVRHI_FAILED(device->createBuffer(desc, &buffer))) return nullptr;
    commandList->beginTrackingBufferState(buffer, nvrhi::ResourceStates::Common);
    commandList->writeBuffer(buffer, data, size_t(desc.byteSize));
    commandList->setPermanentBufferState(buffer, state);
    return buffer;
}

} // namespace

AsteroidsRenderer::AsteroidsRenderer(donut::app::DeviceManager* deviceManager, const RendererDesc& desc,
                                     AsteroidsSimulation* simulation, OrbitCamera* camera, const Settings* settings)
    : IRenderPass(deviceManager)
    , m_Desc(desc)
    , m_Device(deviceManager->GetDevice())
    , m_Api(deviceManager->GetDevice()->getGraphicsAPI())
    , m_Simulation(simulation)
    , m_Camera(camera)
    , m_Settings(settings)
{
}

AsteroidsRenderer::~AsteroidsRenderer()
{
    Shutdown();
}

bool AsteroidsRenderer::Init(donut::engine::ShaderFactory& shaderFactory, const std::filesystem::path& mediaDirectory)
{
    const bool bindless = m_Desc.binding == BindingMode::Bindless;
    const uint32_t threadCount = m_Desc.threads;

    // ---- shaders (compiled at build time by ShaderTool, see shaders/shaders.cfg) ----
    {
        const char* bindingModeValue = m_Desc.binding == BindingMode::TexMutPC ? "1" : (bindless ? "2" : "0");
        std::vector<donut::engine::ShaderMacro> vsMacros = {{"BINDING_MODE", bindingModeValue}};
        std::vector<donut::engine::ShaderMacro> psMacros = {{"BINDLESS", bindless ? "1" : "0"}};
        m_AsteroidVS = shaderFactory.CreateShader("app/asteroid_vs.hlsl", "main", &vsMacros, nvrhi::ShaderType::Vertex);
        m_AsteroidPS = shaderFactory.CreateShader("app/asteroid_ps.hlsl", "main", &psMacros, nvrhi::ShaderType::Pixel);
        m_SkyboxVS   = shaderFactory.CreateShader("app/skybox_vs.hlsl", "main", nullptr, nvrhi::ShaderType::Vertex);
        m_SkyboxPS   = shaderFactory.CreateShader("app/skybox_ps.hlsl", "main", nullptr, nvrhi::ShaderType::Pixel);
        if (!m_AsteroidVS || !m_AsteroidPS || !m_SkyboxVS || !m_SkyboxPS)
        {
            donut::log::error("cannot load the asteroids_nvrhi shaders");
            return false;
        }
    }

    // ---- subsets: the split of the reference (Asteroids::SubsetRange in asteroids_DE.cpp):
    //      ceil(NUM_ASTEROIDS / threads) asteroids per subset, the last one takes what is left.
    const uint32_t total      = uint32_t(NUM_ASTEROIDS);
    const uint32_t subsetSize = (total + threadCount - 1) / threadCount;
    uint32_t maxSubsetSize = 0;
    for (uint32_t t = 0; t < threadCount; ++t)
    {
        auto ctx   = std::make_unique<ThreadContext>();
        ctx->first = std::min(subsetSize * t, total);
        ctx->count = std::min(subsetSize, total - ctx->first);
        maxSubsetSize = std::max(maxSubsetSize, ctx->count);
        m_Threads.push_back(std::move(ctx));
    }

    // ---- static resources, uploaded with a temporary command list ----
    nvrhi::CommandListHandle initCommandList;
    m_Device->createCommandList(nvrhi::CommandListParameters(), &initCommandList);
    initCommandList->open();

    const Mesh* meshes = m_Simulation->Meshes();
    m_VertexBuffer = CreateStaticBuffer(m_Device, initCommandList,
        nvrhi::BufferDesc().setByteSize(meshes->vertices.size() * sizeof(Vertex)).setIsVertexBuffer(true).setDebugName("Asteroid vertices"),
        meshes->vertices.data(), nvrhi::ResourceStates::VertexBuffer);
    m_IndexBuffer = CreateStaticBuffer(m_Device, initCommandList,
        nvrhi::BufferDesc().setByteSize(meshes->indices.size() * sizeof(IndexType)).setIsIndexBuffer(true).setDebugName("Asteroid indices"),
        meshes->indices.data(), nvrhi::ResourceStates::IndexBuffer);

    std::vector<SkyboxVertex> skyboxVertices;
    CreateSkyboxMesh(&skyboxVertices);
    m_SkyboxVertexBuffer = CreateStaticBuffer(m_Device, initCommandList,
        nvrhi::BufferDesc().setByteSize(skyboxVertices.size() * sizeof(SkyboxVertex)).setIsVertexBuffer(true).setDebugName("Skybox vertices"),
        skyboxVertices.data(), nvrhi::ResourceStates::VertexBuffer);

    if (bindless)
    {
        // SV_InstanceID does not include the base instance, so the instance index comes from a
        // per-instance vertex stream 0, 1, 2, ... addressed with startInstanceLocation (as in the reference).
        std::vector<uint32_t> ids(maxSubsetSize);
        for (uint32_t i = 0; i < maxSubsetSize; ++i) ids[i] = i;
        m_InstanceIdBuffer = CreateStaticBuffer(m_Device, initCommandList,
            nvrhi::BufferDesc().setByteSize(ids.size() * sizeof(uint32_t)).setIsVertexBuffer(true).setDebugName("Instance IDs"),
            ids.data(), nvrhi::ResourceStates::VertexBuffer);
    }

    // Asteroid textures: 256x256 RGBA8 sRGB, 3 array slices, full mip chain
    {
        const uint32_t mipLevels = m_Simulation->GetTextureMipLevels();
        const uint32_t arraySize = 3;
        nvrhi::TextureDesc textureDesc;
        textureDesc.width     = TEXTURE_DIM;
        textureDesc.height    = TEXTURE_DIM;
        textureDesc.arraySize = arraySize;
        textureDesc.mipLevels = mipLevels;
        textureDesc.format    = nvrhi::Format::SRGBA8_UNORM;
        textureDesc.dimension = nvrhi::TextureDimension::Texture2DArray;
        for (uint32_t t = 0; t < NUM_UNIQUE_TEXTURES; ++t)
        {
            textureDesc.debugName = "Asteroid texture " + std::to_string(t);
            if (NVRHI_FAILED(m_Device->createTexture(textureDesc, &m_Textures[t]))) return false;
            initCommandList->beginTrackingTextureState(m_Textures[t], nvrhi::AllSubresources, nvrhi::ResourceStates::Common);
            const D3D11_SUBRESOURCE_DATA* data = m_Simulation->TextureData(t);
            for (uint32_t slice = 0; slice < arraySize; ++slice)
                for (uint32_t mip = 0; mip < mipLevels; ++mip)
                {
                    const D3D11_SUBRESOURCE_DATA& subresource = data[slice * mipLevels + mip];
                    initCommandList->writeTexture(m_Textures[t], slice, mip, subresource.pSysMem, subresource.SysMemPitch);
                }
            // Permanent state: binding sets that reference the texture need no state tracking at all.
            initCommandList->setPermanentTextureState(m_Textures[t], nvrhi::ResourceStates::ShaderResource);
        }
        initCommandList->commitBarriers();
    }

    // Skybox cube map (BC1 sRGB). Donut's loader gives it the permanent ShaderResource state.
    {
        const std::filesystem::path path = mediaDirectory / "starbox_1024.dds";
        auto nativeFS = MAKE_RC_OBJ_PTR(donut::vfs::NativeFileSystem);
        nvrhi::AutoPtr<nvrhi::IDataBlob> blob;
        if (NVRHI_FAILED(nativeFS->readFile(path, &blob)) || !blob)
        {
            donut::log::error("cannot read %s", path.generic_string().c_str());
            return false;
        }
        m_SkyboxTexture = donut::engine::CreateDDSTextureFromMemory(m_Device, initCommandList, blob, "Skybox", /* forceSRGB = */ true);
        if (!m_SkyboxTexture)
        {
            donut::log::error("cannot create the skybox texture from %s", path.generic_string().c_str());
            return false;
        }
    }

    initCommandList->close();
    m_Device->executeCommandList(initCommandList);
    m_Device->waitForIdle();
    initCommandList = nullptr;
    m_Device->runGarbageCollection();

    // ---- sampler: anisotropic 2x, wrap (the sampler of the Diligent renderer) ----
    m_Device->createSampler(nvrhi::SamplerDesc()
        .setAllFilters(true)
        .setMaxAnisotropy(float(TEXTURE_ANISO))
        .setAllAddressModes(nvrhi::SamplerAddressMode::Wrap), &m_Sampler);

    // ---- vertex layouts ----
    {
        nvrhi::VertexAttributeDesc attributes[3] = {
            nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RGB32_FLOAT).setBufferIndex(0).setOffset(0).setElementStride(sizeof(Vertex)),
            nvrhi::VertexAttributeDesc().setName("NORMAL").setFormat(nvrhi::Format::RGB32_FLOAT).setBufferIndex(0).setOffset(12).setElementStride(sizeof(Vertex)),
            nvrhi::VertexAttributeDesc().setName("INSTANCEID").setFormat(nvrhi::Format::R32_UINT).setBufferIndex(1).setOffset(0).setElementStride(sizeof(uint32_t)).setIsInstanced(true)};
        m_Device->createInputLayout(attributes, bindless ? 3 : 2, m_AsteroidVS, &m_AsteroidInputLayout);

        nvrhi::VertexAttributeDesc skyboxAttribute =
            nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RGB32_FLOAT).setBufferIndex(0).setOffset(0).setElementStride(sizeof(SkyboxVertex));
        m_Device->createInputLayout(&skyboxAttribute, 1, m_SkyboxVS, &m_SkyboxInputLayout);
    }

    // ---- binding layouts ----
    // Layout 0 (register space 0): per-thread data - constants and the sampler.
    // Layout 1 (register space 1): the texture(s) - the part that changes between draws.
    // The sampler is kept out of the per-texture sets: every D3D12 binding set with a sampler takes a slot
    // of the shader-visible sampler heap, which holds 2048 descriptors at most (mut needs 50,000 sets).
    {
        nvrhi::BindingLayoutDesc layout0;
        layout0.setVisibility(nvrhi::ShaderType::AllGraphics).setRegisterSpaceAndDescriptorSet(0);
        switch (m_Desc.binding)
        {
            case BindingMode::Mut:
            case BindingMode::TexMut:
                layout0.addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0));
                break;
            case BindingMode::TexMutPC:
                layout0.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(AsteroidData)));
                layout0.addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(1));
                break;
            case BindingMode::Bindless:
                layout0.addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0));
                layout0.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0));
                break;
        }
        layout0.addItem(nvrhi::BindingLayoutItem::Sampler(0));
        m_Device->createBindingLayout(layout0, &m_Layout0);

        if (bindless)
        {
            m_Device->createBindlessLayout(nvrhi::BindlessLayoutDesc()
                .setVisibility(nvrhi::ShaderType::Pixel)
                .setFirstSlot(0)
                .setMaxCapacity(NUM_UNIQUE_TEXTURES)
                .addRegisterSpace(nvrhi::BindingLayoutItem::Texture_SRV(1)), &m_Layout1);
        }
        else
        {
            m_Device->createBindingLayout(nvrhi::BindingLayoutDesc()
                .setVisibility(nvrhi::ShaderType::Pixel)
                .setRegisterSpaceAndDescriptorSet(1)
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0)), &m_Layout1);
        }

        m_Device->createBindingLayout(nvrhi::BindingLayoutDesc()
            .setVisibility(nvrhi::ShaderType::AllGraphics)
            .setRegisterSpaceAndDescriptorSet(0)
            .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0))
            .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
            .addItem(nvrhi::BindingLayoutItem::Sampler(0)), &m_SkyboxLayout);

        if (!m_Layout0 || !m_Layout1 || !m_SkyboxLayout) return false;
    }

    // ---- texture binding sets, created up front (after the textures became permanent) ----
    if (bindless)
    {
        m_Device->createDescriptorTable(m_Layout1, &m_TextureTable);
        m_Device->resizeDescriptorTable(m_TextureTable, NUM_UNIQUE_TEXTURES, false);
        for (uint32_t t = 0; t < NUM_UNIQUE_TEXTURES; ++t)
            if (!m_Device->writeDescriptorTable(m_TextureTable, nvrhi::BindingSetItem::Texture_SRV(t, m_Textures[t])))
            {
                donut::log::error("cannot write the bindless descriptor table");
                return false;
            }
    }
    else
    {
        const uint32_t setCount = m_Desc.binding == BindingMode::Mut ? uint32_t(NUM_ASTEROIDS) : uint32_t(NUM_UNIQUE_TEXTURES);
        const AsteroidStatic* staticData = m_Simulation->StaticData();
        m_TextureSets.resize(setCount);
        m_TextureSetPtrs.resize(setCount);
        for (uint32_t i = 0; i < setCount; ++i)
        {
            const uint32_t texture = m_Desc.binding == BindingMode::Mut ? staticData[i].textureIndex : i;
            if (NVRHI_FAILED(m_Device->createBindingSet(nvrhi::BindingSetDesc()
                .setTrackLiveness(m_Desc.trackLiveness)
                .addItem(nvrhi::BindingSetItem::Texture_SRV(0, m_Textures[texture])), m_Layout1, &m_TextureSets[i])))
            {
                donut::log::error("cannot create texture binding set %u of %u", i, setCount);
                return false;
            }
            m_TextureSetPtrs[i] = m_TextureSets[i];
        }
    }

    // ---- skybox ----
    m_Device->createBuffer(nvrhi::BufferDesc()
        .setByteSize(sizeof(FrameConstants)).setIsConstantBuffer(true).setIsVolatile(true).setMaxVersions(kVolatileFrames * 2)
        .setDebugName("Skybox constants"), &m_SkyboxConstantBuffer);
    if (NVRHI_FAILED(m_Device->createBindingSet(nvrhi::BindingSetDesc()
        .setTrackLiveness(m_Desc.trackLiveness)
        .addItem(nvrhi::BindingSetItem::ConstantBuffer(0, m_SkyboxConstantBuffer))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(0, m_SkyboxTexture))
        .addItem(nvrhi::BindingSetItem::Sampler(0, m_Sampler)), m_SkyboxLayout, &m_SkyboxBindingSet))) return false;

    CreateThreadResources();

    if (m_Desc.gpuTiming)
    {
        for (uint32_t i = 0; i < kGpuTimerQueries; ++i)
        {
            nvrhi::TimerQueryHandle query;
            m_Device->createTimerQuery(&query);
            if (query) m_FreeQueries.push_back(query);
        }
    }

    if (m_Api == nvrhi::GraphicsAPI::D3D12)
    {
        // Is a mapped upload buffer write-combined memory? (Recorded only; nvrhi's upload chunks are such buffers.)
        ID3D12Device* d3d12 = static_cast<ID3D12Device*>(m_Device->getNativeObject(nvrhi::ObjectTypes::D3D12_Device));
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = 65536; desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
        desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        nvrhi::AutoPtr<ID3D12Resource> probe;
        void* mapped = nullptr;
        MEMORY_BASIC_INFORMATION info{};
        if (d3d12 && SUCCEEDED(d3d12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                              nullptr, IID_PPV_ARGS(&probe))) &&
            SUCCEEDED(probe->Map(0, nullptr, &mapped)) && mapped && VirtualQuery(mapped, &info, sizeof(info)) == sizeof(info))
        {
            char text[64];
            snprintf(text, sizeof(text), "0x%lx (%s)", static_cast<unsigned long>(info.Protect),
                     (info.Protect & PAGE_WRITECOMBINE) ? "write-combined" : "not write-combined");
            m_UploadHeapProtection = text;
            probe->Unmap(0, nullptr);
        }
    }

    // Persistent worker threads, one per additional subset (as in the reference).
    for (uint32_t t = 1; t < threadCount; ++t)
        m_Threads[t]->thread = std::thread(&AsteroidsRenderer::WorkerMain, this, t);

    return true;
}

void AsteroidsRenderer::CreateThreadResources()
{
    const bool perDrawConstants = m_Desc.binding == BindingMode::Mut || m_Desc.binding == BindingMode::TexMut;
    const bool multithreaded    = m_Threads.size() > 1;

    // nvrhi suballocates volatile constant buffer versions (D3D12) and writeBuffer data from upload
    // chunks owned by the command list; the default chunk is 64 KB, i.e. 256 draws on D3D12.
    m_UploadChunkSize = size_t(1) << 20;

    nvrhi::CommandListParameters& commandListParams = m_CommandListParams;
    commandListParams = nvrhi::CommandListParameters();
    commandListParams.setUploadChunkSize(m_UploadChunkSize);
    // More than one command list is open at a time: they cannot be "immediate" ones.
    // (With one thread the default keeps the single list valid on D3D11 as well.)
    commandListParams.setEnableImmediateExecution(!multithreaded);

    for (auto& ctxPtr : m_Threads)
    {
        ThreadContext& ctx = *ctxPtr;
        m_Device->createCommandList(commandListParams, &ctx.commandList);

        nvrhi::BindingSetDesc set0;
        set0.setTrackLiveness(m_Desc.trackLiveness);
        if (perDrawConstants)
        {
            // One buffer per thread: on Vulkan every write claims a version slot of the buffer with an atomic
            // compare-exchange, so a buffer shared by all threads would make them contend on it.
            const uint32_t versions = ctx.count * kVolatileFrames;
            m_VolatileVersions = std::max(m_VolatileVersions, versions); // the last subset may be smaller
            m_Device->createBuffer(nvrhi::BufferDesc()
                .setByteSize(sizeof(DrawConstants)).setIsConstantBuffer(true).setIsVolatile(true)
                .setMaxVersions(versions).setDebugName("Draw constants"), &ctx.constantBuffer);
            set0.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, ctx.constantBuffer));
        }
        else
        {
            m_VolatileVersions = std::max(m_VolatileVersions, kVolatileFrames * 2);
            m_Device->createBuffer(nvrhi::BufferDesc()
                .setByteSize(sizeof(FrameConstants)).setIsConstantBuffer(true).setIsVolatile(true)
                .setMaxVersions(kVolatileFrames * 2).setDebugName("Frame constants"), &ctx.constantBuffer);
            if (m_Desc.binding == BindingMode::TexMutPC)
            {
                set0.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(AsteroidData)));
                set0.addItem(nvrhi::BindingSetItem::ConstantBuffer(1, ctx.constantBuffer));
            }
            else
            {
                m_Device->createBuffer(nvrhi::BufferDesc()
                    .setByteSize(uint64_t(ctx.count) * sizeof(AsteroidData)).setStructStride(sizeof(AsteroidData))
                    .enableAutomaticStateTracking(nvrhi::ResourceStates::ShaderResource).setDebugName("Asteroid instance data"), &ctx.instanceData);
                ctx.staging.resize(ctx.count);
                set0.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, ctx.constantBuffer));
                set0.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, ctx.instanceData));
            }
        }
        set0.addItem(nvrhi::BindingSetItem::Sampler(0, m_Sampler));
        m_Device->createBindingSet(set0, m_Layout0, &ctx.bindingSet0);
        if (!ctx.commandList || !ctx.constantBuffer || !ctx.bindingSet0)
            donut::log::error("cannot create the per-thread resources");
    }

    if (multithreaded)
        m_Device->createCommandList(commandListParams, &m_PostCommandList);
}

void AsteroidsRenderer::CreatePipelines(nvrhi::IFramebuffer* framebuffer)
{
    // State of the reference: default blend, depth test GREATER_EQUAL with depth write (reverse Z),
    // back-face culling with clockwise front faces, depth clipping on.
    nvrhi::RenderState renderState;
    renderState.depthStencilState.depthTestEnable  = true;
    renderState.depthStencilState.depthWriteEnable = true;
    renderState.depthStencilState.depthFunc        = nvrhi::ComparisonFunc::GreaterOrEqual;
    renderState.depthStencilState.stencilEnable    = false;
    renderState.rasterState.cullMode              = nvrhi::RasterCullMode::Back;
    renderState.rasterState.frontCounterClockwise = false;
    renderState.rasterState.depthClipEnable       = true;

    nvrhi::GraphicsPipelineDesc asteroidDesc;
    asteroidDesc.primType    = nvrhi::PrimitiveType::TriangleList;
    asteroidDesc.inputLayout = m_AsteroidInputLayout;
    asteroidDesc.VS          = m_AsteroidVS;
    asteroidDesc.PS          = m_AsteroidPS;
    asteroidDesc.renderState = renderState;
    asteroidDesc.bindingLayouts = {m_Layout0, m_Layout1};
    m_Device->createGraphicsPipeline2(asteroidDesc, framebuffer, &m_AsteroidPipeline);

    nvrhi::GraphicsPipelineDesc skyboxDesc;
    skyboxDesc.primType    = nvrhi::PrimitiveType::TriangleList;
    skyboxDesc.inputLayout = m_SkyboxInputLayout;
    skyboxDesc.VS          = m_SkyboxVS;
    skyboxDesc.PS          = m_SkyboxPS;
    skyboxDesc.renderState = renderState;
    skyboxDesc.bindingLayouts = {m_SkyboxLayout};
    m_Device->createGraphicsPipeline2(skyboxDesc, framebuffer, &m_SkyboxPipeline);

    if (!m_AsteroidPipeline || !m_SkyboxPipeline)
        donut::log::error("cannot create the graphics pipelines");
}

void AsteroidsRenderer::BackBufferResizing()
{
    // The swap chain cannot be resized while anything references its buffers.
    m_Framebuffers.clear();
    m_CaptureFramebuffer = nullptr;
    m_CaptureTexture     = nullptr;
    m_CaptureStaging     = nullptr;
    m_DepthBuffer        = nullptr;
    m_FrameFramebuffer   = nullptr;
    for (auto& ctx : m_Threads) ctx->state.framebuffer = nullptr;
    m_SkyboxState.framebuffer = nullptr;
}

void AsteroidsRenderer::BackBufferResized(uint32_t width, uint32_t height, uint32_t sampleCount)
{
    (void)sampleCount;
    if (width == 0 || height == 0 || m_Threads.empty()) return;
    m_Width  = width;
    m_Height = height;

    // Camera projection as in the reference (WM_SIZE handler of WinWrapper.cpp)
    m_Camera->Projection(XM_PIDIV2 * 0.8f * 3 / 2, float(width) / float(height));

    // Depth buffer: D32, reverse Z (cleared to 0), as in the reference.
    nvrhi::TextureDesc depthDesc = nvrhi::TextureDesc()
        .setWidth(width).setHeight(height).setFormat(nvrhi::Format::D32).setIsRenderTarget(true)
        .setClearValue(nvrhi::Color(0.f))
        .enableAutomaticStateTracking(nvrhi::ResourceStates::DepthWrite)
        .setDebugName("Depth buffer");
    depthDesc.isShaderResource = false; // never sampled; D3D11 rejects a non-typeless D32 texture with an SRV bind flag
    m_Device->createTexture(depthDesc, &m_DepthBuffer);

    donut::app::DeviceManager* deviceManager = GetDeviceManager();
    const uint32_t backBufferCount = deviceManager->GetBackBufferCount();
    m_Framebuffers.resize(backBufferCount);
    for (uint32_t i = 0; i < backBufferCount; ++i)
        m_Device->createFramebuffer(nvrhi::FramebufferDesc()
            .addColorAttachment(deviceManager->GetBackBuffer(i))
            .setDepthAttachment(m_DepthBuffer), &m_Framebuffers[i]);

    if (!m_AsteroidPipeline)
        CreatePipelines(m_Framebuffers[0]);

    // Graphics states: built once, only the framebuffer (per frame) and, in the texture binding modes,
    // bindings[1] (per draw) change afterwards.
    const nvrhi::ViewportState viewport =
        nvrhi::ViewportState().addViewportAndScissorRect(nvrhi::Viewport(float(width), float(height)));

    for (auto& ctxPtr : m_Threads)
    {
        nvrhi::GraphicsState& state = ctxPtr->state;
        state = nvrhi::GraphicsState();
        state.pipeline = m_AsteroidPipeline;
        state.viewport = viewport;
        state.bindings.push_back(ctxPtr->bindingSet0);
        state.bindings.push_back(m_Desc.binding == BindingMode::Bindless ? static_cast<nvrhi::IBindingSet*>(m_TextureTable.Get())
                                                                        : m_TextureSetPtrs[0]);
        state.vertexBuffers.push_back(nvrhi::VertexBufferBinding().setBuffer(m_VertexBuffer).setSlot(0).setOffset(0));
        if (m_Desc.binding == BindingMode::Bindless)
            state.vertexBuffers.push_back(nvrhi::VertexBufferBinding().setBuffer(m_InstanceIdBuffer).setSlot(1).setOffset(0));
        state.indexBuffer = nvrhi::IndexBufferBinding().setBuffer(m_IndexBuffer).setFormat(nvrhi::Format::R16_UINT).setOffset(0);
    }

    m_SkyboxState = nvrhi::GraphicsState();
    m_SkyboxState.pipeline = m_SkyboxPipeline;
    m_SkyboxState.viewport = viewport;
    m_SkyboxState.bindings.push_back(m_SkyboxBindingSet);
    m_SkyboxState.vertexBuffers.push_back(nvrhi::VertexBufferBinding().setBuffer(m_SkyboxVertexBuffer).setSlot(0).setOffset(0));
}

// ------------------------------------------------------------------------------------------------
// Worker threads (scheme of the reference's asteroids_DE.cpp: persistent threads, an update and a
// render signal per frame, the main thread spins with yield() until all workers reported completion)
// ------------------------------------------------------------------------------------------------

void AsteroidsRenderer::Dispatch(Job job)
{
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Completed.store(0, std::memory_order_relaxed);
        m_Job = job;
        ++m_Generation;
    }
    m_Signal.notify_all();
}

void AsteroidsRenderer::JoinWorkers()
{
    const int workers = int(m_Threads.size()) - 1;
    while (m_Completed.load(std::memory_order_acquire) < workers)
        std::this_thread::yield();
}

void AsteroidsRenderer::WorkerMain(uint32_t threadIndex)
{
    ThreadContext& ctx = *m_Threads[threadIndex];
    uint64_t seen = 0;
    for (;;)
    {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m_Mutex);
            m_Signal.wait(lock, [&] { return m_Generation != seen; });
            seen = m_Generation;
            job  = m_Job;
        }
        if (job == Job::Exit) return;

        if (job == Job::Update)
        {
            m_Simulation->Update(m_FrameDelta, m_Camera->Eye(), *m_Settings, ctx.first, ctx.count);
        }
        else
        {
            ctx.commandList->open();
            RecordAsteroids(ctx);
            ctx.commandList->close();
        }
        m_Completed.fetch_add(1, std::memory_order_release);
    }
}

// ------------------------------------------------------------------------------------------------
// Recording
// ------------------------------------------------------------------------------------------------

void AsteroidsRenderer::RecordAsteroids(ThreadContext& ctx)
{
    nvrhi::ICommandList* const  commandList = ctx.commandList;
    nvrhi::GraphicsState&       state       = ctx.state;
    const AsteroidStatic* const staticData  = m_Simulation->StaticData();
    const AsteroidDynamic* const dynamicData = m_Simulation->DynamicData();
    const uint32_t first = ctx.first, last = ctx.first + ctx.count;

    state.framebuffer = m_FrameFramebuffer;

    nvrhi::DrawArguments args;
    args.instanceCount = 1;
    uint64_t indices = 0;

    // Sensitivity switches (RendererDesc). alwaysSetState: no redundant-state filter in the application.
    // noAutoBarriers: the first setGraphicsState of the list places every barrier the list needs (all other
    // resources are permanent or volatile), so automatic barrier placement is switched off for the rest.
    const bool alwaysSetState = m_Desc.alwaysSetState;
    bool       disableBarriers = m_Desc.noAutoBarriers;
    auto setState = [&] {
        commandList->setGraphicsState(state);
        if (disableBarriers)
        {
            commandList->setEnableAutomaticBarriers(false);
            disableBarriers = false;
        }
    };

    switch (m_Desc.binding)
    {
        case BindingMode::Mut:
        case BindingMode::TexMut:
        {
            // Per draw: one volatile constant buffer version; the binding set is switched with
            // setGraphicsState whenever the texture set differs from the bound one.
            nvrhi::IBuffer* const constantBuffer = ctx.constantBuffer;
            nvrhi::IBindingSet* const* const sets = m_TextureSetPtrs.data();
            const bool perAsteroidSet = m_Desc.binding == BindingMode::Mut;
            nvrhi::IBindingSet* bound = nullptr;
            // 64-byte alignment: the 32-byte stores of the world matrix below must never straddle a page boundary
            // (a page-split store after a write into write-combined memory costs about 80 ns on this CPU, and the
            // offset of the stack inside its page differs from process to process).
            alignas(64) DrawConstants constants;
            if (&ctx == m_Threads[0].get()) m_DrawConstantsAddress = uint64_t(reinterpret_cast<uintptr_t>(&constants));
            constants.viewProjection = m_ViewProjection;
            constants.data.unused0   = 0.f;
            for (uint32_t i = first; i < last; ++i)
            {
                const AsteroidStatic&  s = staticData[i];
                const AsteroidDynamic& d = dynamicData[i];

                XMStoreFloat4x4(&constants.data.world, d.world);
                constants.data.surfaceColor = s.surfaceColor;
                constants.data.deepColor    = s.deepColor;
                constants.data.textureIndex = s.textureIndex;
                commandList->writeBuffer(constantBuffer, &constants, sizeof(constants));

                nvrhi::IBindingSet* const set = perAsteroidSet ? sets[i] : sets[s.textureIndex];
                if (set != bound || alwaysSetState)
                {
                    state.bindings[1] = set;
                    setState();
                    bound = set;
                }

                args.vertexCount         = d.indexCount;
                args.startIndexLocation  = d.indexStart;
                args.startVertexLocation = s.vertexStart;
                commandList->drawIndexed(args);
                indices += d.indexCount;
            }
            break;
        }

        case BindingMode::TexMutPC:
        {
            // View-projection once per command list, per-draw data through push constants.
            FrameConstants frame;
            frame.viewProjection = m_ViewProjection;
            commandList->writeBuffer(ctx.constantBuffer, &frame, sizeof(frame));

            nvrhi::IBindingSet* const* const sets = m_TextureSetPtrs.data();
            nvrhi::IBindingSet* bound = nullptr;
            alignas(64) AsteroidData data; // see DrawConstants above
            data.unused0 = 0.f;
            for (uint32_t i = first; i < last; ++i)
            {
                const AsteroidStatic&  s = staticData[i];
                const AsteroidDynamic& d = dynamicData[i];

                nvrhi::IBindingSet* const set = sets[s.textureIndex];
                if (set != bound || alwaysSetState)
                {
                    state.bindings[1] = set;
                    setState();
                    bound = set;
                }

                XMStoreFloat4x4(&data.world, d.world);
                data.surfaceColor = s.surfaceColor;
                data.deepColor    = s.deepColor;
                data.textureIndex = s.textureIndex;
                commandList->setPushConstants(&data, sizeof(data));

                args.vertexCount         = d.indexCount;
                args.startIndexLocation  = d.indexStart;
                args.startVertexLocation = s.vertexStart;
                commandList->drawIndexed(args);
                indices += d.indexCount;
            }
            break;
        }

        case BindingMode::Bindless:
        {
            // All per-instance data of the subset in one structured buffer update, one state, then draws only.
            AsteroidData* const staging = ctx.staging.data();
            for (uint32_t i = first; i < last; ++i)
            {
                AsteroidData& data = staging[i - first];
                XMStoreFloat4x4(&data.world, dynamicData[i].world);
                data.surfaceColor = staticData[i].surfaceColor;
                data.unused0      = 0.f;
                data.deepColor    = staticData[i].deepColor;
                data.textureIndex = staticData[i].textureIndex;
            }
            commandList->writeBuffer(ctx.instanceData, staging, size_t(ctx.count) * sizeof(AsteroidData));

            FrameConstants frame;
            frame.viewProjection = m_ViewProjection;
            commandList->writeBuffer(ctx.constantBuffer, &frame, sizeof(frame));

            setState();
            for (uint32_t i = first; i < last; ++i)
            {
                const AsteroidDynamic& d = dynamicData[i];
                if (alwaysSetState && i != first) setState();
                args.vertexCount           = d.indexCount;
                args.startIndexLocation    = d.indexStart;
                args.startVertexLocation   = staticData[i].vertexStart;
                args.startInstanceLocation = i - first;
                commandList->drawIndexed(args);
                indices += d.indexCount;
            }
            break;
        }
    }

    if (m_Desc.noAutoBarriers)
        commandList->setEnableAutomaticBarriers(true); // the skybox, the capture copy and the next frame rely on them

    ctx.draws   = ctx.count;
    ctx.indices = indices;
}

void AsteroidsRenderer::RecordSkybox(nvrhi::ICommandList* commandList)
{
    FrameConstants constants;
    constants.viewProjection = m_ViewProjection;
    commandList->writeBuffer(m_SkyboxConstantBuffer, &constants, sizeof(constants));

    m_SkyboxState.framebuffer = m_FrameFramebuffer;
    commandList->setGraphicsState(m_SkyboxState);

    nvrhi::DrawArguments args;
    args.vertexCount = 6 * 6;
    commandList->draw(args);
}

void AsteroidsRenderer::Animate(float elapsedTimeSeconds)
{
    if (m_Done) return;

    m_Camera->ProcessInertia();

    // Fixed step in benchmark mode; otherwise the smoothed real frame time of the reference.
    m_SmoothedDelta = 0.2f * elapsedTimeSeconds + 0.8f * m_SmoothedDelta;
    m_FrameDelta    = Benchmark::FrameDelta(m_SmoothedDelta);

    {
        Benchmark::ScopedTimer timer(Benchmark::recorder.Times().update);
        const bool multithreaded = m_Threads.size() > 1;
        if (multithreaded) Dispatch(Job::Update);
        m_Simulation->Update(m_FrameDelta, m_Camera->Eye(), *m_Settings, m_Threads[0]->first, m_Threads[0]->count);
        if (multithreaded) JoinWorkers();
    }

    if (Benchmark::recorder.CaptureThisFrame())
        Benchmark::recorder.SetCaptureSceneHash(Benchmark::DynamicSceneHash(*m_Simulation, unsigned(NUM_ASTEROIDS)));
}

void AsteroidsRenderer::Render(nvrhi::IFramebuffer* framebuffer)
{
    (void)framebuffer; // Donut's framebuffer has no depth attachment; the pass uses its own (same back buffer).
    if (m_Done || m_Framebuffers.empty()) return;

    donut::app::DeviceManager* deviceManager = GetDeviceManager();
    const bool multithreaded = m_Threads.size() > 1;
    const bool capture       = Benchmark::recorder.CaptureThisFrame();

    if (capture) PrepareCapture();
    if (m_PlacementPending) ApplyPendingCommandLists(); // outside the timed columns; the workers are idle

    ThreadContext& main = *m_Threads[0];
    nvrhi::ICommandList* const mainCommandList = main.commandList;
    nvrhi::ICommandList* const lastCommandList = multithreaded ? m_PostCommandList.Get() : mainCommandList;

    // ---- render: from the start of command recording until every command list is recorded ----
    {
        Benchmark::ScopedTimer timer(Benchmark::recorder.Times().render);

        nvrhi::IFramebuffer* backBufferFramebuffer = m_Framebuffers[deviceManager->GetCurrentBackBufferIndex()];
        m_FrameFramebuffer = capture ? m_CaptureFramebuffer.Get() : backBufferFramebuffer;
        XMStoreFloat4x4(&m_ViewProjection, m_Camera->ViewProjection());

        if (multithreaded) Dispatch(Job::Render);

        mainCommandList->open();
        BeginGpuTimer(mainCommandList);
        if (capture)
        {
            // The back buffer is presented but not drawn on the capture frame: give it defined contents.
            nvrhi::utils::ClearColorAttachment(mainCommandList, backBufferFramebuffer, 0, nvrhi::Color(0.f));
        }
        nvrhi::utils::ClearColorAttachment(mainCommandList, m_FrameFramebuffer, 0, nvrhi::Color(0.f));
        nvrhi::utils::ClearDepthStencilAttachment(mainCommandList, m_FrameFramebuffer, 0.f, 0);
        RecordAsteroids(main);

        if (multithreaded) m_PostCommandList->open();
        RecordSkybox(lastCommandList);
        if (capture)
            lastCommandList->copyTexture2(m_CaptureStaging, nvrhi::TextureSlice(), m_CaptureTexture, nvrhi::TextureSlice());
        EndGpuTimer(lastCommandList);

        if (multithreaded) JoinWorkers(); // the workers close their command lists themselves
    }

    // Once, on the first frame, while the main thread's list is still open (close() clears the state looked for).
    if (!m_PlacementProbed) ProbeCommandListPlacement(main);

    // ---- submit: close the main thread's command lists and execute everything with one call ----
    {
        Benchmark::ScopedTimer timer(Benchmark::recorder.Times().submit);

        mainCommandList->close();
        if (multithreaded) m_PostCommandList->close();

        m_ExecuteList.clear();
        for (auto& ctx : m_Threads) m_ExecuteList.push_back(ctx->commandList);
        if (multithreaded) m_ExecuteList.push_back(m_PostCommandList);
        m_Device->executeCommandLists(m_ExecuteList.data(), m_ExecuteList.size());
    }

    if (m_ActiveQuery)
    {
        m_PendingQueries.emplace_back(Benchmark::recorder.FrameIndex(), m_ActiveQuery);
        m_ActiveQuery = nullptr;
    }

    if (capture) FinishCapture();

    m_RenderedThisFrame = true;
}

// ------------------------------------------------------------------------------------------------
// Command list placement (RendererDesc::cbPageSplit, docs/LIMITATIONS.md "page-split store"). D3D12 only.
// ------------------------------------------------------------------------------------------------

// Finds nvrhi's m_CurrentGraphicsVolatileCBs inside the D3D12 CommandList object by its contents: after a
// setGraphicsState with one volatile constant buffer bound, element 0 is { root parameter index, the Buffer*,
// the GPU address of the last write (256-byte aligned) } and the element count behind the array is 1. Nothing
// is hard-coded, so another nvrhi version or compiler cannot silently shift the answer; when the pattern is
// not found the placement is reported as unknown.
void AsteroidsRenderer::ProbeCommandListPlacement(ThreadContext& main)
{
    m_PlacementProbed = true;
    if (m_Api != nvrhi::GraphicsAPI::D3D12) return;

    const auto* object = reinterpret_cast<const unsigned char*>(main.commandList.Get());
    MEMORY_BASIC_INFORMATION region{};
    if (object && VirtualQuery(object, &region, sizeof(region)) == sizeof(region) && region.State == MEM_COMMIT)
    {
        const auto* regionEnd = static_cast<const unsigned char*>(region.BaseAddress) + region.RegionSize;
        const size_t readable = std::min<size_t>(size_t(regionEnd - object), 16384);
        // With one thread the skybox was recorded into the same list after the asteroids: its buffer is bound then.
        const uint64_t wanted  = uint64_t(reinterpret_cast<uintptr_t>(main.constantBuffer.Get()));
        const uint64_t wanted2 = uint64_t(reinterpret_cast<uintptr_t>(m_SkyboxConstantBuffer.Get()));
        for (size_t offset = 0; offset + kCbStateArray + 8 <= readable; offset += 8)
        {
            uint32_t index; uint64_t buffer, address, count;
            memcpy(&index, object + offset, 4);
            memcpy(&buffer, object + offset + 8, 8);
            memcpy(&address, object + offset + 16, 8);
            memcpy(&count, object + offset + kCbStateArray, 8);
            if ((buffer == wanted || buffer == wanted2) && index < 64 && address != 0 && (address & 0xff) == 0 && count == 1)
            {
                m_CbStateOffset = int64_t(offset);
                break;
            }
        }
    }

    for (auto& ctx : m_Threads)
        ctx->cbPageSplit = CbStatePageSplit(ctx->commandList, m_CbStateOffset);

    if (m_Desc.cbPageSplit == RendererDesc::CbPageSplit::Natural) return;
    if (m_CbStateOffset < 0)
        Benchmark::Fail("-cb_page_split_*: the volatile constant buffer state was not found inside the nvrhi D3D12 command list");

    // Create command lists until one lies as requested; the rejected ones stay alive meanwhile so that the heap
    // hands out new addresses, and are released afterwards (they were never opened).
    const bool force = m_Desc.cbPageSplit == RendererDesc::CbPageSplit::Force;
    std::vector<nvrhi::CommandListHandle> rejected;
    for (auto& ctxPtr : m_Threads)
    {
        ThreadContext& ctx = *ctxPtr;
        auto suitable = [&](nvrhi::ICommandList* commandList) {
            return force ? CbStatePageSplit(commandList, m_CbStateOffset) == 1 : CbStateFarFromPageBoundary(commandList, m_CbStateOffset);
        };
        if (suitable(ctx.commandList)) continue;
        for (;;)
        {
            if (m_PlacementCandidates >= kPlacementCandidatesMax)
                Benchmark::Fail("-cb_page_split_*: no command list with the requested placement among " +
                                std::to_string(kPlacementCandidatesMax) + " candidates");
            nvrhi::CommandListHandle candidate;
            m_Device->createCommandList(m_CommandListParams, &candidate);
            ++m_PlacementCandidates;
            if (suitable(candidate))
            {
                ctx.pendingCommandList = candidate;
                m_PlacementPending = true;
                break;
            }
            rejected.push_back(candidate);
        }
    }
}

void AsteroidsRenderer::ApplyPendingCommandLists()
{
    m_PlacementPending = false;
    for (auto& ctx : m_Threads)
    {
        if (!ctx->pendingCommandList) continue;
        m_RetiredCommandLists.push_back(ctx->commandList); // executed last frame: must outlive the GPU's use of it
        ctx->commandList        = ctx->pendingCommandList;
        ctx->pendingCommandList = nullptr;
        ctx->cbPageSplit        = CbStatePageSplit(ctx->commandList, m_CbStateOffset);
    }
}

// ------------------------------------------------------------------------------------------------
// GPU timing: one nvrhi timer query per frame, begun in the first and ended in the last command list
// ------------------------------------------------------------------------------------------------

void AsteroidsRenderer::BeginGpuTimer(nvrhi::ICommandList* commandList)
{
    if (!m_Desc.gpuTiming) return;
    if (m_FreeQueries.empty())
    {
        ++m_GpuFramesSkipped; // all queries are still in flight: this frame gets no gpu_ms
        return;
    }
    m_ActiveQuery = m_FreeQueries.back();
    m_FreeQueries.pop_back();
    commandList->beginTimerQuery(m_ActiveQuery);
}

void AsteroidsRenderer::EndGpuTimer(nvrhi::ICommandList* commandList)
{
    if (m_ActiveQuery) commandList->endTimerQuery(m_ActiveQuery);
}

void AsteroidsRenderer::PollGpuTimers()
{
    while (!m_PendingQueries.empty() && m_Device->pollTimerQuery(m_PendingQueries.front().second))
    {
        auto& [frame, query] = m_PendingQueries.front();
        Benchmark::recorder.SetGpuMs(frame, 1000.0 * double(m_Device->getTimerQueryTime(query)));
        m_Device->resetTimerQuery(query);
        m_FreeQueries.push_back(query);
        m_PendingQueries.pop_front();
    }
}

// ------------------------------------------------------------------------------------------------
// Frame capture for the parity checks (not used in measured runs). A Vulkan swap chain image created
// by Donut has no TRANSFER_SRC usage, so the capture frame is rendered into an offscreen texture of
// the back buffer's format and size with exactly the same commands, and that texture is read back.
// ------------------------------------------------------------------------------------------------

void AsteroidsRenderer::PrepareCapture()
{
    if (m_CaptureTexture) return;
    const nvrhi::Format format = GetDeviceManager()->GetBackBuffer(0)->getDesc().format;

    nvrhi::TextureDesc desc;
    desc.width          = m_Width;
    desc.height         = m_Height;
    desc.format         = format;
    desc.isRenderTarget = true;
    desc.debugName      = "Capture target";
    desc.setClearValue(nvrhi::Color(0.f));
    desc.enableAutomaticStateTracking(nvrhi::ResourceStates::RenderTarget);
    m_Device->createTexture(desc, &m_CaptureTexture);

    m_Device->createFramebuffer(nvrhi::FramebufferDesc()
        .addColorAttachment(m_CaptureTexture)
        .setDepthAttachment(m_DepthBuffer), &m_CaptureFramebuffer);

    nvrhi::TextureDesc stagingDesc;
    stagingDesc.width     = m_Width;
    stagingDesc.height    = m_Height;
    stagingDesc.format    = format;
    stagingDesc.debugName = "Capture staging";
    m_Device->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read, &m_CaptureStaging);
    m_CapturePending = true;
}

void AsteroidsRenderer::FinishCapture()
{
    if (!m_CapturePending) return;
    m_CapturePending = false;

    m_Device->waitForIdle();
    size_t rowPitch = 0;
    const void* pixels = m_Device->mapStagingTexture(m_CaptureStaging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, rowPitch);
    if (!pixels)
    {
        donut::log::error("capture: cannot map the staging texture");
        return;
    }
    const nvrhi::Format format = m_CaptureStaging->getDesc().format;
    const bool bgra = format == nvrhi::Format::BGRA8_UNORM || format == nvrhi::Format::SBGRA8_UNORM;
    std::string error;
    const bool ok = Benchmark::WriteCapture(pixels, int(m_Width), int(m_Height), rowPitch, bgra, error);
    m_Device->unmapStagingTexture(m_CaptureStaging);
    if (!ok) donut::log::error("capture: %s", error.c_str());
}

// ------------------------------------------------------------------------------------------------
// Benchmark columns. Donut owns the frame loop (DeviceManager::RunMessageLoop):
//
//   beforeFrame | glfwPollEvents | Animate | afterAnimate | BeginFrame | beforeRender | Render |
//   beforePresent | Present | afterPresent | sleep_for(0), IDevice::runGarbageCollection | next frame
//
// A benchmark frame runs from afterPresent to the next afterPresent:
//   wait    += afterPresent .. beforeFrame   Donut's sleep_for(0) + per-frame IDevice::runGarbageCollection
//                                            (polls the GPU for finished command lists and releases what they
//                                            referenced); the same interval is the gc_ms column (a part of
//                                            wait_ms) and extra.median_gc_ms
//   update   = simulation update inside Animate
//   wait    += afterAnimate .. beforeRender  DeviceManager::BeginFrame (D3D12: frame fence wait,
//                                            Vulkan: vkAcquireNextImageKHR, D3D11: nothing)
//   render   = Render: start of recording until all command lists are recorded and the workers joined
//   submit   = Render: closing the main thread's command lists + IDevice::executeCommandLists
//   present  = beforePresent .. afterPresent DeviceManager::Present (on Vulkan this includes Donut's wait
//                                            for the frame maxFramesInFlight ago)
// ------------------------------------------------------------------------------------------------

void AsteroidsRenderer::InstallFrameCallbacks()
{
    auto& callbacks = GetDeviceManager()->m_callbacks;
    callbacks.beforeFrame   = [this](donut::app::DeviceManager&, uint32_t) { OnBeforeFrame(); };
    callbacks.afterAnimate  = [this](donut::app::DeviceManager&, uint32_t) { OnAfterAnimate(); };
    callbacks.beforeRender  = [this](donut::app::DeviceManager&, uint32_t) { OnBeforeRender(); };
    callbacks.beforePresent = [this](donut::app::DeviceManager&, uint32_t) { OnBeforePresent(); };
    callbacks.afterPresent  = [this](donut::app::DeviceManager&, uint32_t) { OnAfterPresent(); };

    m_GcSamples.reserve(size_t(1) << 16);
    Benchmark::recorder.BeginFrame(); // opens frame 0
    m_FrameStartStamp = Benchmark::Now();
}

void AsteroidsRenderer::OnBeforeFrame()
{
    const double ms = Benchmark::Ms(Benchmark::Now() - m_FrameStartStamp);
    Benchmark::recorder.Times().wait += ms;
    Benchmark::recorder.Times().gc   += ms;
    if (Benchmark::recorder.Measuring()) m_GcSamples.push_back(ms);
}

void AsteroidsRenderer::OnAfterAnimate()
{
    m_WaitStamp = Benchmark::Now();
}

void AsteroidsRenderer::OnBeforeRender()
{
    Benchmark::recorder.Times().wait += Benchmark::Ms(Benchmark::Now() - m_WaitStamp);
}

void AsteroidsRenderer::OnBeforePresent()
{
    m_PresentStamp = Benchmark::Now();
}

void AsteroidsRenderer::OnAfterPresent()
{
    if (m_Done) return;
    Benchmark::recorder.Times().present += Benchmark::Ms(Benchmark::Now() - m_PresentStamp);

    if (m_RenderedThisFrame)
    {
        // asteroid draws + the skybox draw; indices of the asteroid draws (the skybox is not indexed)
        uint64_t draws = 1, indices = 0;
        for (auto& ctx : m_Threads)
        {
            draws   += ctx->draws;
            indices += ctx->indices;
        }
        Benchmark::recorder.SetFrameStats(draws, indices);
        m_RenderedThisFrame = false;
    }

    if (m_Desc.gpuTiming) PollGpuTimers();

    if (!Benchmark::recorder.BeginFrame())
    {
        // The measured duration has elapsed: leave Donut's message loop.
        m_Done = true;
        glfwSetWindowShouldClose(GetDeviceManager()->GetWindow(), GLFW_TRUE);
    }
    m_FrameStartStamp = Benchmark::Now();
}

void AsteroidsRenderer::FillRunInfo(Benchmark::RunInfo& info) const
{
    info.width     = int(m_Width);
    info.height    = int(m_Height);
    info.threads   = int(m_Threads.size());
    info.asteroids = NUM_ASTEROIDS;
    info.meshes    = NUM_UNIQUE_MESHES;
    info.textures  = NUM_UNIQUE_TEXTURES;

    auto add = [&](const char* key, const std::string& value) { info.extra.emplace_back(key, value); };
    const auto& params = GetDeviceManager()->GetDeviceParams();
    add("nvrhi_binding", BindingModeName(m_Desc.binding));
    add("texture_binding_sets", std::to_string(m_TextureSets.size()));
    add("track_liveness", m_Desc.trackLiveness ? "true" : "false");
    add("always_set_state", m_Desc.alwaysSetState ? "true" : "false");
    add("auto_barriers", m_Desc.noAutoBarriers ? "off after the first setGraphicsState of each command list" : "on");
    add("no_auto_barriers", m_Desc.noAutoBarriers ? "true" : "false");
    add("redundant_binding_set_filter", m_Desc.alwaysSetState ? "off" : "on");
    add("upload_chunk_size", std::to_string(m_UploadChunkSize));
    {
        // Page-split store condition of this process (docs/LIMITATIONS.md). cb_page_split is what the analysis
        // groups by: "true" when the command list of at least one recording thread is affected.
        const char* mode = m_Desc.cbPageSplit == RendererDesc::CbPageSplit::Avoid ? "avoid"
                         : m_Desc.cbPageSplit == RendererDesc::CbPageSplit::Force ? "force" : "natural";
        int split = 0, unknown = 0;
        for (auto& ctx : m_Threads)
        {
            if (ctx->cbPageSplit > 0) ++split;
            if (ctx->cbPageSplit < 0) ++unknown;
        }
        add("cb_page_split_mode", mode);
        add("cb_page_split_avoid", m_Desc.cbPageSplit == RendererDesc::CbPageSplit::Avoid ? "true" : "false");
        add("cb_page_split_force", m_Desc.cbPageSplit == RendererDesc::CbPageSplit::Force ? "true" : "false");
        add("cb_page_split", m_Api != nvrhi::GraphicsAPI::D3D12 ? "n/a" : unknown ? "unknown" : split ? "true" : "false");
        add("cb_page_split_command_lists", std::to_string(split));
        add("cb_page_split_state_offset", std::to_string(m_CbStateOffset));
        add("cb_page_split_candidates", std::to_string(m_PlacementCandidates));
        std::string offsets; // of every recording thread's command list object inside its 4 KB page
        for (auto& ctx : m_Threads)
            offsets += (offsets.empty() ? "" : " ") +
                       std::to_string(uint64_t(reinterpret_cast<uintptr_t>(ctx->commandList.Get())) & (kPageSize - 1));
        add("command_list_page_offsets", offsets);
        add("draw_constants_page_offset", m_DrawConstantsAddress ? std::to_string(m_DrawConstantsAddress & (kPageSize - 1)) : "n/a");
        if (!m_UploadHeapProtection.empty()) add("d3d12_upload_heap_page_protection", m_UploadHeapProtection);
    }
    add("volatile_cb_max_versions", std::to_string(m_VolatileVersions));
    add("command_lists_per_frame", std::to_string(m_Threads.size() + (m_Threads.size() > 1 ? 1 : 0)));
    {
        // How Donut presents with vsync off (the renderer cannot choose it):
        //   D3D12   Donut/src/app/dx12/DeviceManager_DX12.cpp: flip-discard, DXGI_PRESENT_ALLOW_TEARING when
        //           DXGI_FEATURE_PRESENT_ALLOW_TEARING is supported (the same query is repeated here)
        //   D3D11   Donut/src/app/dx11/DeviceManager_DX11.cpp: Present(0, 0), no tearing flag
        //   Vulkan  Donut/src/app/vulkan/DeviceManager_VK.cpp: VK_PRESENT_MODE_IMMEDIATE_KHR
        bool tearing = false;
        nvrhi::AutoPtr<IDXGIFactory5> factory;
        BOOL supported = FALSE;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &supported, sizeof(supported))))
            tearing = supported != FALSE;
        switch (m_Api)
        {
            case nvrhi::GraphicsAPI::D3D12:
                add("present_mode", tearing ? "DXGI flip-discard, sync interval 0, DXGI_PRESENT_ALLOW_TEARING"
                                            : "DXGI flip-discard, sync interval 0, tearing not supported");
                add("swap_chain_tearing", tearing ? "true" : "false");
                break;
            case nvrhi::GraphicsAPI::D3D11:
                add("present_mode", "DXGI, sync interval 0, no tearing flag");
                add("swap_chain_tearing", "false");
                break;
            default:
                add("present_mode", "VK_PRESENT_MODE_IMMEDIATE_KHR");
                add("swap_chain_tearing", "true");
                break;
        }
    }
    add("swap_chain_buffers", std::to_string(GetDeviceManager()->GetBackBufferCount()));
    add("swap_chain_buffers_requested", std::to_string(params.swapChainBufferCount));
    add("max_frames_in_flight", std::to_string(params.maxFramesInFlight));
    add("back_buffer_format", nvrhi::getFormatInfo(GetDeviceManager()->GetBackBuffer(0)->getDesc().format).name);
    add("median_gc_ms", std::to_string(Benchmark::Median(m_GcSamples)));
    add("threading", m_Threads.size() > 1 ? "persistent workers, one nvrhi command list per thread + one for the skybox, one executeCommandLists"
                                          : "one nvrhi command list");
    add("columns", "render = clears + recording of all command lists (workers close theirs) + skybox; submit = close of the main "
                   "thread's command lists + IDevice::executeCommandLists; wait = DeviceManager::BeginFrame (D3D12 frame fence "
                   "wait, Vulkan vkAcquireNextImageKHR) + Donut's sleep_for(0) and per-frame runGarbageCollection (the gc_ms "
                   "column, median_gc_ms); "
                   "present = DeviceManager::Present (Vulkan: includes the semaphore-signal submission and Donut's wait for the "
                   "frame maxFramesInFlight ago; D3D12: includes the frame fence Signal)");
    if (m_Desc.gpuTiming)
    {
        add("gpu_timing_source", "nvrhi timer queries");
        add("gpu_frames_without_query", std::to_string(m_GpuFramesSkipped));
    }
}

void AsteroidsRenderer::Shutdown()
{
    if (!m_Threads.empty())
    {
        bool anyWorker = false;
        for (auto& ctx : m_Threads) anyWorker = anyWorker || ctx->thread.joinable();
        if (anyWorker)
        {
            Dispatch(Job::Exit);
            for (auto& ctx : m_Threads)
                if (ctx->thread.joinable()) ctx->thread.join();
        }
    }
    if (m_Device) m_Device->waitForIdle();

    m_PendingQueries.clear();
    m_FreeQueries.clear();
    m_ActiveQuery = nullptr;
    BackBufferResizing();
    m_ExecuteList.clear();
    m_PostCommandList = nullptr;
    m_RetiredCommandLists.clear();
    m_Threads.clear();
    m_TextureSetPtrs.clear();
    m_TextureSets.clear();
    m_TextureTable = nullptr;
    m_SkyboxState = nvrhi::GraphicsState();
    m_SkyboxBindingSet = nullptr;
    m_SkyboxConstantBuffer = nullptr;
    m_AsteroidPipeline = nullptr;
    m_SkyboxPipeline = nullptr;
    m_Layout0 = m_Layout1 = m_SkyboxLayout = nullptr;
    m_AsteroidInputLayout = m_SkyboxInputLayout = nullptr;
    m_AsteroidVS = m_AsteroidPS = m_SkyboxVS = m_SkyboxPS = nullptr;
    m_Sampler = nullptr;
    m_SkyboxTexture = nullptr;
    for (auto& texture : m_Textures) texture = nullptr;
    m_VertexBuffer = m_IndexBuffer = m_SkyboxVertexBuffer = m_InstanceIdBuffer = nullptr;
    if (m_Device)
    {
        m_Device->runGarbageCollection();
        m_Device = nullptr;
    }
}
