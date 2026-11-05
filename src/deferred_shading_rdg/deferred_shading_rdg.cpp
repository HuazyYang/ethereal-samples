/*
* Copyright (c) 2014-2021, NVIDIA CORPORATION. All rights reserved.
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

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include <donut/app/ApplicationBase.h>
#include <donut/app/DeviceManager.h>
#include <donut/app/RenderGraphLayer.h>
#include <donut/app/RenderGraphVisualizer.h>
#include <donut/core/log.h>
#include <donut/core/vfs/VFS.h>
#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/SceneGraph.h>
#include <donut/engine/FramebufferFactory.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/TextureCache.h>
#include <donut/render/DeferredLightingPass.h>
#include <donut/render/GBuffer.h>
#include <donut/render/GBufferFillPass.h>
#include <donut/render/BlitPass.h>
#include <donut/render/RenderGraphLambdaPass.h>
#include <donut/render/DrawStrategy.h>

using namespace donut;
using namespace donut::math;
using namespace donut::engine;
using namespace donut::render;

#include <donut/shaders/material_cb.h>
#include <donut/shaders/bindless.h>

#include "../deferred_shading/CubeGeometry.h"
#include "../sample_harness/SampleHarness.h"
#include "../sample_harness/GraphRecording.h"

static const char* g_WindowTitle = "Donut Example: Deferred Shading (render graph)";

class SimpleScene
{
public:

    bool Init(nvrhi::IDevice* device, nvrhi::ICommandList* commandList, TextureCache* textureCache)
    {
        commandList->open();

        m_Buffers = MAKE_RC_OBJ_PTR(BufferGroup);
        m_Buffers->indexBuffer = CreateGeometryBuffer(device, commandList, "IndexBuffer", g_Indices, sizeof(g_Indices), false, false);

        uint64_t vertexBufferSize = 0;
        m_Buffers->getVertexBufferRange(VertexAttribute::Position).setByteOffset(vertexBufferSize).setByteSize(sizeof(g_Positions)); vertexBufferSize += sizeof(g_Positions);
        m_Buffers->getVertexBufferRange(VertexAttribute::TexCoord1).setByteOffset(vertexBufferSize).setByteSize(sizeof(g_TexCoords)); vertexBufferSize += sizeof(g_TexCoords);
        m_Buffers->getVertexBufferRange(VertexAttribute::Normal).setByteOffset(vertexBufferSize).setByteSize(sizeof(g_Normals)); vertexBufferSize += sizeof(g_Normals);
        m_Buffers->getVertexBufferRange(VertexAttribute::Tangent).setByteOffset(vertexBufferSize).setByteSize(sizeof(g_Tangents)); vertexBufferSize += sizeof(g_Tangents);
        m_Buffers->vertexBuffer = CreateGeometryBuffer(device, commandList, "VertexBuffer", nullptr, vertexBufferSize, true, false);

        commandList->beginTrackingBufferState(m_Buffers->vertexBuffer, nvrhi::ResourceStates::CopyDest);
        commandList->writeBuffer(m_Buffers->vertexBuffer, g_Positions, sizeof(g_Positions), m_Buffers->getVertexBufferRange(VertexAttribute::Position).byteOffset);
        commandList->writeBuffer(m_Buffers->vertexBuffer, g_TexCoords, sizeof(g_TexCoords), m_Buffers->getVertexBufferRange(VertexAttribute::TexCoord1).byteOffset);
        commandList->writeBuffer(m_Buffers->vertexBuffer, g_Normals, sizeof(g_Normals), m_Buffers->getVertexBufferRange(VertexAttribute::Normal).byteOffset);
        commandList->writeBuffer(m_Buffers->vertexBuffer, g_Tangents, sizeof(g_Tangents), m_Buffers->getVertexBufferRange(VertexAttribute::Tangent).byteOffset);
        commandList->setPermanentBufferState(m_Buffers->vertexBuffer, nvrhi::ResourceStates::ShaderResource);
        
        InstanceData instance{};
        instance.transform = math::float3x4(transpose(math::affineToHomogeneous(math::affine3::identity())));
        instance.prevTransform = instance.transform;
        m_Buffers->instanceBuffer = CreateGeometryBuffer(device, commandList, "VertexBufferTransform", &instance, sizeof(instance), false, true);

        std::filesystem::path textureFileName = app::GetDirectoryWithExecutable().parent_path() / "media/nvidia-logo.png";

        m_Material = MAKE_RC_OBJ_PTR(Material);
        m_Material->name = "CubeMaterial";
        m_Material->useSpecularGlossModel = true;
        m_Material->enableBaseOrDiffuseTexture = true;
        m_Material->baseOrDiffuseTexture = textureCache->LoadTextureFromFile(textureFileName, true, nullptr, commandList);
        m_Material->materialConstants = CreateMaterialConstantBuffer(device, commandList, m_Material);

        commandList->close();
        device->executeCommandList(commandList);

        if (!m_Material->baseOrDiffuseTexture->texture)
        {
            log::error("Couldn't load the texture");
            return false;
        }

        auto geometry = MAKE_RC_OBJ_PTR(MeshGeometry);
        geometry->material = m_Material;
        geometry->numIndices = dim(g_Indices);
        geometry->numVertices = dim(g_Positions);

        m_MeshInfo = MAKE_RC_OBJ_PTR(MeshInfo);
        m_MeshInfo->name = "CubeMesh";
        m_MeshInfo->buffers = m_Buffers;
        m_MeshInfo->objectSpaceBounds = math::box3(math::float3(-0.5f), math::float3(0.5f));;
        m_MeshInfo->totalIndices = geometry->numIndices;
        m_MeshInfo->totalVertices = geometry->numVertices;
        m_MeshInfo->geometries.push_back(geometry);

        m_SceneGraph = MAKE_RC_OBJ_PTR(SceneGraph);
        auto node = MAKE_RC_OBJ_PTR(SceneGraphNode);
        m_SceneGraph->SetRootNode(node);

        m_MeshInstance = MAKE_RC_OBJ_PTR(MeshInstance, m_MeshInfo);
        node->SetLeaf(m_MeshInstance);
        node->SetName("CubeNode");

        nvrhi::AutoPtr<DirectionalLight> sunLight = MAKE_RC_OBJ_PTR(DirectionalLight);
        m_SceneGraph->AttachLeafNode(node, sunLight);

        sunLight->SetDirection(double3(0.1, -1.0, 0.2));
        sunLight->angularSize = 0.53f;
        sunLight->irradiance = 1.f;
        sunLight->SetName("Sun");

        m_SceneGraph->Refresh(0);

        PrintSceneGraph(m_SceneGraph->GetRootNode());

        return true;
    }

    const nvrhi::AutoPtr<MeshInstance>& GetMeshInstance() const
    {
        return m_MeshInstance;
    }

    const nvrhi::AutoPtr<SceneGraph>& GetSceneGraph() const
    {
        return m_SceneGraph;
    }
    
    const std::vector<nvrhi::AutoPtr<Light>>& GetLights() const
    {
        return m_SceneGraph->GetLights();
    }

private:
    nvrhi::AutoPtr<BufferGroup> m_Buffers;
    nvrhi::AutoPtr<Material> m_Material;
    nvrhi::AutoPtr<MeshInfo> m_MeshInfo;
    nvrhi::AutoPtr<MeshInstance> m_MeshInstance;
    nvrhi::AutoPtr<SceneGraph> m_SceneGraph;

    nvrhi::BufferHandle CreateGeometryBuffer(nvrhi::IDevice* device, nvrhi::ICommandList* commandList,
        const char* debugName, const void* data, uint64_t dataSize, bool isVertexBuffer, bool isInstanceBuffer)
    {
        nvrhi::BufferHandle bufHandle;

        // The G-buffer fill pass accesses instance buffers as structured on DX12 and Vulkan, and as raw on DX11.
        bool const needStructuredBuffer = isInstanceBuffer && device->getGraphicsAPI() != nvrhi::GraphicsAPI::D3D11;
        
        nvrhi::BufferDesc desc;
        desc.byteSize = dataSize;
        desc.isIndexBuffer = !isVertexBuffer && !isInstanceBuffer;
        desc.canHaveRawViews = isVertexBuffer || isInstanceBuffer;
        desc.structStride = needStructuredBuffer ? sizeof(InstanceData) : 0;
        desc.debugName = debugName;
        desc.initialState = nvrhi::ResourceStates::CopyDest;
        device->createBuffer(desc, &bufHandle);

        if (data)
        {
            commandList->beginTrackingBufferState(bufHandle, nvrhi::ResourceStates::CopyDest);
            commandList->writeBuffer(bufHandle, data, dataSize);
            commandList->setPermanentBufferState(bufHandle, (isVertexBuffer || isInstanceBuffer)
                ? nvrhi::ResourceStates::ShaderResource
                : nvrhi::ResourceStates::IndexBuffer);
        }

        return bufHandle;
    }

    nvrhi::BufferHandle CreateMaterialConstantBuffer(nvrhi::IDevice* device, nvrhi::ICommandList* commandList, const nvrhi::AutoPtr<Material> material)
    {
        nvrhi::BufferDesc bufferDesc;
        bufferDesc.byteSize = sizeof(MaterialConstants);
        bufferDesc.debugName = material->name;
        bufferDesc.isConstantBuffer = true;
        bufferDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
        bufferDesc.keepInitialState = true;
        nvrhi::BufferHandle buffer;
        device->createBuffer(bufferDesc, &buffer);

        MaterialConstants constants;
        material->FillConstantBuffer(constants);
        commandList->writeBuffer(buffer, &constants, sizeof(constants));

        return buffer;
    }
};

// The deferred-shading sample on a render graph (aggregate plan docs/plans/2026-11-05-render-graph.md, step P2.1):
// the scene, view and passes of deferred_shading, recorded each frame through the passes' AddToGraph on
// app::RenderGraphLayer. The G-buffer and the shaded colour are transients, so the graph places and discards them;
// the back buffer is the layer's import. Per-pass GPU timers are on, and the visualizer shows the plan.
//
// Measurements of step P2.1, beside the harness's options:
//     -graphstats F    median per frame, after the warm-up: the layer's cost (BuildGraph, Compile, Execute), the
//                      executor's record and submit time, and the barriers; written to F as JSON
//     -nocache         CompileOptions::maxCachedPlans = 0: every frame compiles (7.7)
//     -emptybodies     the same graph with empty pass bodies: what is left is the executor's overhead (10.3)
//     -notimers        no per-pass GPU timers (ExecuteDesc::timers), which the original has no counterpart of
//     -parallel N, -granularity G   parallel recording (sample_harness/GraphRecording.h, step P2.4)
class DeferredShadingGraph : public app::RenderGraphLayer
{
    NVRHI_INHERIT_INTERFACE_TABLE()
private:
    nvrhi::AutoPtr<ShaderFactory> m_ShaderFactory;
    nvrhi::AutoPtr<TextureCache> m_TextureCache;
    nvrhi::AutoPtr<CommonRenderPasses> m_CommonPasses;

    nvrhi::AutoPtr<GBufferFillPass> m_GBufferPass;
    nvrhi::AutoPtr<DeferredLightingPass> m_DeferredLightingPass;
    nvrhi::AutoPtr<render::BlitPass> m_BlitPass;
    GBufferFillPass::Context m_GBufferContext;

    PlanarView m_View;
    SimpleScene m_Scene;
    render::DrawItem m_DrawItem;
    render::PassthroughDrawStrategy m_DrawStrategy;
    nvrhi::Format m_DepthFormat = nvrhi::Format::UNKNOWN;

    nvrhi::CommandListHandle m_CommandList;
    float m_Rotation = 0.f;

    bool m_EmptyBodies = false;
    bool m_NoTimers = false;
    uint32_t m_Warmup = 100;
    uint32_t m_Frame = 0;
    std::vector<double> m_FrameCostMs;
    std::vector<double> m_ExecuteMs;
    std::vector<double> m_RecordMs;
    std::vector<uint32_t> m_Barriers;
    render::RunStatistics m_LastRun;
    sample_harness::GraphRecording m_Recording;

    static double Median(std::vector<double> samples)
    {
        if (samples.empty())
            return 0.0;
        std::sort(samples.begin(), samples.end());
        return samples[samples.size() / 2];
    }

public:
    using RenderGraphLayer::RenderGraphLayer;

    void Configure(int argc, const char* const* argv, uint32_t warmup)
    {
        m_Warmup = warmup;
        for (int i = 1; i < argc; ++i)
        {
            if (!std::strcmp(argv[i], "-nocache"))
                GetCompileOptions().maxCachedPlans = 0;
            else if (!std::strcmp(argv[i], "-emptybodies"))
                m_EmptyBodies = true;
            else if (!std::strcmp(argv[i], "-notimers"))
                m_NoTimers = true;
        }
        m_Recording.Configure(argc, argv, this);
    }

    void Render(nvrhi::IFramebuffer* framebuffer) override
    {
        const auto start = std::chrono::steady_clock::now();
        RenderGraphLayer::Render(framebuffer);
        const double cost = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        GetRegistry()->GetRunStatistics(m_LastRun);
        if (m_Frame++ >= m_Warmup)
        {
            m_FrameCostMs.push_back(cost);
            m_ExecuteMs.push_back(double(m_LastRun.recordMilliseconds) + double(m_LastRun.submitMilliseconds));
            m_RecordMs.push_back(double(m_LastRun.recordMilliseconds));
            m_Barriers.push_back(m_LastRun.barriers);
        }
    }

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
        GetRegistry()->GetRegistryStatistics(registry);
        std::fprintf(file, "{\n  \"frames\": %u,\n  \"warmup\": %u,\n  \"cache\": %s,\n  \"empty_bodies\": %s,\n  \"timers\": %s,\n"
            "  \"median_layer_frame_ms\": %.4f,\n  \"median_execute_ms\": %.4f,\n  \"median_record_ms\": %.4f,\n"
            "  \"barriers_min\": %u,\n  \"barriers_max\": %u,\n  \"passes\": %u,\n  \"segments\": %u,\n  \"threads\": %u,\n"
            "  \"record_tasks\": %u,\n  \"plan_hits\": %llu,\n  \"plan_misses\": %llu\n}\n",
            m_Frame, m_Warmup, GetCompileOptions().maxCachedPlans ? "true" : "false", m_EmptyBodies ? "true" : "false",
            GetExecuteDesc().timers ? "true" : "false",
            Median(m_FrameCostMs), Median(m_ExecuteMs), Median(m_RecordMs), m_Barriers.empty() ? 0u : barriersMin, barriersMax,
            m_LastRun.passes, m_LastRun.segments, m_Recording.GetThreadCount(), m_LastRun.recordTasks,
            (unsigned long long)registry.planHits, (unsigned long long)registry.planMisses);
        std::fclose(file);
    }

    ShaderFactory* GetShaderFactory() const { return m_ShaderFactory; }

    void SetupView(float2 renderTargetSize)
    {
        math::affine3 viewMatrix = math::yawPitchRoll(m_Rotation, 0.f, 0.f)
            * math::yawPitchRoll(0.f, math::radians(-30.f), 0.f)
            * math::translation(math::float3(0, 0, 2));

        float4x4 projection = math::perspProjD3DStyle(math::radians(60.f), renderTargetSize.x / renderTargetSize.y, 0.1f, 10.f);

        m_View.SetViewport(nvrhi::Viewport(renderTargetSize.x, renderTargetSize.y));
        m_View.SetMatrices(viewMatrix, projection);
        m_View.UpdateCache();
    }

    bool Init()
    {
        auto nativeFS = MAKE_RC_OBJ_PTR(vfs::NativeFileSystem);

        std::filesystem::path frameworkShaderPath = app::GetDirectoryWithExecutable() / "shaders/framework" / app::GetShaderTypeName(GetDevice()->getGraphicsAPI());

        nvrhi::AutoPtr<vfs::RootFileSystem> rootFS = MAKE_RC_OBJ_PTR(vfs::RootFileSystem);
        rootFS->mount("/shaders/donut", frameworkShaderPath);
        m_ShaderFactory = MAKE_RC_OBJ_PTR(ShaderFactory, GetDevice(), rootFS, "/shaders");
        m_CommonPasses = MAKE_RC_OBJ_PTR(CommonRenderPasses, GetDevice(), m_ShaderFactory);

        m_GBufferPass = MAKE_RC_OBJ_PTR(GBufferFillPass, GetDevice(), m_CommonPasses);
        m_GBufferPass->Init(*m_ShaderFactory, GBufferFillPass::CreateParameters());
        m_DeferredLightingPass = MAKE_RC_OBJ_PTR(DeferredLightingPass, GetDevice(), m_CommonPasses);
        m_DeferredLightingPass->Init(m_ShaderFactory);
        m_BlitPass = MAKE_RC_OBJ_PTR(render::BlitPass, GetDevice(), m_CommonPasses);

        // The depth format GBufferRenderTargets chooses.
        const nvrhi::Format depthFormats[] = { nvrhi::Format::D24S8, nvrhi::Format::D32S8, nvrhi::Format::D32, nvrhi::Format::D16 };
        m_DepthFormat = nvrhi::utils::ChooseFormat(GetDevice(),
            nvrhi::FormatSupport::Texture | nvrhi::FormatSupport::DepthStencil | nvrhi::FormatSupport::ShaderLoad,
            depthFormats, std::size(depthFormats));

        m_TextureCache = MAKE_RC_OBJ_PTR(TextureCache, GetDevice(), nativeFS, nullptr);

        GetDevice()->createCommandList(nvrhi::CommandListParameters(), &m_CommandList);

        GetExecuteDesc().timers = !m_NoTimers;

        return m_Scene.Init(GetDevice(), m_CommandList, m_TextureCache);
    }

    void Animate(float seconds) override
    {
        seconds = sample_harness::Step(seconds);
        m_Rotation += seconds * 1.1f;
        GetDeviceManager()->SetInformativeWindowTitle(g_WindowTitle);
    }

    void BuildGraph(render::IRenderGraph* graph, render::FrameData&, render::ResourceId backBuffer) override
    {
        nvrhi::TextureDesc backBufferDesc;
        if (graph->GetTextureDesc(backBuffer, backBufferDesc) != nvrhi::FS_OK)
            return;
        const uint32_t width = backBufferDesc.width;
        const uint32_t height = backBufferDesc.height;
        SetupView(float2(float(width), float(height)));

        // The G-buffer of GBufferRenderTargets, without motion vectors, and the shaded colour: transients.
        auto target = [&](nvrhi::Format format, const char* name) {
            return graph->CreateTexture(nvrhi::TextureDesc().setWidth(width).setHeight(height).setFormat(format)
                .setIsRenderTarget(true).setDebugName(name), render::ResourceFlags::None);
        };
        if (m_EmptyBodies)
        {
            BuildEmptyBodies(graph, backBuffer, width, height);
            return;
        }

        render::GeometryGraphInputs gbuffer;
        gbuffer.name = "GBufferFill";
        gbuffer.view = &m_View;
        gbuffer.viewPrev = &m_View;
        gbuffer.colorTargets[0] = target(nvrhi::Format::SRGBA8_UNORM, "GBufferDiffuse");
        gbuffer.colorTargets[1] = target(nvrhi::Format::SRGBA8_UNORM, "GBufferSpecular");
        gbuffer.colorTargets[2] = target(nvrhi::Format::RGBA16_SNORM, "GBufferNormals");
        gbuffer.colorTargets[3] = target(nvrhi::Format::RGBA16_FLOAT, "GBufferEmissive");
        gbuffer.colorTargetCount = 4;
        gbuffer.depth = graph->CreateTexture(nvrhi::TextureDesc().setWidth(width).setHeight(height).setFormat(m_DepthFormat)
            .setIsRenderTarget(true).setIsTypeless(true).setDebugName("GBufferDepth"), render::ResourceFlags::None);
        // GBufferRenderTargets::Clear: colour 0, depth 1 (no reverse projection).
        gbuffer.clearTargets = true;
        gbuffer.depthClear.depth = 1.f;

        m_DrawItem.instance = m_Scene.GetMeshInstance().Get();
        m_DrawItem.mesh = m_DrawItem.instance->GetMesh();
        m_DrawItem.geometry = m_DrawItem.mesh->geometries[0].Get();
        m_DrawItem.material = m_DrawItem.geometry->material.Get();
        m_DrawItem.buffers = m_DrawItem.mesh->buffers.Get();
        m_DrawItem.distanceToCamera = 0;
        m_DrawItem.cullMode = nvrhi::RasterCullMode::Back;
        m_DrawStrategy.SetData(&m_DrawItem, 1);
        gbuffer.drawStrategy = &m_DrawStrategy;
        gbuffer.passContext = &m_GBufferContext;
        const render::GeometryGraphOutputs filled = m_GBufferPass->AddToGraph(graph, gbuffer);

        render::DeferredLightingGraphInputs lighting;
        lighting.view = &m_View;
        lighting.depth = filled.depth;
        lighting.gbufferDiffuse = filled.colorTargets[0];
        lighting.gbufferSpecular = filled.colorTargets[1];
        lighting.gbufferNormals = filled.colorTargets[2];
        lighting.gbufferEmissive = filled.colorTargets[3];
        lighting.output = graph->CreateTexture(nvrhi::TextureDesc().setWidth(width).setHeight(height)
            .setFormat(nvrhi::Format::RGBA16_FLOAT).setIsUAV(true).setDebugName("ShadedColor"), render::ResourceFlags::None);
        lighting.ambientColorTop = 0.2f;
        lighting.ambientColorBottom = lighting.ambientColorTop * float3(0.3f, 0.4f, 0.3f);
        lighting.lights = &m_Scene.GetLights();
        const render::DeferredLightingGraphOutputs lit = m_DeferredLightingPass->AddToGraph(graph, lighting);

        render::BlitGraphInputs blit;
        blit.source = lit.output;
        blit.target = backBuffer;
        m_BlitPass->AddToGraph(graph, blit);
    }

    // The frame's graph with the same resources and accesses, and empty bodies (10.3, the executor's overhead).
    void BuildEmptyBodies(render::IRenderGraph* graph, render::ResourceId backBuffer, uint32_t width, uint32_t height)
    {
        struct NoData { uint32_t unused; };
        auto target = [&](nvrhi::Format format, const char* name) {
            return graph->CreateTexture(nvrhi::TextureDesc().setWidth(width).setHeight(height).setFormat(format)
                .setIsRenderTarget(true).setDebugName(name), render::ResourceFlags::None);
        };
        render::ResourceId gbuffer[4] = {
            target(nvrhi::Format::SRGBA8_UNORM, "GBufferDiffuse"), target(nvrhi::Format::SRGBA8_UNORM, "GBufferSpecular"),
            target(nvrhi::Format::RGBA16_SNORM, "GBufferNormals"), target(nvrhi::Format::RGBA16_FLOAT, "GBufferEmissive") };
        render::ResourceId depth = graph->CreateTexture(nvrhi::TextureDesc().setWidth(width).setHeight(height).setFormat(m_DepthFormat)
            .setIsRenderTarget(true).setIsTypeless(true).setDebugName("GBufferDepth"), render::ResourceFlags::None);
        render::ResourceId shaded = graph->CreateTexture(nvrhi::TextureDesc().setWidth(width).setHeight(height)
            .setFormat(nvrhi::Format::RGBA16_FLOAT).setIsUAV(true).setDebugName("ShadedColor"), render::ResourceFlags::None);
        static const render::ClearValue c_Clear = { { 0.f, 0.f, 0.f, 0.f }, 1.f, 0 };
        render::AddLambdaPass<NoData>(graph, render::PassDesc{ "GBufferFill", render::PassType::Raster, render::PassFlags::None, 0 },
            [&](render::ISetupContext* s, NoData&) {
                for (uint32_t i = 0; i < 4; ++i)
                    gbuffer[i] = s->SetColorTarget(i, gbuffer[i], nvrhi::SubresourceRegion(), render::AccessFlags::Clear, &c_Clear);
                depth = s->SetDepthTarget(depth, nvrhi::SubresourceRegion(), false, render::AccessFlags::Clear, &c_Clear);
            },
            [](render::IPassContext*, const NoData&) { });
        render::AddLambdaPass<NoData>(graph, render::PassDesc{ "DeferredLighting", render::PassType::Compute, render::PassFlags::None, 0 },
            [&](render::ISetupContext* s, NoData&) {
                s->Read(depth, render::Access::ShaderRead, nvrhi::ShaderType::Compute, nvrhi::SubresourceRegion());
                for (render::ResourceId id : gbuffer)
                    s->Read(id, render::Access::ShaderRead, nvrhi::ShaderType::Compute, nvrhi::SubresourceRegion());
                shaded = s->Write(shaded, render::Access::UAVWrite, nvrhi::ShaderType::Compute, nvrhi::SubresourceRegion(),
                                  render::AccessFlags::FullWrite);
            },
            [](render::IPassContext*, const NoData&) { });
        render::AddLambdaPass<NoData>(graph, render::PassDesc{ "Blit", render::PassType::Raster, render::PassFlags::None, 0 },
            [&](render::ISetupContext* s, NoData&) {
                s->Read(shaded, render::Access::ShaderRead, nvrhi::ShaderType::Pixel, nvrhi::SubresourceRegion());
                backBuffer = s->SetColorTarget(0, backBuffer, nvrhi::SubresourceRegion(), render::AccessFlags::FullWrite, nullptr);
            },
            [](render::IPassContext*, const NoData&) { });
    }
};

#ifdef WIN32
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
#else
int main(int __argc, const char** __argv)
#endif
{
    nvrhi::GraphicsAPI api = app::GetGraphicsAPIFromCommandLine(__argc, __argv);
    auto deviceManager = nvrhi::TakeOver(app::DeviceManager::Create(api));

    app::DeviceCreationParameters deviceParams;
#ifdef _DEBUG
    deviceParams.enableDebugRuntime = true;
    deviceParams.enableNvrhiValidationLayer = true;
#endif

    if (!deviceManager->CreateWindowDeviceAndSwapChain(deviceParams, g_WindowTitle))
    {
        log::fatal("Cannot initialize a graphics device with the requested parameters");
        return 1;
    }

    {
        sample_harness::Harness harness(deviceManager, sample_harness::ParseOptions(__argc, __argv));
        DeferredShadingGraph example(deviceManager);
        example.Configure(__argc, __argv, harness.GetOptions().warmup);
        if (example.Init())
        {
            deviceManager->AddLayerToBack(&example);
            // The visualizer draws over the frame: interactive runs only, not the harness's deterministic ones.
            nvrhi::AutoPtr<app::RenderGraphVisualizer> visualizer;
            if (!harness.GetOptions().frames)
            {
                visualizer = MAKE_RC_OBJ_PTR(app::RenderGraphVisualizer, deviceManager, &example);
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
                if (!std::strcmp(__argv[i], "-graphstats"))
                    example.WriteGraphStats(__argv[i + 1]);
            }
            if (visualizer)
                deviceManager->RemoveLayer(visualizer);
            deviceManager->RemoveLayer(&example);
        }
    }

    deviceManager->Shutdown();

    return 0;
}
