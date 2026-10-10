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

#include <donut/app/ApplicationBase.h>
#include <donut/app/Camera.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/TextureCache.h>
#include <donut/engine/Scene.h>
#include <donut/engine/BindingCache.h>
#include <donut/engine/View.h>
#include <donut/app/DeviceManager.h>
#include <donut/core/log.h>
#include <donut/core/vfs/VFS.h>
#include <donut/core/math/math.h>
#include <nvrhi/utils.h>
#include <donut/app/Camera.h>
#include <stb_image.h>

#include "SampleScene.h"
#include "Config.h"
#include "GlobalResources.h"
#include "PathTracingPass.h"
#include "GBufferPass.h"
#include "DDGIRenderPass.h"
#include "RTAOPass.h"
#include "CompositePass.h"
#include "UIPass.h"

static const char* g_WindowTitle = "DDGISample";

using namespace donut;

class DDGISample : public donut::app::ApplicationBase {
    NVRHI_INHERIT_INTERFACE_TABLE()
 private:
    nvrhi::MonoPtr<Config> m_Config;
    nvrhi::AutoPtr<vfs::RootFileSystem> m_RootFS;
    nvrhi::CommandListHandle m_CommandList;

    nvrhi::AutoPtr<engine::ShaderFactory> m_ShaderFactory;
    nvrhi::AutoPtr<SampleScene> m_Scene;
    nvrhi::AutoPtr<donut::engine::DescriptorTableManager> m_SceneDescriptorTableManager;
    app::FirstPersonCamera m_Camera;
    engine::PlanarView m_View;
    nvrhi::MonoPtr<engine::BindingCache> m_BindingCache;

    nvrhi::MonoPtr<GlobalResources> m_GlobalResources;
    nvrhi::MonoPtr<PathTracingPass> m_PathTracingPass;
    nvrhi::MonoPtr<GBufferPass> m_GBufferPass;
    nvrhi::MonoPtr<DDGIRenderPass> m_DDGIPass;
    nvrhi::MonoPtr<RTAOPass> m_RTAOPass;
    nvrhi::MonoPtr<CompositePass> m_CompositePass;

    float m_PerspectiveFOV;
    bool m_EnableAnimations = true;
    uint32_t m_FrameNumber = 1;

 public:
    using ApplicationBase::ApplicationBase;

    DDGISample(donut::app::DeviceManager *deviceManager)
    : app::ApplicationBase(deviceManager) {}

    bool Init(std::string_view configPath, UIPass *ui) {
        auto nativeFS = MAKE_RC_OBJ_PTR(vfs::NativeFileSystem);
        std::filesystem::path frameworkShaderPath =
            app::GetDirectoryWithExecutable() / "shaders/framework" /
            app::GetShaderTypeName(GetDevice()->getGraphicsAPI());
        std::filesystem::path appShaderPath =
            app::GetDirectoryWithExecutable() / "shaders/DDGISample" /
            app::GetShaderTypeName(GetDevice()->getGraphicsAPI());

        m_RootFS = MAKE_RC_OBJ_PTR(vfs::RootFileSystem);
        m_RootFS->mount("/shaders/donut", frameworkShaderPath);
        m_RootFS->mount("/shaders/app", appShaderPath);

        m_Config = nvrhi::MakeMono<Config>();
        if (!LoadConfigs(nativeFS, configPath, *m_Config)) {
            donut::log::fatal("Can not load config file '%s'", configPath);
            return false;
        }

        m_ShaderFactory =
            MAKE_RC_OBJ_PTR(engine::ShaderFactory, GetDevice(), m_RootFS, "/shaders");

        if (!ui->Init(m_ShaderFactory, m_Config)) {
            return false;
        }

        m_CommonPasses =
            MAKE_RC_OBJ_PTR(engine::CommonRenderPasses, GetDevice(), m_ShaderFactory);
        m_BindingCache = nvrhi::MakeMono<engine::BindingCache>(GetDevice());

        int fbWidth, fbHeight;
        GetDeviceManager()->GetWindowDimensions(fbWidth, fbHeight);

        m_GlobalResources = nvrhi::MakeMono<GlobalResources>();
        m_GlobalResources->Initialize(GetDevice(), dm::uint2(fbWidth, fbHeight),
                                      GetDeviceManager()->GetBackBufferCount());

        m_PathTracingPass = nvrhi::MakeMono<PathTracingPass>();
        m_GBufferPass = nvrhi::MakeMono<GBufferPass>();
        m_DDGIPass = nvrhi::MakeMono<DDGIRenderPass>();
        m_RTAOPass = nvrhi::MakeMono<RTAOPass>();
        m_CompositePass = nvrhi::MakeMono<CompositePass>();

        m_PathTracingPass->Initialize(m_GlobalResources, m_ShaderFactory);
        m_GBufferPass->Initialize(m_GlobalResources, m_ShaderFactory);
        m_DDGIPass->Initialize(m_GlobalResources, m_ShaderFactory);
        m_RTAOPass->Initialize(m_GlobalResources, m_ShaderFactory);
        m_CompositePass->Initialize(m_GlobalResources, m_ShaderFactory);

        // Load Scene
        m_SceneDescriptorTableManager =
            MAKE_RC_OBJ_PTR(donut::engine::DescriptorTableManager, GetDevice(), m_GlobalResources->BindlessLayout);
        m_TextureCache = MAKE_RC_OBJ_PTR(engine::TextureCache, GetDevice(), nativeFS,
                                         m_SceneDescriptorTableManager);

        SetAsynchronousLoadingEnabled(false);
        BeginLoadingScene(nativeFS, m_Config->scene.filePath);

        m_Scene->FinishedLoading(GetFrameIndex());

        auto cameras = m_Scene->GetSceneGraph()->GetCameras();
        if (!cameras.empty()) {
            auto perspectiveCamera =
                donut::query_cast<engine::PerspectiveCamera>(cameras[0].Get());
            auto viewToWorld = perspectiveCamera->GetViewToWorldMatrix();
            m_PerspectiveFOV = dm::radians(perspectiveCamera->verticalFov);
            m_Camera.LookAt(viewToWorld.m_translation, viewToWorld.m_translation + viewToWorld.m_linear[2]);
        }

        m_Camera.SetMoveSpeed(3.f);

        GetDevice()->createCommandList(nvrhi::CommandListParameters(), &m_CommandList);

        m_CommandList->open();

        m_Scene->BuildMeshBLASes(m_CommandList);
        m_Scene->UpdateLightsBuffer(m_CommandList);

        if (!LoadBlueNoiseTexture()) return false;

        m_GlobalResources->SetSceneBuffers(
            m_Scene->GetTopLevelAS(), m_Scene->GetInstanceBuffer(),
            m_Scene->GetGeometryBuffer(), m_Scene->GetMaterialBuffer(),
            m_Scene->GetLightConstantsBuffer(), m_Scene->GetDescriptorTable());

        m_GlobalResources->LoadDDGIVolumes(m_CommandList, m_Config);

        m_GlobalResources->CreateBindingSets();

        m_CommandList->close();
        GetDevice()->executeCommandList(m_CommandList);
        GetDevice()->waitForIdle();

        return true;
    }

    bool LoadScene(vfs::IFileSystem* fs,
                   const std::filesystem::path& sceneFileName) override {
        auto scene = MAKE_RC_OBJ_PTR(SampleScene,
            GetDevice(), m_ShaderFactory, fs, m_TextureCache, m_SceneDescriptorTableManager);

        if (scene->Load(sceneFileName)) {
            m_Scene = std::move(scene);
            return true;
        }

        return false;
    }

    bool LoadBlueNoiseTexture() {
        auto blueNoiseFilePath = std::filesystem::path{m_Config->configFileDir} /
                                 "../data/textures/blue-noise-rgb-256.png";
        int w, h, channel;
        auto data = stbi_load(blueNoiseFilePath.string().c_str(), &w, &h, &channel, 4);
        if (!data) {
            donut::log::error("Load blue noise texture'%s' failed",
                              blueNoiseFilePath.string().c_str());
            return false;
        }

        nvrhi::TextureDesc desc;
        desc.dimension = nvrhi::TextureDimension::Texture2D;
        desc.width = w;
        desc.height = h;
        desc.format = nvrhi::Format::RGBA8_UNORM;
        // Created in Common, the state a new texture is really in on every backend (a Vulkan image starts in
        // VK_IMAGE_LAYOUT_UNDEFINED whatever initialState says); writeTexture moves it to CopyDest.
        desc.initialState = nvrhi::ResourceStates::Common;
        desc.debugName = "Blue Noise";

        nvrhi::TextureHandle blueNoiseTexture;
        GetDevice()->createTexture(desc, &blueNoiseTexture);
        m_CommandList->beginTrackingTextureState(blueNoiseTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);
        m_CommandList->writeTexture(blueNoiseTexture, 0, 0, data, w * 4);
        stbi_image_free(data);
        m_CommandList->setPermanentTextureState(blueNoiseTexture,
                                                nvrhi::ResourceStates::ShaderResource);
        m_CommandList->commitBarriers();
        m_GlobalResources->SetBlueNoiseTexture(blueNoiseTexture);
        return true;
    }

    bool KeyboardUpdate(int key, int scancode, int action, int mods) override {
        m_Camera.KeyboardUpdate(key, scancode, action, mods);

        if (key == GLFW_KEY_SPACE && action == GLFW_PRESS) {
            m_EnableAnimations = !m_EnableAnimations;
            return true;
        }

        return true;
    }

    bool MousePosUpdate(double xpos, double ypos) override {
        m_Camera.MousePosUpdate(xpos, ypos);
        return true;
    }

    bool MouseButtonUpdate(int button, int action, int mods) override {
        m_Camera.MouseButtonUpdate(button, action, mods);
        return true;
    }

    bool MouseScrollUpdate(double xoffset, double yoffset) override {
        m_Camera.MouseScrollUpdate(xoffset, yoffset);
        return true;
    }

    void Update(const Config *config) {
        {
            auto &globalConsts = m_GlobalResources->GlobalConsts;
            globalConsts.app.skyRadiance = config->scene.skyRadiance;

            globalConsts.lights.hasDirectionalLight =
                m_Scene->GetNumDirectionalLights() != 0;
            globalConsts.lights.numSpotLights = m_Scene->GetNumSpotLights();
            globalConsts.lights.numPointLights = m_Scene->GetNumPointLights();
        }

        auto frameConsts = m_GlobalResources->FrameConsts;
        {
            nvrhi::Viewport windowViewport(m_GlobalResources->Size.x,
                                           m_GlobalResources->Size.y);
            bool resizeWindow = m_View.GetViewport().width() != windowViewport.width() ||
                                m_View.GetViewport().height() != windowViewport.height();
            m_View.SetViewport(windowViewport);
            m_View.SetMatrices(m_Camera.GetWorldToViewMatrix(),
                               dm::perspProjD3DStyleReverse(
                                   m_PerspectiveFOV,
                                   windowViewport.width() / windowViewport.height(), 0.1f));
            m_View.UpdateCache();

            Graphics::PlanarViewConstants viewConsts;
            m_View.FillPlanarViewConstants((PlanarViewConstants &)viewConsts);

            if (resizeWindow ||
                any(viewConsts.matWorldToView != frameConsts.view.matWorldToView) ||
                any(viewConsts.matViewToClip != frameConsts.view.matViewToClip))
                m_FrameNumber = 1;

            frameConsts.view = viewConsts;
            frameConsts.frameNumber = m_FrameNumber;
        }

        m_GlobalResources->Update(config, frameConsts);
        m_PathTracingPass->Update(config);
        m_GBufferPass->Update(config);
        m_DDGIPass->Update(config);
        m_RTAOPass->Update(config);
        m_CompositePass->Update(config);
    }

    void Animate(float fElapsedTimeSeconds) override {
        m_Camera.Animate(fElapsedTimeSeconds);

        if (IsSceneLoaded() && m_EnableAnimations) {
            m_Scene->Animate(fElapsedTimeSeconds);
        }

        Update(m_Config);

        GetDeviceManager()->SetInformativeWindowTitle(g_WindowTitle);
    }

    void BackBufferResizing() override {
        m_BindingCache->Clear();
    }

    void BackBufferResized(const uint32_t width, const uint32_t height,
                           const uint32_t sampleCount) override {
        m_GlobalResources->Resize(dm::uint2(width, height));
    }

    void Render(nvrhi::IFramebuffer* framebuffer) override {
        m_CommandList->open();

        m_Scene->Refresh(m_CommandList, GetFrameIndex());
        m_Scene->BuildTLAS(m_CommandList, GetFrameIndex());

        m_GlobalResources->Execute(m_CommandList,
                                   GetDeviceManager()->GetCurrentBackBufferIndex());

        if (m_Config->renderers.renderMode == Config::Renderers::RenderMode::PathTracing) {
            m_PathTracingPass->Execute(m_CommandList);

            m_CommonPasses->BlitTexture(m_CommandList, framebuffer,
                                        m_GlobalResources->PTOutputTexture,
                                        m_BindingCache);
        } else {
            m_GBufferPass->Execute(m_CommandList);
            m_DDGIPass->Execute(m_CommandList);
            m_RTAOPass->Execute(m_CommandList);
            m_CompositePass->Execute(m_CommandList);

            m_CommonPasses->BlitTexture(m_CommandList, framebuffer,
                                        m_GlobalResources->CompositeOutputTexture,
                                        m_BindingCache);
        }

        m_CommandList->close();
        GetDevice()->executeCommandList(m_CommandList);
        ++m_FrameNumber;
    }
};

#ifdef WIN32
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine,
                   int nCmdShow)
#else
int main(int __argc, const char **__argv)
#endif
{
    nvrhi::GraphicsAPI api = app::GetGraphicsAPIFromCommandLine(__argc, __argv);
    auto deviceManager = nvrhi::TakeOver(app::DeviceManager::Create(api));

    app::DeviceCreationParameters deviceParams;
    deviceParams.enablePerMonitorDPI = true;
    deviceParams.enableRayTracingExtensions = true;
    deviceParams.backBufferWidth = 1920;
    deviceParams.backBufferHeight = 1080;
    deviceParams.swapChainFormat = nvrhi::Format::RGBA8_UNORM;

    const char *configPath = nullptr;
    int adapterIndex = 0;
    for (int i = 1; i < __argc; i++) {
        if (strcmp(__argv[i], "--config") == 0) {
            if (i + 1 < __argc) {
                configPath = __argv[++i];
                continue;
            } else {
                log::fatal("--config is provided but no argument is available!");
                return -1;
            }
        } else if (strcmp(__argv[i], "--debug") == 0) {
            deviceParams.enableDebugRuntime = true;
            deviceParams.enableNvrhiValidationLayer = true;
            continue;
        } else if(strcmp(__argv[i], "--adapter") == 0) {
            if (i + 1 < __argc) {
                adapterIndex = std::strtol(__argv[i + 1], nullptr, 10);
                deviceParams.adapterIndex = adapterIndex;
            } else {
                donut::log::fatal("--adapter <number> must be provided!");
                return -1;
            }
        }
    }

    if (!configPath) {
        donut::log::fatal("Config file not available!");
        return -1;
    }

    if (!deviceManager->CreateWindowDeviceAndSwapChain(deviceParams, g_WindowTitle)) {
        log::fatal("Cannot initialize a graphics device with the requested parameters");
        return 1;
    }

    if (!deviceManager->GetDevice()->queryFeatureSupport(
                            nvrhi::Feature::RayTracingPipeline)) {
        log::fatal("The graphics device does not support Ray Tracing Pipelines");
        return 1;
    }

    {
        auto uiPass = MAKE_RC_OBJ_PTR(UIPass, deviceManager);
        auto example = MAKE_RC_OBJ_PTR(DDGISample, deviceManager);
        if (example->Init(configPath, uiPass)) {
            deviceManager->AddRenderPassToBack(example);
            deviceManager->AddRenderPassToBack(uiPass);
            deviceManager->RunMessageLoop();
        }
    }

    deviceManager->Shutdown();

    return 0;
}
