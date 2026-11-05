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

#include <donut/render/GBufferFillPass.h>
#include <donut/render/ForwardShadingPass.h>
#include <donut/render/TemporalAntiAliasingPass.h>
#include <donut/render/BlitPass.h>
#include <donut/render/RenderGraph.h>
#include <donut/render/RenderGraphLambdaPass.h>
#include <donut/render/DrawStrategy.h>
#include <donut/app/ApplicationBase.h>
#include <donut/app/Camera.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/TextureCache.h>
#include <donut/engine/Scene.h>
#include <donut/engine/FramebufferFactory.h>
#include <donut/engine/BindingCache.h>
#include <donut/app/DeviceManager.h>
#include <donut/app/RenderGraphLayer.h>
#include <donut/app/RenderGraphVisualizer.h>
#include <donut/core/log.h>
#include <donut/core/vfs/VFS.h>
#include <donut/core/math/math.h>
#include <nvrhi/utils.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>


using namespace donut;
using namespace donut::math;

#include "../variable_shading/lighting_cb.h"
#include "../sample_harness/SampleHarness.h"

static const char* g_WindowTitle = "Donut Example: Variable Rate Shading (render graph)";

// The variable-rate-shading sample on a render graph (aggregate plan docs/plans/2026-11-05-render-graph.md, step P2.2).
// The frame of variable_shading, in its order:
//   1. TAA's motion vectors (from the second frame on), from last frame's depth;
//   2. a compute pass writes the shading-rate surface from the motion vectors and last frame's colour;
//   3. the forward pass clears colour and depth and draws the scene, opaque then transparent, with the surface as the
//      framebuffer's shading-rate attachment: a ShadingRateRead access;
//   4. TAA's resolve; 5. a blit to the back buffer.
// Colour, depth and motion vectors carry from one frame to the next, as the original's targets do: persistent
// resources. The surface and the resolved colour are transients; TAA's feedback is its history pair.
//
// Beside the harness's options: -graphstats F, -nocache, -notimers and -emptybodies (as deferred_shading_rdg; with
// empty bodies, TAA and the blit are lambda passes with their accesses).

class VariableShadingGraph;

// The layer that records each frame; the sample, an ApplicationBase for its scene loading, renders through it.
class FrameGraphLayer : public app::RenderGraphLayer
{
    NVRHI_INHERIT_INTERFACE_TABLE()
public:
    FrameGraphLayer(app::DeviceManager* deviceManager, VariableShadingGraph* sample)
        : RenderGraphLayer(deviceManager), m_Sample(sample)
    { }
    void BuildGraph(render::IRenderGraph* graph, render::FrameData& frameData, render::ResourceId backBuffer) override;
private:
    VariableShadingGraph* m_Sample;
};

class VariableShadingGraph : public app::ApplicationBase
{
    NVRHI_INHERIT_INTERFACE_TABLE()
private:
    nvrhi::AutoPtr<vfs::RootFileSystem> m_RootFS;
    nvrhi::AutoPtr<engine::ShaderFactory> m_ShaderFactory;
    nvrhi::AutoPtr<engine::Scene> m_Scene;
    nvrhi::AutoPtr<render::ForwardShadingPass> m_ForwardPass;
    nvrhi::AutoPtr<render::TemporalAntiAliasingPass> m_TemporalPass;
    nvrhi::AutoPtr<render::BlitPass> m_BlitPass;
    nvrhi::AutoPtr<FrameGraphLayer> m_Graph;
    app::FirstPersonCamera m_Camera;
    engine::PlanarView m_View;
    engine::PlanarView m_VrsView;          // m_View with variable-rate shading on: the forward pass's
    engine::PlanarView m_ViewPrevious;
    bool m_PreviousViewsValid = false;
    nvrhi::AutoPtr<engine::DirectionalLight> m_SunLight;
    nvrhi::AutoPtr<render::InstancedOpaqueDrawStrategy> m_OpaqueDrawStrategy;
    nvrhi::AutoPtr<render::TransparentDrawStrategy> m_TransparentDrawStrategy;
    render::ForwardShadingPass::Context m_ForwardContext;
    std::unique_ptr<engine::BindingCache> m_BindingCache;

    nvrhi::ShaderHandle m_ShadingRateShader;
    nvrhi::ComputePipelineHandle m_ShadingRatePipeline;
    nvrhi::BindingLayoutHandle m_ShadingRateLayout;
    uint32_t m_VrsTileSize = 16;
    bool m_EmptyBodies = false;

    // Measurements (P2.1's switches).
    uint32_t m_Warmup = 100;
    uint32_t m_Frame = 0;
    std::vector<double> m_FrameCostMs;
    std::vector<double> m_ExecuteMs;
    std::vector<uint32_t> m_Barriers;
    render::RunStatistics m_LastRun;

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
        std::filesystem::path sceneFileName = app::GetDirectoryWithExecutable().parent_path() / "media/glTF-Sample-Assets/Models/Sponza/glTF/Sponza.gltf";
        std::filesystem::path frameworkShaderPath = app::GetDirectoryWithExecutable() / "shaders/framework" / app::GetShaderTypeName(GetDevice()->getGraphicsAPI());
        std::filesystem::path appShaderPath = app::GetDirectoryWithExecutable() / "shaders/variable_shading" / app::GetShaderTypeName(GetDevice()->getGraphicsAPI());

        m_RootFS = MAKE_RC_OBJ_PTR(vfs::RootFileSystem);
        m_RootFS->mount("/shaders/donut", frameworkShaderPath);
        m_RootFS->mount("/shaders/app", appShaderPath);

        m_ShaderFactory = MAKE_RC_OBJ_PTR(engine::ShaderFactory, GetDevice(), m_RootFS, "/shaders");
        m_CommonPasses = MAKE_RC_OBJ_PTR(engine::CommonRenderPasses, GetDevice(), m_ShaderFactory);
        m_BindingCache = std::make_unique<engine::BindingCache>(GetDevice());

        m_ShadingRateShader = m_ShaderFactory->CreateShader("/shaders/app/shaders.hlsl", "main_cs", nullptr, nvrhi::ShaderType::Compute);
        if (!m_ShadingRateShader)
            return false;

        auto nativeFS = MAKE_RC_OBJ_PTR(vfs::NativeFileSystem);
        m_TextureCache = MAKE_RC_OBJ_PTR(engine::TextureCache, GetDevice(), nativeFS, nullptr);

        SetAsynchronousLoadingEnabled(false);
        BeginLoadingScene(nativeFS, sceneFileName);

        m_OpaqueDrawStrategy = MAKE_RC_OBJ_PTR(render::InstancedOpaqueDrawStrategy);
        m_TransparentDrawStrategy = MAKE_RC_OBJ_PTR(render::TransparentDrawStrategy);

        m_SunLight = MAKE_RC_OBJ_PTR(engine::DirectionalLight);
        m_Scene->GetSceneGraph()->AttachLeafNode(m_Scene->GetSceneGraph()->GetRootNode(), m_SunLight);
        m_SunLight->SetDirection(double3(0.1, -1.0, 0.15));
        m_SunLight->SetName("Sun");
        m_SunLight->angularSize = 0.53f;
        m_SunLight->irradiance = 2.f;

        m_Scene->FinishedLoading(GetFrameIndex());

        m_Camera.LookAt(float3(0.f, 1.8f, 0.f), float3(1.f, 1.8f, 0.f));
        m_Camera.SetMoveSpeed(3.f);

        nvrhi::VariableRateShadingFeatureInfo info = {};
        GetDevice()->queryFeatureSupport(nvrhi::Feature::VariableRateShading, &info, sizeof(info));
        m_VrsTileSize = info.shadingRateImageTileSize;

        m_ForwardPass = MAKE_RC_OBJ_PTR(render::ForwardShadingPass, GetDevice(), m_CommonPasses);
        m_ForwardPass->Init(*m_ShaderFactory, render::ForwardShadingPass::CreateParameters());

        render::TemporalAntiAliasingPass::CreateParameters taaParams;
        taaParams.motionVectorStencilMask = 0x01;
        taaParams.useCatmullRomFilter = true;
        m_TemporalPass = MAKE_RC_OBJ_PTR(render::TemporalAntiAliasingPass, GetDevice(), m_ShaderFactory, m_CommonPasses, taaParams);
        m_BlitPass = MAKE_RC_OBJ_PTR(render::BlitPass, GetDevice(), m_CommonPasses);

        nvrhi::BindingLayoutDesc layoutDesc;
        layoutDesc.visibility = nvrhi::ShaderType::Compute;
        layoutDesc.bindings = {
            nvrhi::BindingLayoutItem::Texture_UAV(0),
            nvrhi::BindingLayoutItem::Texture_SRV(0),
            nvrhi::BindingLayoutItem::Texture_SRV(1)
        };
        GetDevice()->createBindingLayout(layoutDesc, &m_ShadingRateLayout);
        nvrhi::ComputePipelineDesc psoDesc = {};
        psoDesc.CS = m_ShadingRateShader;
        psoDesc.bindingLayouts = { m_ShadingRateLayout };
        GetDevice()->createComputePipeline(psoDesc, &m_ShadingRatePipeline);

        m_Graph = MAKE_RC_OBJ_PTR(FrameGraphLayer, GetDeviceManager(), this);
        m_Warmup = warmup;
        bool timers = true;
        for (int i = 1; i < argc; ++i)
        {
            if (!std::strcmp(argv[i], "-nocache"))
                m_Graph->GetCompileOptions().maxCachedPlans = 0;
            else if (!std::strcmp(argv[i], "-notimers"))
                timers = false;
            else if (!std::strcmp(argv[i], "-emptybodies"))
                m_EmptyBodies = true;
        }
        m_Graph->GetExecuteDesc().timers = timers;

        GetDevice()->waitForIdle();
        return true;
    }

    FrameGraphLayer* GetGraphLayer() const { return m_Graph; }
    engine::ShaderFactory* GetShaderFactory() const { return m_ShaderFactory; }

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
        m_Camera.Animate(fElapsedTimeSeconds);
        GetDeviceManager()->SetInformativeWindowTitle(g_WindowTitle);
    }

    void BackBufferResizing() override
    {
        m_BindingCache->Clear();
        m_PreviousViewsValid = false;
        if (m_Graph)
            m_Graph->BackBufferResizing();
    }

    void Render(nvrhi::IFramebuffer* framebuffer) override
    {
        const auto start = std::chrono::steady_clock::now();
        m_Graph->Render(framebuffer);
        const double cost = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        m_Graph->GetRegistry()->GetRunStatistics(m_LastRun);
        if (m_Frame++ >= m_Warmup)
        {
            m_FrameCostMs.push_back(cost);
            m_ExecuteMs.push_back(double(m_LastRun.recordMilliseconds) + double(m_LastRun.submitMilliseconds));
            m_Barriers.push_back(m_LastRun.barriers);
        }

        // As the original after its resolve: this frame's view is the next frame's previous one.
        m_ViewPrevious.SetViewport(m_View.GetViewport());
        m_ViewPrevious.SetVariableRateShadingState(m_View.GetVariableRateShadingState());
        m_ViewPrevious.SetMatrices(m_View.GetViewMatrix(), m_View.GetProjectionMatrix(false));
        m_ViewPrevious.SetPixelOffset(m_View.GetPixelOffset());
        m_ViewPrevious.UpdateCache();
        m_PreviousViewsValid = true;
    }

    void BuildGraph(render::IRenderGraph* graph, render::ResourceId backBuffer)
    {
        nvrhi::TextureDesc backBufferDesc;
        if (graph->GetTextureDesc(backBuffer, backBufferDesc) != nvrhi::FS_OK)
            return;
        const uint32_t width = backBufferDesc.width;
        const uint32_t height = backBufferDesc.height;

        const nvrhi::Viewport windowViewport = nvrhi::Viewport(float(width), float(height));
        m_View.SetViewport(windowViewport);
        m_View.SetMatrices(m_Camera.GetWorldToViewMatrix(), perspProjD3DStyleReverse(dm::PI_f * 0.25f, windowViewport.width() / windowViewport.height(), 0.1f));
        m_View.UpdateCache();
        m_VrsView.SetViewport(windowViewport);
        m_VrsView.SetMatrices(m_View.GetViewMatrix(), m_View.GetProjectionMatrix(false));
        m_VrsView.SetVariableRateShadingState(nvrhi::VariableRateShadingState().setEnabled(true)
            .setShadingRate(nvrhi::VariableShadingRate::e1x1).setImageCombiner(nvrhi::ShadingRateCombiner::Override));
        m_VrsView.UpdateCache();

        // The original's RenderTargets, carried from frame to frame, with their optimized clear value (0), which lets
        // D3D12 clear them fast.
        nvrhi::TextureDesc depthDesc = nvrhi::TextureDesc().setWidth(width).setHeight(height).setFormat(nvrhi::Format::D24S8)
            .setIsRenderTarget(true).setIsTypeless(true).setClearValue(nvrhi::Color(0.f)).setDebugName("DepthBuffer");
        render::ResourceId depth = graph->GetPersistentTexture("VariableShading.Depth", depthDesc);
        render::ResourceId hdr = graph->GetPersistentTexture("VariableShading.HdrColor", nvrhi::TextureDesc().setWidth(width)
            .setHeight(height).setFormat(nvrhi::Format::RGBA16_FLOAT).setIsRenderTarget(true).setIsUAV(true)
            .setClearValue(nvrhi::Color(0.f)).setDebugName("HdrColor"));
        render::ResourceId motionVectors = graph->GetPersistentTexture("VariableShading.MotionVectors", nvrhi::TextureDesc()
            .setWidth(width).setHeight(height).setFormat(nvrhi::Format::RG16_FLOAT).setIsRenderTarget(true).setIsUAV(true)
            .setClearValue(nvrhi::Color(0.f)).setDebugName("MotionVectors"));

        // 1. Motion vectors, from the second frame on.
        render::TemporalAntiAliasingGraphInputs taa;
        taa.view = &m_View;
        taa.viewPrevious = m_PreviousViewsValid ? &m_ViewPrevious : nullptr;
        taa.depth = depth;
        taa.motionVectors = motionVectors;
        taa.feedbackFormat = nvrhi::Format::RGBA16_SNORM;
        struct NoData { uint32_t unused; };
        if (m_PreviousViewsValid && m_EmptyBodies)
        {
            render::AddLambdaPass<NoData>(graph, render::PassDesc{ "MotionVectors", render::PassType::Raster, render::PassFlags::None, 0 },
                [&](render::ISetupContext* s, NoData&) {
                    s->Read(depth, render::Access::ShaderRead, nvrhi::ShaderType::Pixel, nvrhi::SubresourceRegion());
                    motionVectors = s->SetColorTarget(0, motionVectors, nvrhi::SubresourceRegion(), render::AccessFlags::None, nullptr);
                },
                [](render::IPassContext*, const NoData&) { });
        }
        else if (m_PreviousViewsValid)
            motionVectors = m_TemporalPass->AddMotionVectorsToGraph(graph, taa);

        // 2. The shading-rate surface: about one texel per tile.
        const uint32_t tilesX = (width + m_VrsTileSize - 1) / m_VrsTileSize;
        const uint32_t tilesY = (height + m_VrsTileSize - 1) / m_VrsTileSize;
        nvrhi::TextureDesc surfaceDesc = nvrhi::TextureDesc().setWidth(tilesX).setHeight(tilesY).setFormat(nvrhi::Format::R8_UINT)
            .setIsUAV(true).setDebugName("ShadingRateTexture");
        surfaceDesc.isShadingRateSurface = true;
        render::ResourceId surface = graph->CreateTexture(surfaceDesc, render::ResourceFlags::None);
        struct ShadingRateData { render::ResourceId surface, motionVectors, hdr; uint32_t tilesX, tilesY; };
        const ShadingRateData shadingRate{ surface, motionVectors, hdr, tilesX, tilesY };
        render::AddLambdaPass<ShadingRateData>(graph, render::PassDesc{ "ShadingRate", render::PassType::Compute, render::PassFlags::None, 0 },
            [&](render::ISetupContext* s, ShadingRateData& data) {
                data = shadingRate;
                s->Read(data.motionVectors, render::Access::ShaderRead, nvrhi::ShaderType::Compute, nvrhi::SubresourceRegion());
                s->Read(data.hdr, render::Access::ShaderRead, nvrhi::ShaderType::Compute, nvrhi::SubresourceRegion());
                data.surface = s->Write(data.surface, render::Access::UAVWrite, nvrhi::ShaderType::Compute, nvrhi::SubresourceRegion(),
                                        render::AccessFlags::FullWrite);
                surface = data.surface;
            },
            [this](render::IPassContext* context, const ShadingRateData& data) {
                if (m_EmptyBodies)
                    return;
                nvrhi::BindingSetDesc bindings;
                bindings.bindings = {
                    nvrhi::BindingSetItem::Texture_UAV(0, context->GetTexture(data.surface), nvrhi::Format::R8_UINT),
                    nvrhi::BindingSetItem::Texture_SRV(0, context->GetTexture(data.motionVectors), nvrhi::Format::RG16_FLOAT),
                    nvrhi::BindingSetItem::Texture_SRV(1, context->GetTexture(data.hdr), nvrhi::Format::RGBA16_FLOAT)
                };
                nvrhi::ComputeState state;
                state.pipeline = m_ShadingRatePipeline;
                state.bindings = { m_BindingCache->GetOrCreateBindingSet(bindings, m_ShadingRateLayout) };
                nvrhi::ICommandList* commandList = context->GetCommandList();
                commandList->setComputeState(state);
                commandList->dispatch(data.tilesX, data.tilesY, 1);
            });

        // 3. The forward pass: the original's clears, PrepareLights (a volatile buffer, so on this pass's list) and the
        // two draws, with the surface attached. Tracked: the scene's buffers and textures stay with nvrhi's tracker.
        struct ForwardData { render::ResourceId surface, hdr, depth; };
        ForwardData forward{ surface, hdr, depth };
        static const render::ClearValue c_ColorClear = { { 0.f, 0.f, 0.f, 0.f }, 0.f, 0 };
        static const render::ClearValue c_DepthClear = { { 0.f, 0.f, 0.f, 0.f }, 0.f, 0 };
        render::AddLambdaPass<ForwardData>(graph, render::PassDesc{ "ForwardShading", render::PassType::Raster, render::PassFlags::Tracked, 0 },
            [&](render::ISetupContext* s, ForwardData& data) {
                data = forward;
                s->Read(data.surface, render::Access::ShadingRateRead, nvrhi::ShaderType::None, nvrhi::SubresourceRegion());
                data.hdr = s->SetColorTarget(0, data.hdr, nvrhi::SubresourceRegion(), render::AccessFlags::Clear, &c_ColorClear);
                data.depth = s->SetDepthTarget(data.depth, nvrhi::SubresourceRegion(), false, render::AccessFlags::Clear, &c_DepthClear);
                forward = data;
            },
            [this](render::IPassContext* context, const ForwardData&) {
                if (m_EmptyBodies)
                    return;
                nvrhi::ICommandList* commandList = context->GetCommandList();
                nvrhi::IFramebuffer* framebuffer = context->GetFramebuffer();
                m_ForwardPass->PrepareLights(m_ForwardContext, commandList, m_Scene->GetSceneGraph()->GetLights(),
                                             float3(0.2f), float3(0.2f), {});
                // RenderCompositeView's steps, with the graph's framebuffer: each strategy is prepared for the view.
                engine::SceneGraphNode* root = m_Scene->GetSceneGraph()->GetRootNode();
                m_OpaqueDrawStrategy->PrepareForView(root, m_VrsView);
                render::RenderView(commandList, &m_VrsView, &m_VrsView, framebuffer, *m_OpaqueDrawStrategy, *m_ForwardPass, m_ForwardContext);
                m_TransparentDrawStrategy->PrepareForView(root, m_VrsView);
                render::RenderView(commandList, &m_VrsView, &m_VrsView, framebuffer, *m_TransparentDrawStrategy, *m_ForwardPass, m_ForwardContext);
            });

        // 4. TAA's resolve, at full rate; 5. the blit.
        taa.motionVectors = motionVectors;
        taa.unresolvedColor = forward.hdr;
        taa.resolvedColor = graph->CreateTexture(nvrhi::TextureDesc().setWidth(width).setHeight(height)
            .setFormat(nvrhi::Format::RGBA16_FLOAT).setIsRenderTarget(true).setIsUAV(true).setClearValue(nvrhi::Color(0.f))
            .setDebugName("ResolvedColor"), render::ResourceFlags::None);
        if (m_EmptyBodies)
        {
            render::ResourceId resolved = taa.resolvedColor;
            render::AddLambdaPass<NoData>(graph, render::PassDesc{ "TemporalAA", render::PassType::Compute, render::PassFlags::None, 0 },
                [&](render::ISetupContext* s, NoData&) {
                    s->Read(taa.unresolvedColor, render::Access::ShaderRead, nvrhi::ShaderType::Compute, nvrhi::SubresourceRegion());
                    s->Read(taa.motionVectors, render::Access::ShaderRead, nvrhi::ShaderType::Compute, nvrhi::SubresourceRegion());
                    resolved = s->Write(resolved, render::Access::UAVWrite, nvrhi::ShaderType::Compute, nvrhi::SubresourceRegion(),
                                        render::AccessFlags::FullWrite);
                },
                [](render::IPassContext*, const NoData&) { });
            render::AddLambdaPass<NoData>(graph, render::PassDesc{ "Blit", render::PassType::Raster, render::PassFlags::None, 0 },
                [&](render::ISetupContext* s, NoData&) {
                    s->Read(resolved, render::Access::ShaderRead, nvrhi::ShaderType::Pixel, nvrhi::SubresourceRegion());
                    backBuffer = s->SetColorTarget(0, backBuffer, nvrhi::SubresourceRegion(), render::AccessFlags::FullWrite, nullptr);
                },
                [](render::IPassContext*, const NoData&) { });
            return;
        }
        const render::ResourceId resolved = m_TemporalPass->AddResolveToGraph(graph, taa);

        render::BlitGraphInputs blit;
        blit.source = resolved;
        blit.target = backBuffer;
        m_BlitPass->AddToGraph(graph, blit);
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
        m_Graph->GetRegistry()->GetRegistryStatistics(registry);
        std::fprintf(file, "{\n  \"frames\": %u,\n  \"warmup\": %u,\n  \"cache\": %s,\n  \"timers\": %s,\n  \"empty_bodies\": %s,\n"
            "  \"median_layer_frame_ms\": %.4f,\n  \"median_execute_ms\": %.4f,\n  \"barriers_min\": %u,\n  \"barriers_max\": %u,\n"
            "  \"passes\": %u,\n  \"segments\": %u,\n  \"plan_hits\": %llu,\n  \"plan_misses\": %llu\n}\n",
            m_Frame, m_Warmup, m_Graph->GetCompileOptions().maxCachedPlans ? "true" : "false",
            m_Graph->GetExecuteDesc().timers ? "true" : "false", m_EmptyBodies ? "true" : "false",
            Median(m_FrameCostMs), Median(m_ExecuteMs), m_Barriers.empty() ? 0u : barriersMin, barriersMax,
            m_LastRun.passes, m_LastRun.segments, (unsigned long long)registry.planHits, (unsigned long long)registry.planMisses);
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
    if (api == nvrhi::GraphicsAPI::D3D11)
    {
        log::error("The Variable Rate Shading example does not support D3D11.");
        return 1;
    }

    auto deviceManager = nvrhi::TakeOver(app::DeviceManager::Create(api));

    app::DeviceCreationParameters deviceParams;
#ifdef _DEBUG
    deviceParams.enableDebugRuntime = true;
    deviceParams.enableNvrhiValidationLayer = true;
#endif

    if (!deviceManager->CreateWindowDeviceAndSwapChain(deviceParams, g_WindowTitle))
    {
        log::error("Cannot initialize a graphics device with the requested parameters");
        return 1;
    }

    if (!deviceManager->GetDevice()->queryFeatureSupport(nvrhi::Feature::VariableRateShading))
    {
        log::error("The device does not support Variable Rate Shading");
        return 1;
    }

    {
        sample_harness::Harness harness(deviceManager, sample_harness::ParseOptions(__argc, __argv));
        VariableShadingGraph example(deviceManager);
        if (example.Init(__argc, __argv, harness.GetOptions().warmup))
        {
            deviceManager->AddLayerToBack(&example);
            nvrhi::AutoPtr<app::RenderGraphVisualizer> visualizer;
            if (!harness.GetOptions().frames)
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
