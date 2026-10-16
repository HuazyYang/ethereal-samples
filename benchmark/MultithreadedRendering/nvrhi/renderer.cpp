#include "renderer.h"

#include <donut/core/log.h>
#include <nvrhi/utils.h>

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include <algorithm>
#include <cstring>

#include "dds.h"
#include "frame_scheduler.h"

using namespace MtScene;

namespace
{

nvrhi::Format ToNvrhiFormat(DXGI_FORMAT format)
{
    switch (format)
    {
        case DXGI_FORMAT_BC1_UNORM: return nvrhi::Format::BC1_UNORM;
        case DXGI_FORMAT_BC1_UNORM_SRGB: return nvrhi::Format::BC1_UNORM_SRGB;
        case DXGI_FORMAT_BC2_UNORM: return nvrhi::Format::BC2_UNORM;
        case DXGI_FORMAT_BC2_UNORM_SRGB: return nvrhi::Format::BC2_UNORM_SRGB;
        case DXGI_FORMAT_BC3_UNORM: return nvrhi::Format::BC3_UNORM;
        case DXGI_FORMAT_BC3_UNORM_SRGB: return nvrhi::Format::BC3_UNORM_SRGB;
        case DXGI_FORMAT_BC4_UNORM: return nvrhi::Format::BC4_UNORM;
        case DXGI_FORMAT_BC5_UNORM: return nvrhi::Format::BC5_UNORM;
        case DXGI_FORMAT_BC7_UNORM: return nvrhi::Format::BC7_UNORM;
        case DXGI_FORMAT_BC7_UNORM_SRGB: return nvrhi::Format::BC7_UNORM_SRGB;
        case DXGI_FORMAT_R8G8B8A8_UNORM: return nvrhi::Format::RGBA8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return nvrhi::Format::SRGBA8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM: return nvrhi::Format::BGRA8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return nvrhi::Format::SBGRA8_UNORM;
        default: return nvrhi::Format::UNKNOWN;
    }
}

nvrhi::PrimitiveType ToPrimitiveType(uint32_t sdkmeshPrimitiveType)
{
    switch (sdkmeshPrimitiveType)
    {
        case SDKMESH_PT_TRIANGLE_LIST: return nvrhi::PrimitiveType::TriangleList;
        case SDKMESH_PT_TRIANGLE_STRIP: return nvrhi::PrimitiveType::TriangleStrip;
        case SDKMESH_PT_LINE_LIST: return nvrhi::PrimitiveType::LineList;
        case SDKMESH_PT_LINE_STRIP: return nvrhi::PrimitiveType::LineStrip;
        case SDKMESH_PT_POINT_LIST: return nvrhi::PrimitiveType::PointList;
        case SDKMESH_PT_TRIANGLE_LIST_ADJ: return nvrhi::PrimitiveType::TriangleListWithAdjacency;
        case SDKMESH_PT_TRIANGLE_STRIP_ADJ: return nvrhi::PrimitiveType::TriangleStripWithAdjacency;
        default: return nvrhi::PrimitiveType::TriangleList;
    }
}

std::wstring Widen(const char* s)
{
    wchar_t buffer[MAX_PATH]{};
    MultiByteToWideChar(CP_ACP, 0, s, -1, buffer, MAX_PATH - 1);
    return buffer;
}

constexpr uint32_t kConstantBufferVersions = 64; // writes per command list per frame x frames in flight

} // namespace

MtRenderer::MtRenderer(donut::app::DeviceManager* deviceManager, const RendererDesc& desc, Scene* scene)
    : IRenderPass(deviceManager)
    , m_Desc(desc)
    , m_Mode(desc.mode)
    , m_Threads(std::clamp(desc.threads, 1u, kMaxThreads))
    , m_Device(deviceManager->GetDevice())
    , m_Api(deviceManager->GetDevice()->getGraphicsAPI())
    , m_Scene(scene)
{
}

MtRenderer::~MtRenderer()
{
    Shutdown();
}

void MtRenderer::Shutdown()
{
    m_Workers.reset();
    if (m_Device)
        m_Device->waitForIdle();
    m_Chunks.clear();
    for (auto& p : m_Pipelines) p = nullptr;
    m_MaterialSets.clear();
    m_MaterialSetPtrs.clear();
    m_Textures.clear();
    m_MeshBuffers.clear();
    for (auto& f : m_ShadowFramebuffers) f = nullptr;
    for (auto& t : m_ShadowMaps) t = nullptr;
}

bool MtRenderer::Init(donut::engine::ShaderFactory& shaderFactory, const SdkMesh& mesh)
{
    if (m_Desc.debug && m_Api == nvrhi::GraphicsAPI::D3D12)
        m_DebugMessages.Init(static_cast<ID3D12Device*>(m_Device->getNativeObject(nvrhi::ObjectTypes::D3D12_Device)));
    m_VertexShader = shaderFactory.CreateShader("app/mt_scene.hlsl", "VSMain", nullptr, nvrhi::ShaderType::Vertex);
    m_PixelShader = shaderFactory.CreateShader("app/mt_scene.hlsl", "PSMain", nullptr, nvrhi::ShaderType::Pixel);
    if (!m_VertexShader || !m_PixelShader)
    {
        donut::log::error("mt_scene.hlsl shaders not found");
        return false;
    }

    // The two input layouts of the sample.
    {
        nvrhi::VertexAttributeDesc compressed[] = {
            nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RGB32_FLOAT).setOffset(0).setElementStride(24),
            nvrhi::VertexAttributeDesc().setName("NORMAL").setFormat(nvrhi::Format::R10G10B10A2_UNORM).setOffset(12).setElementStride(24),
            nvrhi::VertexAttributeDesc().setName("TEXCOORD").setFormat(nvrhi::Format::RG16_FLOAT).setOffset(16).setElementStride(24),
            nvrhi::VertexAttributeDesc().setName("TANGENT").setFormat(nvrhi::Format::R10G10B10A2_UNORM).setOffset(20).setElementStride(24),
        };
        m_Device->createInputLayout(compressed, uint32_t(std::size(compressed)), m_VertexShader, &m_CompressedLayout);
        nvrhi::VertexAttributeDesc uncompressed[] = {
            nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RGB32_FLOAT).setOffset(0).setElementStride(sizeof(MirrorVertex)),
            nvrhi::VertexAttributeDesc().setName("NORMAL").setFormat(nvrhi::Format::RGB32_FLOAT).setOffset(12).setElementStride(sizeof(MirrorVertex)),
            nvrhi::VertexAttributeDesc().setName("TEXCOORD").setFormat(nvrhi::Format::RG32_FLOAT).setOffset(24).setElementStride(sizeof(MirrorVertex)),
            nvrhi::VertexAttributeDesc().setName("TANGENT").setFormat(nvrhi::Format::RGB32_FLOAT).setOffset(32).setElementStride(sizeof(MirrorVertex)),
        };
        m_Device->createInputLayout(uncompressed, uint32_t(std::size(uncompressed)), m_VertexShader, &m_UncompressedLayout);
    }

    // Binding layouts: set 0 = constants, shadow map, samplers (per recording chunk); set 1 = the
    // material's two textures (per material).
    {
        // Every layout of a pipeline must agree on registerSpaceIsDescriptorSet: layout 1 lives in space1 / set 1.
        nvrhi::BindingLayoutDesc layout0;
        layout0.visibility = nvrhi::ShaderType::All;
        layout0.registerSpaceIsDescriptorSet = true;
        layout0.bindings = {
            nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
            nvrhi::BindingLayoutItem::VolatileConstantBuffer(1),
            nvrhi::BindingLayoutItem::VolatileConstantBuffer(2),
            nvrhi::BindingLayoutItem::Texture_SRV(2),
            nvrhi::BindingLayoutItem::Sampler(0),
            nvrhi::BindingLayoutItem::Sampler(1),
        };
        m_Device->createBindingLayout(layout0, &m_Layout0);
        // Vertex-only pipelines (shadow map, mirror stencil / depth clear / surface) read b0 and b2 only; nvrhi
        // rejects a bound volatile buffer that the list did not write, so they get a layout without b1.
        nvrhi::BindingLayoutDesc layout0NoLights;
        layout0NoLights.visibility = nvrhi::ShaderType::All;
        layout0NoLights.registerSpaceIsDescriptorSet = true;
        layout0NoLights.bindings = {
            nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
            nvrhi::BindingLayoutItem::VolatileConstantBuffer(2),
        };
        m_Device->createBindingLayout(layout0NoLights, &m_Layout0NoLights);
        nvrhi::BindingLayoutDesc layout1;
        layout1.visibility = nvrhi::ShaderType::Pixel;
        layout1.registerSpace = 1; // space1 in mt_scene.hlsl: descriptor set 1 on Vulkan
        layout1.registerSpaceIsDescriptorSet = true;
        layout1.bindings = {
            nvrhi::BindingLayoutItem::Texture_SRV(0),
            nvrhi::BindingLayoutItem::Texture_SRV(1),
        };
        m_Device->createBindingLayout(layout1, &m_Layout1);
    }

    // s0: point clamp, s1: linear clamp (the sample's static samplers)
    {
        nvrhi::SamplerDesc point;
        point.setAllFilters(false).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
        m_Device->createSampler(point, &m_PointClampSampler);
        nvrhi::SamplerDesc linear;
        linear.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
        m_Device->createSampler(linear, &m_LinearClampSampler);
    }

    CreateShadowMaps();

    nvrhi::CommandListHandle uploadList;
    m_Device->createCommandList(nvrhi::CommandListParameters(), &uploadList);
    uploadList->open();
    const bool loaded = LoadModel(mesh, uploadList);
    uploadList->close();
    m_Device->executeCommandList(uploadList);
    m_Device->waitForIdle();
    if (!loaded)
        return false;

    CreateChunkContexts();
    if (m_Threads > 1)
        m_Workers = std::make_unique<WorkerPool>(m_Threads - 1);
    return true;
}

void MtRenderer::CreateShadowMaps()
{
    for (int i = 0; i < kNumShadows; ++i)
    {
        nvrhi::TextureDesc desc;
        desc.width = kShadowMapSize;
        desc.height = kShadowMapSize;
        desc.format = nvrhi::Format::D32;
        desc.isRenderTarget = true;
        desc.isTypeless = true;
        desc.useClearValue = true;
        desc.clearValue = nvrhi::Color(1.f);
        // Every list that writes the map transitions it and restores the shader-resource state at close
        // (the shadow pass lists keep nvrhi's automatic barriers in both barrier modes).
        desc.initialState = nvrhi::ResourceStates::ShaderResource;
        desc.keepInitialState = true;
        desc.debugName = "ShadowMap";
        m_Device->createTexture(desc, &m_ShadowMaps[i]);

        nvrhi::FramebufferDesc fb;
        fb.setDepthAttachment(m_ShadowMaps[i]);
        m_Device->createFramebuffer(fb, &m_ShadowFramebuffers[i]);
    }
}

nvrhi::TextureHandle MtRenderer::CreateSolidTexture(nvrhi::ICommandList* commandList, uint32_t rgba, const char* name)
{
    nvrhi::TextureDesc desc;
    desc.width = desc.height = 1;
    desc.format = nvrhi::Format::RGBA8_UNORM;
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    desc.debugName = name;
    nvrhi::TextureHandle texture;
    m_Device->createTexture(desc, &texture);
    commandList->writeTexture(texture, 0, 0, &rgba, 4);
    return texture;
}

bool MtRenderer::LoadModel(const SdkMesh& mesh, nvrhi::ICommandList* commandList)
{
    // One buffer per sdkmesh vertex / index buffer, as DXUT's loader creates them.
    std::vector<nvrhi::BufferHandle> vertexBuffers(mesh.NumVertexBuffers()), indexBuffers(mesh.NumIndexBuffers());
    for (uint32_t i = 0; i < mesh.NumVertexBuffers(); ++i)
    {
        nvrhi::BufferDesc desc;
        desc.byteSize = mesh.VertexBuffer(i).SizeBytes;
        desc.isVertexBuffer = true;
        desc.initialState = nvrhi::ResourceStates::CopyDest;
        desc.debugName = "VertexBuffer";
        m_Device->createBuffer(desc, &vertexBuffers[i]);
        commandList->beginTrackingBufferState(vertexBuffers[i], nvrhi::ResourceStates::CopyDest);
        commandList->writeBuffer(vertexBuffers[i], mesh.VertexData(i), desc.byteSize);
        commandList->setPermanentBufferState(vertexBuffers[i], nvrhi::ResourceStates::VertexBuffer);
    }
    for (uint32_t i = 0; i < mesh.NumIndexBuffers(); ++i)
    {
        nvrhi::BufferDesc desc;
        desc.byteSize = mesh.IndexBuffer(i).SizeBytes;
        desc.isIndexBuffer = true;
        desc.initialState = nvrhi::ResourceStates::CopyDest;
        desc.debugName = "IndexBuffer";
        m_Device->createBuffer(desc, &indexBuffers[i]);
        commandList->beginTrackingBufferState(indexBuffers[i], nvrhi::ResourceStates::CopyDest);
        commandList->writeBuffer(indexBuffers[i], mesh.IndexData(i), desc.byteSize);
        commandList->setPermanentBufferState(indexBuffers[i], nvrhi::ResourceStates::IndexBuffer);
    }

    m_MeshBuffers.resize(mesh.NumMeshes());
    for (uint32_t i = 0; i < mesh.NumMeshes(); ++i)
    {
        const SdkMeshMesh& m = mesh.Mesh(i);
        m_MeshBuffers[i].vertexBuffer = vertexBuffers[m.VertexBuffers[0]];
        m_MeshBuffers[i].indexBuffer = indexBuffers[m.IndexBuffer];
        m_MeshBuffers[i].indexFormat = mesh.IndexBuffer(m.IndexBuffer).IndexType == SDKMESH_IT_32BIT ? nvrhi::Format::R32_UINT : nvrhi::Format::R16_UINT;
    }

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
            m_Subsets.push_back({ ToPrimitiveType(subset.PrimitiveType), subset.MaterialID, uint32_t(subset.IndexCount), uint32_t(subset.IndexStart),
                                  uint32_t(subset.VertexStart) });
        }
        m_RenderOrder.push_back(draw);
    }

    // Textures: diffuse maps as sRGB, normal maps linear; 1x1 fallbacks for missing files.
    nvrhi::TextureHandle white = CreateSolidTexture(commandList, 0xffffffffu, "white");
    nvrhi::TextureHandle flatNormal = CreateSolidTexture(commandList, 0xffff8080u, "flat normal");
    m_Textures.push_back(white);
    m_Textures.push_back(flatNormal);

    struct Loaded { std::wstring name; bool srgb; nvrhi::TextureHandle texture; };
    std::vector<Loaded> cache;
    auto load = [&](const char* fileName, bool srgb, nvrhi::ITexture* fallback) -> nvrhi::ITexture* {
        if (!fileName[0]) return fallback;
        const std::wstring name = Widen(fileName);
        for (const Loaded& l : cache)
            if (l.name == name && l.srgb == srgb) return l.texture;
        DdsImage image;
        std::string error;
        if (!image.Load(mesh.Directory() / name, error))
        {
            donut::log::warning("%s", error.c_str());
            cache.push_back({ name, srgb, nvrhi::TextureHandle(fallback) });
            return fallback;
        }
        const nvrhi::Format format = ToNvrhiFormat(srgb ? DdsImage::MakeSRGB(image.Format()) : image.Format());
        if (format == nvrhi::Format::UNKNOWN)
        {
            donut::log::warning("unsupported texture format: %s", fileName);
            cache.push_back({ name, srgb, nvrhi::TextureHandle(fallback) });
            return fallback;
        }
        nvrhi::TextureDesc desc;
        desc.width = image.Width();
        desc.height = image.Height();
        desc.mipLevels = image.MipLevels();
        desc.format = format;
        desc.initialState = nvrhi::ResourceStates::ShaderResource;
        desc.keepInitialState = true;
        desc.debugName = fileName;
        nvrhi::TextureHandle texture;
        m_Device->createTexture(desc, &texture);
        for (uint32_t mip = 0; mip < image.MipLevels(); ++mip)
        {
            const DdsSubresource& sub = image.Mip(mip);
            commandList->writeTexture(texture, 0, mip, sub.data, sub.rowPitch);
        }
        m_Textures.push_back(texture);
        cache.push_back({ name, srgb, texture });
        return texture;
    };

    m_MaterialSets.resize(mesh.NumMaterials());
    m_MaterialSetPtrs.resize(mesh.NumMaterials());
    for (uint32_t i = 0; i < mesh.NumMaterials(); ++i)
    {
        const SdkMeshMaterial& mat = mesh.Material(i);
        nvrhi::BindingSetDesc set;
        set.bindings = {
            nvrhi::BindingSetItem::Texture_SRV(0, load(mat.DiffuseTexture, true, white)),
            nvrhi::BindingSetItem::Texture_SRV(1, load(mat.NormalTexture, false, flatNormal)),
        };
        set.trackLiveness = false; // the sets outlive every command list
        m_Device->createBindingSet(set, m_Layout1, &m_MaterialSets[i]);
        m_MaterialSetPtrs[i] = m_MaterialSets[i];
    }

    // Mirror quads: one vertex buffer, four strips.
    {
        MirrorVertex vertices[4 * kNumMirrors];
        for (int i = 0; i < kNumMirrors; ++i)
            memcpy(&vertices[4 * i], m_Scene->GetMirror(i).vertices, sizeof(MirrorVertex) * 4);
        nvrhi::BufferDesc desc;
        desc.byteSize = sizeof(vertices);
        desc.isVertexBuffer = true;
        desc.initialState = nvrhi::ResourceStates::CopyDest;
        desc.debugName = "MirrorVertices";
        m_Device->createBuffer(desc, &m_MirrorVertexBuffer);
        commandList->beginTrackingBufferState(m_MirrorVertexBuffer, nvrhi::ResourceStates::CopyDest);
        commandList->writeBuffer(m_MirrorVertexBuffer, vertices, sizeof(vertices));
        commandList->setPermanentBufferState(m_MirrorVertexBuffer, nvrhi::ResourceStates::VertexBuffer);
    }
    return true;
}

void MtRenderer::CreateChunkContexts()
{
    m_Chunks.clear();
    for (uint32_t c = 0; c < m_Threads; ++c)
    {
        auto ctx = std::make_unique<ChunkContext>();
        for (int pass = 0; pass < kNumPasses; ++pass)
            m_Device->createCommandList(nvrhi::CommandListParameters().setEnableImmediateExecution(false), &ctx->lists[pass]);
        const char* names[] = { "CbPerObject", "CbPerLight", "CbPerScene" };
        nvrhi::BufferHandle* buffers[] = { &ctx->cbObject, &ctx->cbLight, &ctx->cbScene };
        const uint32_t sizes[] = { sizeof(CbPerObject), sizeof(CbPerLight), sizeof(CbPerScene) };
        for (int i = 0; i < 3; ++i)
            m_Device->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(sizes[i], names[i], kConstantBufferVersions), &(*buffers[i]));

        nvrhi::BindingSetDesc set;
        set.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, ctx->cbObject),
            nvrhi::BindingSetItem::ConstantBuffer(1, ctx->cbLight),
            nvrhi::BindingSetItem::ConstantBuffer(2, ctx->cbScene),
            nvrhi::BindingSetItem::Texture_SRV(2, m_ShadowMaps[0]),
            nvrhi::BindingSetItem::Sampler(0, m_PointClampSampler),
            nvrhi::BindingSetItem::Sampler(1, m_LinearClampSampler),
        };
        set.trackLiveness = false;
        m_Device->createBindingSet(set, m_Layout0, &ctx->set0);
        nvrhi::BindingSetDesc setNoLights;
        setNoLights.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, ctx->cbObject),
            nvrhi::BindingSetItem::ConstantBuffer(2, ctx->cbScene),
        };
        setNoLights.trackLiveness = false;
        m_Device->createBindingSet(setNoLights, m_Layout0NoLights, &ctx->set0NoLights);
        m_Chunks.push_back(std::move(ctx));
    }
}

void MtRenderer::CreatePipelines(nvrhi::IFramebuffer* framebuffer)
{
    using namespace nvrhi;
    const FramebufferInfoEx& fbinfo = framebuffer->getFramebufferInfo();
    m_PipelineFramebufferInfo = fbinfo;

    auto base = [&](bool withPixelShader, IInputLayout* layout) {
        GraphicsPipelineDesc d;
        d.primType = PrimitiveType::TriangleList;
        d.inputLayout = layout;
        d.VS = m_VertexShader;
        if (withPixelShader) d.PS = m_PixelShader;
        d.bindingLayouts = { withPixelShader ? m_Layout0 : m_Layout0NoLights, m_Layout1 };
        d.renderState.rasterState.depthClipEnable = true;
        d.renderState.rasterState.cullMode = RasterCullMode::Back;
        d.renderState.depthStencilState.depthTestEnable = true;
        d.renderState.depthStencilState.depthWriteEnable = true;
        d.renderState.depthStencilState.depthFunc = ComparisonFunc::Less;
        if (!withPixelShader)
            d.renderState.blendState.targets[0].colorWriteMask = ColorMask(0); // depth / stencil only
        return d;
    };
    auto stencilOp = [](StencilOp fail, StencilOp depthFail, StencilOp pass, ComparisonFunc func) {
        DepthStencilState::StencilOpDesc op;
        op.failOp = fail;
        op.depthFailOp = depthFail;
        op.passOp = pass;
        op.stencilFunc = func;
        return op;
    };
    const auto keepAll = stencilOp(StencilOp::Keep, StencilOp::Keep, StencilOp::Keep, ComparisonFunc::Never);

    // Shadow map: depth only, biased.
    {
        GraphicsPipelineDesc d = base(false, m_CompressedLayout);
        d.renderState.rasterState.depthBias = 1 << 8;
        d.renderState.rasterState.slopeScaledDepthBias = 1.E-4f;
        // Donut's Vulkan device does not enable the depthBiasClamp feature; the clamp must then be 0.
        d.renderState.rasterState.depthBiasClamp = m_Api == nvrhi::GraphicsAPI::VULKAN ? 0.f : 1.E-2f;
        d.renderState.depthStencilState.depthFunc = ComparisonFunc::LessOrEqual;
        m_Device->createGraphicsPipeline2(d, m_ShadowFramebuffers[0], &m_Pipelines[PipelineShadow]);
    }
    // The direct view.
    {
        GraphicsPipelineDesc d = base(true, m_CompressedLayout);
        m_Device->createGraphicsPipeline2(d, framebuffer, &m_Pipelines[PipelineDirect]);
    }

    // The three mirror quad pipelines draw 4-vertex triangle strips.
    GraphicsPipelineDesc stencilWrite = base(false, m_UncompressedLayout);
    stencilWrite.primType = PrimitiveType::TriangleStrip;
    stencilWrite.renderState.depthStencilState.depthWriteEnable = false;
    stencilWrite.renderState.depthStencilState.stencilEnable = true;
    stencilWrite.renderState.depthStencilState.frontFaceStencil = stencilOp(StencilOp::Replace, StencilOp::Keep, StencilOp::Replace, ComparisonFunc::Always);
    stencilWrite.renderState.depthStencilState.backFaceStencil = stencilWrite.renderState.depthStencilState.frontFaceStencil;

    GraphicsPipelineDesc clearDepth = base(false, m_UncompressedLayout);
    clearDepth.primType = PrimitiveType::TriangleStrip;
    clearDepth.renderState.depthStencilState.stencilEnable = true;
    clearDepth.renderState.depthStencilState.frontFaceStencil = stencilOp(StencilOp::Keep, StencilOp::Keep, StencilOp::Keep, ComparisonFunc::Equal);
    clearDepth.renderState.depthStencilState.backFaceStencil = keepAll;

    GraphicsPipelineDesc mirrored = base(true, m_CompressedLayout);
    mirrored.renderState.rasterState.frontCounterClockwise = true;
    mirrored.renderState.depthStencilState.depthFunc = ComparisonFunc::LessOrEqual;
    mirrored.renderState.depthStencilState.stencilEnable = true;
    mirrored.renderState.depthStencilState.frontFaceStencil = stencilOp(StencilOp::Keep, StencilOp::Keep, StencilOp::Keep, ComparisonFunc::Equal);
    mirrored.renderState.depthStencilState.backFaceStencil = keepAll;

    GraphicsPipelineDesc surface = base(false, m_UncompressedLayout);
    surface.primType = PrimitiveType::TriangleStrip;
    surface.renderState.depthStencilState.stencilEnable = true;
    surface.renderState.depthStencilState.frontFaceStencil = stencilOp(StencilOp::Zero, StencilOp::Keep, StencilOp::Zero, ComparisonFunc::Equal);
    surface.renderState.depthStencilState.backFaceStencil = keepAll;

    for (int i = 0; i < 8; ++i)
    {
        const uint8_t bit = uint8_t(1u << i);
        auto masks = [&](GraphicsPipelineDesc& d, uint8_t read, uint8_t write) {
            d.renderState.depthStencilState.stencilReadMask = read;
            d.renderState.depthStencilState.stencilWriteMask = write;
            d.renderState.depthStencilState.stencilRefValue = bit; // the sample sets the ref per pass; it is constant per variant
        };
        masks(stencilWrite, 0, bit);
        masks(clearDepth, bit, 0);
        masks(mirrored, bit, 0);
        masks(surface, bit, bit);
        m_Device->createGraphicsPipeline2(stencilWrite, framebuffer, &m_Pipelines[PipelineMirrorStencil + i]);
        m_Device->createGraphicsPipeline2(clearDepth, framebuffer, &m_Pipelines[PipelineMirrorClearDepth + i]);
        m_Device->createGraphicsPipeline2(mirrored, framebuffer, &m_Pipelines[PipelineMirroredScene + i]);
        m_Device->createGraphicsPipeline2(surface, framebuffer, &m_Pipelines[PipelineMirrorSurface + i]);
    }
}

// ---------------------------------------------------------------------------------------------
// IRenderPass
// ---------------------------------------------------------------------------------------------

void MtRenderer::Animate(float elapsedTimeSeconds)
{
    // Benchmark mode: wall clock since the first frame (same camera path for every renderer, see the native host).
    if (Benchmark::config.enabled)
    {
        if (m_StartStamp == 0) m_StartStamp = Benchmark::Now();
        m_Time = float(Benchmark::Seconds(Benchmark::Now() - m_StartStamp));
    }
    else
        m_Time += elapsedTimeSeconds;
    if (m_Desc.freezeTime >= 0.f) m_Scene->Animate(m_Desc.freezeTime, true);
    else m_Scene->Animate(m_Time, Benchmark::config.enabled);
}

void MtRenderer::BackBufferResizing()
{
    for (auto& p : m_Pipelines) p = nullptr;
}

void MtRenderer::BackBufferResized(uint32_t width, uint32_t height, uint32_t)
{
    m_Width = width;
    m_Height = height;
    m_Scene->SetViewport(width, height);
}

bool MtRenderer::KeyboardUpdate(int key, int, int action, int)
{
    if (Benchmark::config.enabled || action != GLFW_PRESS) return false;
    switch (key)
    {
        case GLFW_KEY_ESCAPE: glfwSetWindowShouldClose(GetDeviceManager()->GetWindow(), GLFW_TRUE); return true;
        case GLFW_KEY_1: if (m_Threads == 1) m_Mode = Mode::SingleThreaded; return true;
        case GLFW_KEY_2: if (m_Threads > 1) m_Mode = Mode::PerScene; return true;
        case GLFW_KEY_3: if (m_Threads > 1) m_Mode = Mode::PerChunk; return true;
        case GLFW_KEY_M: m_Scene->SetTightMirrorScissor(!m_Scene->TightMirrorScissor()); return true;
    }
    return false;
}

bool MtRenderer::MousePosUpdate(double xpos, double ypos)
{
    if (m_Dragging && !Benchmark::config.enabled)
        m_Scene->Orbit(float(xpos - m_MouseX) * 0.005f, float(ypos - m_MouseY) * 0.005f);
    m_MouseX = xpos;
    m_MouseY = ypos;
    return true;
}

bool MtRenderer::MouseButtonUpdate(int button, int action, int)
{
    if (button == GLFW_MOUSE_BUTTON_LEFT)
        m_Dragging = action == GLFW_PRESS;
    return true;
}

bool MtRenderer::MouseScrollUpdate(double, double yoffset)
{
    if (!Benchmark::config.enabled)
        m_Scene->Zoom(-12.f * float(yoffset));
    return true;
}

void MtRenderer::Render(nvrhi::IFramebuffer* framebuffer)
{
    const nvrhi::FramebufferInfoEx& fbinfo = framebuffer->getFramebufferInfo();
    if (!m_Pipelines[0] || fbinfo.width != m_Width || fbinfo.height != m_Height)
    {
        m_Width = fbinfo.width;
        m_Height = fbinfo.height;
        m_Scene->SetViewport(m_Width, m_Height);
        CreatePipelines(framebuffer);
    }
    m_FrameFramebuffer = framebuffer;

    Benchmark::FrameTimes& times = Benchmark::recorder.Times();
    {
        Benchmark::ScopedTimer update(times.update);
        for (int s = 0; s < kNumShadows; ++s) m_Params.shadow[s] = m_Scene->ShadowPass(s);
        for (int m = 0; m < kNumMirrors; ++m) m_Params.mirror[m] = m_Scene->MirrorPassParams(m);
        m_Params.direct = m_Scene->DirectPass();
        m_Scene->FillLightConstants(m_Params.lights);
        for (auto& ctx : m_Chunks)
            for (auto& s : ctx->stats) s = ListStats{};
    }

    const uint32_t chunkCount = m_Mode == Mode::PerChunk ? m_Threads : 1;
    static WorkerPool noWorkers(0);
    RunFrame(*this, m_Mode, chunkCount, m_Workers ? *m_Workers : noWorkers, times);

    m_LastDraws = m_LastIndices = 0;
    for (auto& ctx : m_Chunks)
        for (const ListStats& s : ctx->stats)
        {
            m_LastDraws += s.draws;
            m_LastIndices += s.indices;
        }
    Benchmark::recorder.SetFrameStats(m_LastDraws, m_LastIndices);
    m_DebugMessages.Drain();

    if (!m_Desc.screenshot.empty() && ++m_RenderedFrames == 30)
    {
        std::string error;
        if (!SaveBackBuffer(framebuffer, m_Desc.screenshot, error)) donut::log::error("%s", error.c_str());
        else printf("screenshot -> %s\n", m_Desc.screenshot.string().c_str());
        glfwSetWindowShouldClose(GetDeviceManager()->GetWindow(), GLFW_TRUE);
    }

    if (!Benchmark::config.enabled)
    {
        char info[160];
        snprintf(info, sizeof(info), "%s | %s | %u threads | render %.2f ms submit %.2f ms | %llu draws%s%s",
                 GetDeviceManager()->GetRendererString(), ModeName(m_Mode), m_Threads, times.render, times.submit,
                 static_cast<unsigned long long>(m_LastDraws), m_Scene->TightMirrorScissor() ? " | tight scissor" : "",
                 m_Desc.explicitBarriers ? " | explicit barriers" : "");
        GetDeviceManager()->SetInformativeWindowTitle("MultithreadedRendering nvrhi", true, info);
    }
}

bool MtRenderer::SaveBackBuffer(nvrhi::IFramebuffer* framebuffer, const std::filesystem::path& path, std::string& error)
{
    nvrhi::ITexture* backBuffer = framebuffer->getDesc().colorAttachments[0].texture;
    const nvrhi::TextureDesc& desc = backBuffer->getDesc();
    nvrhi::StagingTextureHandle staging;
    m_Device->createStagingTexture(desc, nvrhi::CpuAccessMode::Read, &staging);

    nvrhi::CommandListHandle list;
    m_Device->createCommandList(nvrhi::CommandListParameters(), &list);
    list->open();
    list->copyTexture2(staging, nvrhi::TextureSlice(), backBuffer, nvrhi::TextureSlice());
    list->close();
    m_Device->executeCommandList(list);
    m_Device->waitForIdle();

    size_t rowPitch = 0;
    const void* mapped = m_Device->mapStagingTexture(staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, rowPitch);
    if (!mapped)
    {
        error = "mapStagingTexture failed";
        return false;
    }
    const bool bgra = desc.format == nvrhi::Format::BGRA8_UNORM || desc.format == nvrhi::Format::SBGRA8_UNORM;
    const bool ok = Benchmark::WriteBmp(path, mapped, int(desc.width), int(desc.height), rowPitch, bgra, error);
    m_Device->unmapStagingTexture(staging);
    return ok;
}

// ---------------------------------------------------------------------------------------------
// Recording (MtScene::RunFrame interface)
// ---------------------------------------------------------------------------------------------

void MtRenderer::OpenList(int pass, int chunk)
{
    nvrhi::ICommandList* list = m_Chunks[chunk]->lists[pass];
    list->open();
    if (m_Desc.explicitBarriers && pass >= kNumShadows)
    {
        // Explicit barriers: the mirror and direct passes (5 of the 6 passes, every draw of the frame but the
        // shadow map's) run without nvrhi's per-draw resource tracking. The framebuffer attachments are
        // brought to their render states once per list and restored at close by nvrhi's keepInitialState;
        // the shadow map is left in the shader-resource state by the shadow pass, which keeps automatic
        // barriers because its clears and transitions are backend specific (Vulkan clears want TRANSFER_DST).
        list->setEnableAutomaticBarriers(false);
        list->setResourceStatesForFramebuffer(m_FrameFramebuffer);
        list->setTextureState(m_ShadowMaps[0], nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
        list->commitBarriers();
    }
}

void MtRenderer::CloseList(int pass, int chunk)
{
    m_Chunks[chunk]->lists[pass]->close();
}

void MtRenderer::ExecuteLists(int pass, int chunkCount)
{
    m_ExecuteScratch.clear();
    for (int chunk = 0; chunk < chunkCount; ++chunk)
        m_ExecuteScratch.push_back(m_Chunks[chunk]->lists[pass]);
    m_Device->executeCommandLists(m_ExecuteScratch.data(), m_ExecuteScratch.size());
}

void MtRenderer::ExecuteAllPasses()
{
    m_ExecuteScratch.clear();
    for (int pass = 0; pass < kNumPasses; ++pass)
        m_ExecuteScratch.push_back(m_Chunks[0]->lists[pass]);
    m_Device->executeCommandLists(m_ExecuteScratch.data(), m_ExecuteScratch.size());
}

nvrhi::ViewportState MtRenderer::Viewport(uint32_t width, uint32_t height, const RECT& scissor) const
{
    nvrhi::ViewportState vp;
    vp.addViewport(nvrhi::Viewport(float(width), float(height)));
    vp.addScissorRect(nvrhi::Rect(int(scissor.left), int(scissor.right), int(scissor.top), int(scissor.bottom)));
    return vp;
}

void MtRenderer::RecordPass(int pass, int chunk, int chunkCount, int listPass)
{
    ChunkContext& ctx = *m_Chunks[chunk];
    nvrhi::ICommandList* list = ctx.lists[listPass];
    ListStats& stats = ctx.stats[pass];
    const bool first = chunk == 0;
    const bool last = chunk == chunkCount - 1;
    if (pass < kNumShadows)
    {
        const int shadow = pass;
        nvrhi::ITexture* shadowMap = m_ShadowMaps[shadow];
        if (first)
        {
            if (shadow == 0)
            {
                // Frame begin: clear the back buffer and the depth/stencil buffer.
                nvrhi::utils::ClearColorAttachment(list, m_FrameFramebuffer, 0, nvrhi::Color(Scene::kClearColor.x, Scene::kClearColor.y, Scene::kClearColor.z, 1.f));
                nvrhi::utils::ClearDepthStencilAttachment(list, m_FrameFramebuffer, 1.f, 0);
            }
            list->clearDepthStencilTexture(shadowMap, nvrhi::AllSubresources, true, 1.f, false, 0);
        }
        RenderScene(list, ctx, m_Params.shadow[shadow], chunk, chunkCount, stats);
    }
    else if (pass < kPassDirect)
    {
        const MirrorPass& p = m_Params.mirror[pass - kNumShadows];
        if (!p.visible)
            return;
        if (first)
        {
            DrawMirrorQuad(list, ctx, PipelineMirrorStencil + (p.mirror % 8), p.mirror, p.mirrorObject, p.stencilScene, p.scissor);
            DrawMirrorQuad(list, ctx, PipelineMirrorClearDepth + (p.mirror % 8), p.mirror, p.mirrorObject, p.depthClearScene, p.scissor);
        }
        RenderScene(list, ctx, p.reflected, chunk, chunkCount, stats);
        if (last)
            DrawMirrorQuad(list, ctx, PipelineMirrorSurface + (p.mirror % 8), p.mirror, p.mirrorObject, p.stencilScene, p.scissor);
    }
    else
    {
        RenderScene(list, ctx, m_Params.direct, chunk, chunkCount, stats);
        // The back buffer returns to Present through nvrhi's keepInitialState at close().
    }
}

void MtRenderer::DrawMirrorQuad(nvrhi::ICommandList* list, ChunkContext& ctx, int pipeline, int mirror, const CbPerObject& object,
                                const CbPerScene& scene, const RECT& scissor)
{
    list->writeBuffer(ctx.cbObject, &object, sizeof(object));
    list->writeBuffer(ctx.cbScene, &scene, sizeof(scene));

    nvrhi::GraphicsState state;
    state.pipeline = m_Pipelines[pipeline];
    state.framebuffer = m_FrameFramebuffer;
    state.viewport = Viewport(m_Width, m_Height, scissor);
    state.bindings = { ctx.set0NoLights, m_MaterialSetPtrs[0] };
    state.vertexBuffers = { nvrhi::VertexBufferBinding().setBuffer(m_MirrorVertexBuffer).setSlot(0).setOffset(uint64_t(mirror) * sizeof(MirrorVertex) * 4) };
    list->setGraphicsState(state);

    nvrhi::DrawArguments args;
    args.vertexCount = 4;
    list->draw(args);
}

void MtRenderer::RenderScene(nvrhi::ICommandList* list, ChunkContext& ctx, const SceneDraw& draw, int chunk, int chunkCount, ListStats& stats)
{
    int pipeline = PipelineDirect;
    nvrhi::IFramebuffer* framebuffer = m_FrameFramebuffer;
    switch (draw.kind)
    {
        case SceneKind::Shadow:
            pipeline = PipelineShadow;
            framebuffer = m_ShadowFramebuffers[0];
            break;
        case SceneKind::Mirrored: pipeline = PipelineMirroredScene + (draw.mirror % 8); break;
        case SceneKind::Direct: pipeline = PipelineDirect; break;
    }

    list->writeBuffer(ctx.cbScene, &draw.scene, sizeof(draw.scene));
    if (draw.hasLights)
        list->writeBuffer(ctx.cbLight, &m_Params.lights, sizeof(m_Params.lights));
    list->writeBuffer(ctx.cbObject, &draw.object, sizeof(draw.object));

    nvrhi::GraphicsState state;
    state.pipeline = m_Pipelines[pipeline];
    state.framebuffer = framebuffer;
    state.viewport = Viewport(draw.viewportWidth, draw.viewportHeight, draw.scissor);
    state.bindings = { draw.kind == SceneKind::Shadow ? ctx.set0NoLights : ctx.set0, nullptr };
    state.vertexBuffers.resize(1);
    state.vertexBuffers[0].slot = 0;
    state.vertexBuffers[0].offset = 0;
    state.indexBuffer.offset = 0;

    const uint32_t count = uint32_t(m_RenderOrder.size());
    for (uint32_t i = 0; i < count; ++i)
    {
        if (!ChunkOwnsDraw(i, uint32_t(chunk), uint32_t(chunkCount)))
            continue;
        const MeshDraw& mesh = m_RenderOrder[i];
        const MeshBuffers& buffers = m_MeshBuffers[mesh.buffers];
        state.vertexBuffers[0].buffer = buffers.vertexBuffer;
        state.indexBuffer.buffer = buffers.indexBuffer;
        state.indexBuffer.format = buffers.indexFormat;
        for (uint32_t s = 0; s < mesh.subsetCount; ++s)
        {
            const Subset& subset = m_Subsets[mesh.firstSubset + s];
            state.bindings[1] = m_MaterialSetPtrs[subset.material];
            list->setGraphicsState(state);
            nvrhi::DrawArguments args;
            args.vertexCount = subset.indexCount;
            args.startIndexLocation = subset.indexStart;
            args.startVertexLocation = subset.vertexStart;
            list->drawIndexed(args);
            ++stats.draws;
            stats.indices += subset.indexCount;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Benchmark columns (Donut owns the frame loop; see the Asteroids renderer for the derivation):
//   wait    = afterPresent .. beforeFrame (Donut's sleep_for(0) + runGarbageCollection, reported as gc too)
//           + afterAnimate .. beforeRender (DeviceManager::BeginFrame: frame fence / acquire)
//   update  = Animate (Donut) + the pass parameter setup in Render
//   render / submit = inside Render (RunFrame)
//   present = beforePresent .. afterPresent
// ---------------------------------------------------------------------------------------------

void MtRenderer::InstallFrameCallbacks()
{
    auto& callbacks = GetDeviceManager()->m_callbacks;
    callbacks.beforeFrame = [this](donut::app::DeviceManager&, uint32_t) { OnBeforeFrame(); };
    callbacks.afterAnimate = [this](donut::app::DeviceManager&, uint32_t) { OnAfterAnimate(); };
    callbacks.beforeRender = [this](donut::app::DeviceManager&, uint32_t) { OnBeforeRender(); };
    callbacks.beforePresent = [this](donut::app::DeviceManager&, uint32_t) { OnBeforePresent(); };
    callbacks.afterPresent = [this](donut::app::DeviceManager&, uint32_t) { OnAfterPresent(); };
    Benchmark::recorder.BeginFrame(); // opens frame 0
}

void MtRenderer::OnBeforeFrame()
{
    if (m_HaveAfterPresent)
    {
        const double ms = Benchmark::Ms(Benchmark::Now() - m_AfterPresentStamp);
        Benchmark::recorder.Times().wait += ms;
        Benchmark::recorder.Times().gc += ms;
        m_HaveAfterPresent = false;
    }
}

void MtRenderer::OnAfterAnimate()
{
    m_WaitStamp = Benchmark::Now();
}

void MtRenderer::OnBeforeRender()
{
    Benchmark::recorder.Times().wait += Benchmark::Ms(Benchmark::Now() - m_WaitStamp);
}

void MtRenderer::OnBeforePresent()
{
    m_PresentStamp = Benchmark::Now();
}

void MtRenderer::OnAfterPresent()
{
    const int64_t now = Benchmark::Now();
    Benchmark::recorder.Times().present += Benchmark::Ms(now - m_PresentStamp);
    m_AfterPresentStamp = now;
    m_HaveAfterPresent = true;
    if (m_Done) return;
    if (!Benchmark::recorder.BeginFrame())
    {
        m_Done = true;
        glfwSetWindowShouldClose(GetDeviceManager()->GetWindow(), GLFW_TRUE);
    }
}

void MtRenderer::FillRunInfo(Benchmark::RunInfo& info) const
{
    info.width = int(m_Width);
    info.height = int(m_Height);
    info.threads = int(m_Threads);
    info.meshes = m_MeshBuffers.size();
    info.subsets = m_Subsets.size();
    info.textures = uint32_t(m_Textures.size());
    info.passes = kNumPasses;
    auto add = [&](const char* k, const std::string& v) { info.extra.emplace_back(k, v); };
    add("renderer_string", GetDeviceManager()->GetRendererString());
    add("threading", m_Mode == Mode::SingleThreaded ? "main thread, one nvrhi command list" :
                     m_Mode == Mode::PerScene ? "worker pool: one nvrhi command list per shadow map and mirror, direct view on the main thread, one executeCommandLists" :
                     "worker pool: every pass split into per-thread chunks (one nvrhi command list each), one executeCommandLists per pass");
    add("constant_buffers", "three volatile constant buffers per recording chunk (writeBuffer per pass), binding set 0");
    add("textures_binding", "one binding set per material (diffuse, normal) at layout index 1; setGraphicsState per subset");
    add("barriers", m_Desc.explicitBarriers ? "explicit: automatic barriers off in the mirror and direct pass lists (setResourceStatesForFramebuffer once per list), automatic in the shadow pass" : "automatic");
    add("mirror_scissor", m_Scene->TightMirrorScissor() ? "tight" : "full");
    add("swap_chain_buffers", std::to_string(GetDeviceManager()->GetBackBufferCount()));
    add("back_buffer_format", nvrhi::getFormatInfo(GetDeviceManager()->GetBackBuffer(0)->getDesc().format).name);
    add("draw_count_definition", "sdkmesh subset draws of every pass (shadow, 4 mirrors, direct); the mirror quads are not counted");
}
