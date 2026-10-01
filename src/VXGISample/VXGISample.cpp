#include <donut/engine/ShaderFactory.h>
#include <donut/engine/Scene.h>
#include <donut/engine/TextureCache.h>
#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/BindingCache.h>
#include <donut/render/DrawStrategy.h>
#include <donut/render/GBufferFillPass.h>
#include <donut/render/PlanarShadowMap.h>
#include <donut/render/DepthPass.h>
#include <donut/app/DeviceManager.h>
#include <donut/app/ApplicationBase.h>
#include <donut/app/Camera.h>
#include <donut/app/ImGuiRenderPass.h>
#include "VXGITypes.h"
#include "VoxelShadingPass.h"
#include <nvrhi/utils.h>
#include <donut/app/CArgs.h>

#include "SampleRenderTargets.h"
#include "SampleGBufferFillPass.h"
#include "SampleDeferredLightingPass.h"

enum class AOMethod {
    None = 0,
    SSAO = 1,
    VXAO = 2
};

struct Config {

    // { RHI Flags
    int adapterIndex = 0;

    // Camera
    float cameraClipNear = 1.f;
    float cameraClipFar = 10000.f;

    // Scene settings
    float lightSize = 2500.f;
    // Scene description to load. The .obj variant goes through Assimp and is kept
    // as a backward comparison counterpart for the glTF importer path.
    const char *sceneFile = "sponza-plus-gltf.scene.json";

    // VXGI common settings
    bool enableVXGI = true;
    float clipmapRange = 512.f;
    vxgi::EmittanceFormat emittanceFormat = vxgi::EmittanceFormat::PERFORMANCE;

    // VXGI tracing settings
    float ambientScale = 0.f;
    float diffuseScale = 1.f;
    float specularScale = 1.f;
    int traceSparsity = 4;
    int numCones = 8;
    bool enableMultiBounce = true;
    float multiBounceScale = 1.f;
    bool useRefinement = true;

    // VXAO
    AOMethod aoMethod = AOMethod::None;
    float ambientRange = 512.f;
    bool visualizeVXAO = false;

    // VXGI debug
    vxgi::DebugRenderMode debugRenderMode = vxgi::DebugRenderMode::DISABLED;
    int debugLevel = 0;
};

class VXGISample : public donut::app::ApplicationBase {
 public:
    friend class UIPass;

    VXGISample(const Config *config, donut::app::DeviceManager *deviceManager) : ApplicationBase(deviceManager), m_Config(*config) {}

    donut::engine::ShaderFactory *GetShaderFactory() { return m_ShaderFactory; }

    bool Init() {
        auto rootDir = donut::app::GetDirectoryWithExecutable();
        auto frameworkShaderPath =
            rootDir / "shaders/framework" /
            donut::app::GetShaderTypeName(GetDevice()->getGraphicsAPI());
        auto appShaderPath = rootDir / "shaders/VXGISample" /
                             donut::app::GetShaderTypeName(GetDevice()->getGraphicsAPI());
        // Scene descriptions live in etherealsamples/assets/vxgi; the directory is baked in at configure time.
        auto sceneFilePath = std::filesystem::path(ETHEREAL_ASSETS_DIR) / "vxgi" / m_Config.sceneFile;

        m_RootFS = MAKE_RC_OBJ_PTR(donut::vfs::RootFileSystem);
        m_RootFS->mount("/shaders/donut", frameworkShaderPath);
        m_RootFS->mount("/shaders/app", appShaderPath);

        m_ShaderFactory = MAKE_RC_OBJ_PTR(donut::engine::ShaderFactory, GetDevice(),
                                          m_RootFS, "/shaders");

        m_CommonPasses = MAKE_RC_OBJ_PTR(donut::engine::CommonRenderPasses, GetDevice(),
                                         m_ShaderFactory);
        m_BindingCache = nvrhi::MakeMono<donut::engine::BindingCache>(GetDevice());

        auto nativeFS = MAKE_RC_OBJ_PTR(donut::vfs::NativeFileSystem);
        m_TextureCache =
            MAKE_RC_OBJ_PTR(donut::engine::TextureCache, GetDevice(),
                            nativeFS, nullptr);

        SetAsynchronousLoadingEnabled(false);
        BeginLoadingScene(nativeFS, sceneFilePath);

        m_Scene->FinishedLoading(GetFrameIndex());

        // Init camera
        m_Camera.LookAt({0.f, 200.f, 0.f}, {-100.f, 200.f, 0.f});
        m_Camera.SetMoveSpeed(500.f);
        m_Camera.SetRotateSpeed(0.005f);

        auto lights = m_Scene->GetSceneGraph()->GetLights();
        m_SunLight = dynamic_cast<donut::engine::DirectionalLight *>(lights[0].Get());

        m_OpaqueDrawStrategy = MAKE_RC_OBJ_PTR(donut::render::InstancedOpaqueDrawStrategy);
        m_TransparentDrawStrategy = MAKE_RC_OBJ_PTR(donut::render::TransparentDrawStrategy);

        m_RenderTargets = nvrhi::MakeMono<SampleRenderTargets>(GetDevice());

        {
            donut::render::GBufferFillPass::CreateParameters gbufferParams;
            gbufferParams.trackLiveness = false;
            m_GBufferFillPass = MAKE_RC_OBJ_PTR(SampleGBufferFillPass, GetDevice(), m_CommonPasses);
            m_GBufferFillPass->Init(*m_ShaderFactory, gbufferParams);
        }

        {
            m_DeferredLightingPass =
                MAKE_RC_OBJ_PTR(SampleDeferredLightingPass, GetDevice(), m_CommonPasses);
            m_DeferredLightingPass->Init(m_ShaderFactory);
        }

        // Shadow Map
        {
            const nvrhi::Format shadowMapFormats[] = {
                nvrhi::Format::D24S8,
                nvrhi::Format::D32,
                nvrhi::Format::D16,
                nvrhi::Format::D32S8
            };

            const nvrhi::FormatSupport shadowMapFeatures = nvrhi::FormatSupport::Texture |
                                                           nvrhi::FormatSupport::DepthStencil |
                                                           nvrhi::FormatSupport::ShaderLoad;

            nvrhi::Format shadowMapFormat = nvrhi::utils::ChooseFormat(GetDevice(), shadowMapFeatures,
                                                                       shadowMapFormats, dim(shadowMapFormats));

            m_ShadowMap = MAKE_RC_OBJ_PTR(donut::render::PlanarShadowMap, GetDevice(), 2048, shadowMapFormat);
            m_ShadowMap->SetupProxyView();
            m_RenderTargets->CreateShadowFramebuffer(m_ShadowMap->GetTexture());

            donut::render::DepthPass::CreateParameters shadowDepthParams;
            shadowDepthParams.slopeScaledDepthBias = 4.f;
            shadowDepthParams.depthBias = 100;
            m_ShadowDepthPass = MAKE_RC_OBJ_PTR(donut::render::DepthPass, GetDevice(), m_CommonPasses);
            m_ShadowDepthPass->Init(*m_ShaderFactory, shadowDepthParams);

            m_SunLight->shadowMap = m_ShadowMap;
        }

        // Voxelization Pass
        {
            m_VoxelDrawStrategy = MAKE_RC_OBJ_PTR(vxgi::VoxelizationInstancedDrawStrategy);

            vxgi::VoxelShadingPass::CreateParameters voxelParams;
            voxelParams.useInputAssembler = false;
            m_VoxelizationPass = MAKE_RC_OBJ_PTR(vxgi::VoxelShadingPass, GetDevice(), m_ShaderFactory, m_CommonPasses);
            m_VoxelizationPass->Init(voxelParams);
        }

        m_CommandList = GetDevice()->createCommandList();
        m_CommandList->open();

        m_CommandList->close();
        GetDevice()->executeCommandList(m_CommandList);
        GetDevice()->waitForIdle();

        return true;
    }

    bool LoadScene(donut::vfs::IFileSystem *fs, const std::filesystem::path &sceneFilePath) override {
        auto scene = MAKE_RC_OBJ_PTR(donut::engine::Scene, GetDevice(), m_ShaderFactory, fs,
                                     m_TextureCache, nullptr, nullptr);
        if(scene->Load(sceneFilePath)) {
            m_Scene = std::move(scene);
            return true;
        }

        return false;
    }

    void BackBufferResized(const uint32_t width, const uint32_t height,
                           const uint32_t sampleCount) override {
        bool ret = m_RenderTargets->Init({width, height});
        m_BindingCache->Clear();
        m_DeferredLightingPass->ResetBindingCache();
        NVRHI_ASSERT(ret);
    }

    void Animate(float fElapsedTimeInSeconds)  override {
        m_Camera.Animate(fElapsedTimeInSeconds);

        int winWidth, winHeight;
        GetDeviceManager()->GetWindowDimensions(winWidth, winHeight);

        m_Scene->RefreshSceneGraph(GetFrameIndex());

        nvrhi::Viewport windowViewport{float(winWidth), float(winHeight)};
        m_View.SetViewport(windowViewport);
        m_View.SetMatrices(
            m_Camera.GetWorldToViewMatrix(),
            dm::perspProjD3DStyle(dm::PI_f * .25f, windowViewport.width() / windowViewport.height(),
                                  m_Config.cameraClipNear, m_Config.cameraClipFar));
        m_View.UpdateCache();

    }

    bool KeyboardUpdate(int key, int scancode, int action, int mods) override {
        m_Camera.KeyboardUpdate(key, scancode, action, mods);

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

    void Render(nvrhi::IFramebuffer *fb) override {
        auto &fbInfo = fb->getFramebufferInfo();

        m_CommandList->open();

        m_Scene->RefreshBuffers(m_CommandList, GetFrameIndex());
        // Render Shadowmap
        {
            m_ShadowMap->SetupDynamicDirectionalLightView(*m_SunLight, {0.f}, {0.5f * m_Config.lightSize}, {0.f});

            donut::render::DepthPass::Context context;

            m_ShadowMap->Clear(m_CommandList);

            donut::render::RenderCompositeView(m_CommandList, &m_ShadowMap->GetView(), nullptr, m_RenderTargets->ShadowMapFramebuffer,
                                m_Scene->GetSceneGraph()->GetRootNode(), *m_OpaqueDrawStrategy,
                                *m_ShadowDepthPass, context, "ShadowMap", false);
        }

        // VXGI Voxelization
        if(m_Config.enableVXGI)
        {
            vxgi::VoxelizationParameters voxelizationParams;
            voxelizationParams.opacityDirectionCount = vxgi::OpacityDirections::SIX_DIMENSIONAL;
            voxelizationParams.mapSize = 128;
            voxelizationParams.enableMultiBounce = m_Config.enableMultiBounce;
            voxelizationParams.persistentVoxelData = !m_Config.enableMultiBounce;
            voxelizationParams.emittanceFormat = m_Config.emittanceFormat;
            voxelizationParams.enableGeometryShaderPassthrough = true;

            m_VoxelizationPass->SetVoxelizationParameters(voxelizationParams);

            dm::float3 centerPt = m_Camera.GetPosition() + m_Config.clipmapRange * m_Camera.GetDir();

            dm::frustum lightFrusta[2];
            lightFrusta[0] =
                static_cast<const donut::engine::PlanarView &>(m_ShadowMap->GetView()).GetViewFrustum();

            vxgi::UpdateVoxelizationParameters params;
            params.clipmapAnchor = centerPt;
            params.giRange = m_Config.clipmapRange;
            params.indirectIrradianceMapTracingParameters.irradianceScale = m_Config.multiBounceScale;
            params.indirectIrradianceMapTracingParameters.useAutoNormalization = true;

            if(memcmp(&lightFrusta[0], &m_PrevSunLightFrusta, sizeof(lightFrusta[0]))) {
                lightFrusta[1] = m_PrevSunLightFrusta;
                params.invalidatedFrustumCount = 2;
                params.invalidatedLightFrusta = lightFrusta;
                m_PrevSunLightFrusta = lightFrusta[0];
            }

            bool performOpacityVoxelization = false;
            bool performEmittanceVoxelization = false;

            m_VoxelizationPass->PrepareForOpacityVoxelization(
                m_CommandList, params, &performOpacityVoxelization, &performEmittanceVoxelization);

            if(performOpacityVoxelization || performEmittanceVoxelization) {
                dm::float4x4 voxelizationMatrix;
                m_VoxelizationPass->GetVoxelizationViewMatrix(voxelizationMatrix);
                m_VoxelizationView.SetViewport(nvrhi::Viewport{1.f, 1.f}); // Dummy viewport
                m_VoxelizationView.SetMatrices(dm::homogeneousToAffine(voxelizationMatrix), dm::float4x4::identity());
                m_VoxelizationView.UpdateCache();

                const uint32_t maxRegions = 128;
                uint32_t numRegions = 0;
                dm::box3 regions[maxRegions];
                if(m_VoxelizationPass->GetInvalidatedRegions(maxRegions, &numRegions, regions)) {
                    m_VoxelizationView.SetClipRegions(numRegions, regions);

                    vxgi::VoxelShadingPass::Context context;
                    m_VoxelizationPass->PrepareLights(context, m_CommandList,
                                                      m_Scene->GetSceneGraph()->GetLights(), {0.f}, {0.f}, {});

                    if(performOpacityVoxelization) {
                        donut::render::RenderCompositeView(
                            m_CommandList, &m_VoxelizationView, nullptr,
                            m_RenderTargets->VoxelizationDummyFramebuffer, m_Scene->GetSceneGraph()->GetRootNode(),
                            *m_VoxelDrawStrategy, *m_VoxelizationPass, context, "Opacity: Voxelization", false);
                    }

                    if(performEmittanceVoxelization) {
                        m_VoxelizationPass->PrepareForEmittanceVoxelization();

                        m_VoxelizationView.SetClipRegions(0, nullptr);

                        context.isEmittance = true;
                        donut::render::RenderCompositeView(
                            m_CommandList, &m_VoxelizationView, nullptr,
                            m_RenderTargets->VoxelizationDummyFramebuffer, m_Scene->GetSceneGraph()->GetRootNode(),
                            *m_VoxelDrawStrategy, *m_VoxelizationPass, context, "Emittance: Voxelization", false);
                    }
                }
            }

            m_VoxelizationPass->FinalizeVoxelization(m_CommandList);
        }

        // Gbuffer Pass
        {
            m_CommandList->beginMarker("GBuffer");
            m_RenderTargets->ClearGBuffers(m_CommandList);

            donut::render::GBufferFillPass::Context gbufferContext;
            donut::render::RenderCompositeView(m_CommandList, &m_View, &m_View,
                                               m_RenderTargets->GBufferFramebuffer,
                                               m_Scene->GetSceneGraph()->GetRootNode(), *m_OpaqueDrawStrategy,
                                               *m_GBufferFillPass, gbufferContext);
            m_CommandList->endMarker();
        }

        nvrhi::TextureHandle indirectDiffuse, indirectSpecular, ambientOcclusion;

        dm::float3 ambientColor{m_Config.ambientScale};

        // VXGI shading
        if (m_Config.enableVXGI) {
            vxgi::ViewTracerInputBuffers inputBuffers;
            inputBuffers.gbufferViewport = nvrhi::Viewport{(float)m_RenderTargets->Size.x, (float)m_RenderTargets->Size.y};
            inputBuffers.gbufferDepth = m_RenderTargets->GBufferDepth;
            inputBuffers.gbufferNormal = m_RenderTargets->GBufferNormals;
            inputBuffers.viewMatrix = dm::affineToHomogeneous(m_View.GetViewMatrix());
            inputBuffers.projMatrix = m_View.GetProjectionMatrix();

            if (m_Config.debugRenderMode != vxgi::DebugRenderMode::DISABLED) {
                nvrhi::ITexture *gbufferAlbedo = m_RenderTargets->GBufferDiffuse;

                vxgi::DebugRenderParameters params;
                params.debugMode = m_Config.debugRenderMode;
                params.viewMatrix = inputBuffers.viewMatrix;
                params.projMatrix = inputBuffers.projMatrix;
                params.viewport = inputBuffers.gbufferViewport;
                params.destinationTexture = gbufferAlbedo;
                params.destinationDepth = inputBuffers.gbufferDepth;
                params.level = m_Config.debugLevel;

                m_VoxelizationPass->RenderDebug(m_CommandList, params);

                m_CommonPasses->BlitTexture(m_CommandList, fb, gbufferAlbedo, m_BindingCache);
            } else {
                if(m_Config.diffuseScale > 0.f) {
                    vxgi::DiffuseTracingParameters diffuseParams;
                    diffuseParams.numCones = m_Config.numCones;
                    diffuseParams.tracingSparsity = m_Config.traceSparsity;
                    diffuseParams.enableConeRotation = false;
                    diffuseParams.irradianceScale = m_Config.diffuseScale;
                    diffuseParams.ambientColor = ambientColor;
                    diffuseParams.enableSparseTracingRefinement = m_Config.useRefinement;

                    m_VoxelizationPass->ComputeDiffuseChannel(m_CommandList, diffuseParams, &indirectDiffuse,
                                                              &inputBuffers, nullptr);
                }

                if(m_Config.specularScale > 0.f) {
                    vxgi::SpecularTracingParameters specularParams;
                    specularParams.irradianceScale = m_Config.specularScale;
                    specularParams.filter = vxgi::SpecularTracingParameters::FILTER_NONE;

                    m_VoxelizationPass->ComputeSpecularChannel(m_CommandList, specularParams, &indirectSpecular,
                                                               &inputBuffers, nullptr);
                }

                if(m_Config.aoMethod != AOMethod::None) {
                    vxgi::DiffuseTracingParameters diffuseParams;
                    diffuseParams.numCones = m_Config.numCones;
                    diffuseParams.tracingSparsity = m_Config.traceSparsity;
                    diffuseParams.ambientRange = m_Config.ambientRange;
                    diffuseParams.enableSparseTracingRefinement = m_Config.useRefinement;
                    diffuseParams.enableSSAO = m_Config.aoMethod == AOMethod::SSAO;

                    m_VoxelizationPass->ComputeAmbientChannel(m_CommandList, diffuseParams, &ambientOcclusion,
                                                              &inputBuffers, nullptr);
                }
            }
        }

        // Deferred Lighting
        {
            if (!m_Config.enableVXGI || m_Config.debugRenderMode == vxgi::DebugRenderMode::DISABLED) {

                if(m_Config.enableVXGI && m_Config.aoMethod != AOMethod::None && m_Config.visualizeVXAO) {
                    m_CommandList->beginMarker("Visualize VXAO");
                    m_CommonPasses->BlitTexture(m_CommandList, fb, ambientOcclusion, m_BindingCache);
                    m_CommandList->endMarker();
                } else {
                    donut::render::DeferredLightingPass::Inputs deferredInputs;
                    deferredInputs.depth = m_RenderTargets->GBufferDepth;
                    deferredInputs.gbufferDiffuse = m_RenderTargets->GBufferDiffuse;
                    deferredInputs.gbufferSpecular = m_CommonPasses->m_BlackTexture;
                    deferredInputs.gbufferNormals = m_RenderTargets->GBufferNormals;
                    deferredInputs.gbufferEmissive = m_CommonPasses->m_BlackTexture;
                    deferredInputs.indirectDiffuse = indirectDiffuse;
                    deferredInputs.indirectSpecular = indirectSpecular;
                    deferredInputs.ambientOcclusion = ambientOcclusion;
                    deferredInputs.output = m_RenderTargets->HdrColorBuffer;
                    deferredInputs.ambientColorTop = ambientColor;
                    deferredInputs.ambientColorBottom = ambientColor;
                    deferredInputs.lights = &m_Scene->GetSceneGraph()->GetLights();

                    m_DeferredLightingPass->Render(m_CommandList, m_View, deferredInputs);

                    m_CommonPasses->BlitTexture(m_CommandList, fb, m_RenderTargets->HdrColorBuffer,
                                                m_BindingCache);
                }
            }
        }

        m_CommandList->close();

        GetDevice()->executeCommandList(m_CommandList);
    }

 private:
    nvrhi::AutoPtr<donut::vfs::RootFileSystem> m_RootFS;
    nvrhi::AutoPtr<donut::engine::ShaderFactory> m_ShaderFactory;

    nvrhi::MonoPtr<donut::engine::BindingCache> m_BindingCache;
    nvrhi::AutoPtr<donut::engine::Scene> m_Scene;
    nvrhi::AutoPtr<donut::engine::DirectionalLight> m_SunLight;

    donut::engine::PlanarView m_View;
    donut::app::FirstPersonCamera m_Camera;
    Config m_Config;

    nvrhi::CommandListHandle m_CommandList;

    nvrhi::MonoPtr<SampleRenderTargets> m_RenderTargets;
    nvrhi::AutoPtr<donut::render::InstancedOpaqueDrawStrategy> m_OpaqueDrawStrategy;
    nvrhi::AutoPtr<donut::render::TransparentDrawStrategy> m_TransparentDrawStrategy;
    nvrhi::AutoPtr<donut::render::PlanarShadowMap> m_ShadowMap;
    nvrhi::AutoPtr<donut::render::DepthPass> m_ShadowDepthPass;
    nvrhi::AutoPtr<SampleGBufferFillPass> m_GBufferFillPass;
    nvrhi::AutoPtr<SampleDeferredLightingPass> m_DeferredLightingPass;

    // VXGI objects
    vxgi::VoxelizationView m_VoxelizationView;
    nvrhi::AutoPtr<vxgi::VoxelizationInstancedDrawStrategy> m_VoxelDrawStrategy;
    nvrhi::AutoPtr<vxgi::VoxelShadingPass> m_VoxelizationPass;

    dm::frustum m_PrevSunLightFrusta;

    nvrhi::TextureHandle m_DummyShadowMapTexture;
};

class UIPass : public donut::app::ImGuiRenderPass {
public:
    UIPass(VXGISample *sample, donut::app::DeviceManager *deviceManager)
    : ImGuiRenderPass(deviceManager), m_Config(&sample->m_Config) {}

    bool Init(donut::engine::ShaderFactory *pShaderFactory) {
        if(!ImGuiRenderPass::Init(pShaderFactory))
            return false;
        return true;
    }

protected:
   void BuildUI() override {
#define MY_LABEL(label) (ImGui::Text(label), ImGui::SameLine(200.f), ImGui::SetNextItemWidth(-1), "##" #label)
#define MY_LABEL2(label, fmt) (ImGui::Text(label), ImGui::SameLine(200.f), ImGui::SetNextItemWidth(-1), fmt)

       if (ImGui::Begin("Main HUD")) {

            if(ImGui::CollapsingHeader("Frame Statistics", ImGuiTreeNodeFlags_DefaultOpen)) {
                float spf = static_cast<float>(GetDeviceManager()->GetAverageFrameTimeSeconds());
                if(spf) {
                    const float fps = 1.0 / spf;
                    const int precision = (fps <= 20.f) ? 1 : 0;
                    ImGui::Text(MY_LABEL2("FPS", "%.*f"), precision, fps);
                } else
                    ImGui::Text(MY_LABEL2("FPS", "--"));
            }

            if (ImGui::CollapsingHeader("VXGI Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
                if (ImGui::TreeNodeEx("General", ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::Checkbox(MY_LABEL("Enable VXGI"), &m_Config->enableVXGI);

                    ImGui::BeginDisabled(!m_Config->enableVXGI);
                    {
                        int format = (int)m_Config->emittanceFormat - 1;
                        if (ImGui::Combo(MY_LABEL("Emittance format"), (int *)&format,
                                         "Performance (default)\0Quality\0UNORM8\0FLOAT16 NV\0FLOAT32\0")) {
                            m_Config->emittanceFormat = (vxgi::EmittanceFormat)(format + 1);
                        }
                    }

                    ImGui::EndDisabled();
                    ImGui::TreePop();
                }

                ImGui::BeginDisabled(!m_Config->enableVXGI);
                {
                    if (ImGui::TreeNodeEx("Tracing", ImGuiTreeNodeFlags_DefaultOpen)) {
                        ImGui::SliderFloat(MY_LABEL("Ambient scale"), &m_Config->ambientScale, 0.0f, 10.f);
                        ImGui::SliderFloat(MY_LABEL("Diffuse scale"), &m_Config->diffuseScale, 0.0f, 10.f);
                        ImGui::SliderFloat(MY_LABEL("Specular scale"), &m_Config->specularScale, 0.0f, 10.f);
                        ImGui::SliderInt(MY_LABEL("Tracing Sparsity"), &m_Config->traceSparsity, 1, 4);
                        ImGui::SliderInt(MY_LABEL("Number of Cones"), &m_Config->numCones, 4, 32);
                        ImGui::Checkbox(MY_LABEL("Multi-bounce"), &m_Config->enableMultiBounce);
                        ImGui::BeginDisabled(!m_Config->enableMultiBounce);
                        ImGui::SliderFloat(MY_LABEL("Multi-bounce scale"), &m_Config->multiBounceScale, 0.01f,
                                           1.f);
                        ImGui::EndDisabled();
                        ImGui::Checkbox(MY_LABEL("Refine Sparse Tracing"), &m_Config->useRefinement);

                        ImGui::Combo(MY_LABEL("AO type"), (int *)&m_Config->aoMethod, "None\0SSAO\0VXAO");

                        ImGui::BeginDisabled(!(m_Config->aoMethod == AOMethod::VXAO));
                        ImGui::SliderFloat(MY_LABEL("Ambient range"), &m_Config->ambientRange, 10, 10000);
                        ImGui::EndDisabled();
                        ImGui::BeginDisabled(!(m_Config->aoMethod != AOMethod::None));
                        ImGui::Checkbox(MY_LABEL("Visualize VXAO"), &m_Config->visualizeVXAO);
                        ImGui::EndDisabled();

                        ImGui::TreePop();
                    }

                    if (ImGui::TreeNodeEx("Debugging", ImGuiTreeNodeFlags_DefaultOpen)) {
                        ImGui::Combo(MY_LABEL("Debug mode"), (int *)&m_Config->debugRenderMode,
                                     "No debug rendering\0Allocation map\0Opacity map\0Emittance map\0Indirect "
                                     "irradiance map");
                        ImGui::BeginDisabled(!(int)m_Config->debugRenderMode);
                        ImGui::SliderInt(MY_LABEL("Debug level"), &m_Config->debugLevel, 0, 4);
                        ImGui::EndDisabled();

                        ImGui::TreePop();
                    }
                }
                ImGui::EndDisabled();
            }
       }

       ImGui::End();

#undef MY_LABEL
   }
   Config *m_Config;
};

int ParseArgs(int argc, char **argv, Config *config) {
    cag_option options[] = {
        {0x01, NULL, "adapter", "0", "GPU adapter index"},
        {0x02, NULL, "diffuse-factor", "<Diffuse scaling factor>", "VXGI diffuse scaling factor"},
        {0x03, NULL, "specular-factor", "<Specular scaling factor>", "VXGI specular scaling factor"},
        {0x11, NULL, "debug-mode", "<disabled|allocation-map|opacity-texture|emittance-texture>",
         "VXGI debug mode"},
        {0x12, NULL, "debug-level", "0", "VXGI debug level"},
        {0x04, NULL, "scene", "<scene .json under assets/vxgi>", "Scene description to load"},
        {0x05, NULL, "ambient-scale", "<Ambient scaling factor>", "Constant ambient term"},
        {0x06, NULL, "clipmap-range", "<VXGI clipmap range>", "VXGI clipmap half-extent"},
        {0x07, NULL, "multi-bounce", "<0|1>", "Enable VXGI multi-bounce feedback"},
        {-1, "h", "help", "Print this usage message"}
    };

    cag_option_context context;
    const char *optval;

    cag_option_init(&context, options, CAG_ARRAY_SIZE(options), argc, argv);

    while (cag_option_fetch(&context)) {
        switch (cag_option_get_identifier(&context)) {
            case 0x01: {
                optval = cag_option_get_value(&context);
                config->adapterIndex = strtol(optval, NULL, 0);
                if (errno == ERANGE) {
                    donut::log::error("adapter index: numerical result (%s) out of range", optval);
                    return -1;
                }
            } break;
            case 0x02: {
                optval = cag_option_get_value(&context);
                config->diffuseScale = strtof(optval, NULL);
                if (errno == ERANGE) {
                    fprintf(stderr, "option 'diffuse-factor' numerical result (%s) out of range\n", optval);
                    return -1;
                }
            } break;
            case 0x03: {
                optval = cag_option_get_value(&context);
                config->specularScale = strtof(optval, NULL);
                if (errno == ERANGE) {
                    fprintf(stderr, "option 'diffuse-factor' numerical result (%s) out of range\n", optval);
                    return -1;
                }
            } break;
            case 0x04: {
                config->sceneFile = cag_option_get_value(&context);
            } break;
            case 0x05: {
                config->ambientScale = strtof(cag_option_get_value(&context), NULL);
            } break;
            case 0x06: {
                config->clipmapRange = strtof(cag_option_get_value(&context), NULL);
            } break;
            case 0x07: {
                config->enableMultiBounce = strtol(cag_option_get_value(&context), NULL, 0) != 0;
            } break;
            case 0x11: {
                optval = cag_option_get_value(&context);
                if (strcmp(optval, "disabled") == 0) {
                    config->debugRenderMode = vxgi::DebugRenderMode::DISABLED;
                } else if (strcmp(optval, "allocation-map") == 0) {
                    config->debugRenderMode = vxgi::DebugRenderMode::ALLOCATION_MAP;
                } else if (strcmp(optval, "opacity-texture") == 0) {
                    config->debugRenderMode = vxgi::DebugRenderMode::OPACITY_TEXTURE;
                } else if (strcmp(optval, "emittance-texture") == 0) {
                    config->debugRenderMode = vxgi::DebugRenderMode::EMITTANCE_TEXTURE;
                } else if (strcmp(optval, "irradiance-texture") == 0) {
                    config->debugRenderMode = vxgi::DebugRenderMode::INDIRECT_IRRADIANCE_TEXTURE;
                } else {
                    donut::log::error("VXGI debug mode: invalid value (%s)", optval);
                    return -1;
                }
            } break;
            case 0x12: {
                optval = cag_option_get_value(&context);
                config->debugLevel = strtol(optval, NULL, 0);
                if (errno == ERANGE) {
                    donut::log::error("VXGI debug level: numerical result (%s) out of range", optval);
                    return -1;
                }
            } break;
            case -1: {
                cag_option_print(options, CAG_ARRAY_SIZE(options), stdout);
                return -2;
            }
        }
    }

    return 0;
}

#ifdef WIN32
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
#else
int main(int __argc, const char **__argv)
#endif
{
    Config config;

    if(ParseArgs(__argc, __argv, &config))
        return -1;

    donut::log::EnableOutputToMessageBox(false);
    donut::log::EnableOutputToDebug(true);

    nvrhi::GraphicsAPI api = donut::app::GetGraphicsAPIFromCommandLine(__argc, __argv);
    auto deviceManager = nvrhi::TakeOver(donut::app::DeviceManager::Create(api));

    donut::app::GetDirectoryWithExecutable();

    donut::app::DeviceCreationParameters deviceParams;
    // deviceParams.enablePerMonitorDPI = true;
    deviceParams.backBufferWidth = 1280;
    deviceParams.backBufferHeight = 800;
    deviceParams.swapChainFormat = nvrhi::Format::RGBA8_UNORM;
    deviceParams.adapterIndex = config.adapterIndex;
    deviceParams.requiredVulkanDeviceExtensions = {"VK_EXT_depth_clip_enable"};
    deviceParams.multiViewFeature.enabled = true;
    deviceParams.multiViewFeature.maxMultiviewViewCount = 15;
    deviceParams.multiViewFeature.maxMultiviewInstanceIndex = 5;
    // deviceParams.enableDebugRuntime = true;

    if (!deviceManager->CreateWindowDeviceAndSwapChain(deviceParams, "VXGISample")) {
        donut::log::fatal("Cannot initialize a graphics device with the requested parameters");
        return 1;
    }

    {
        VXGISample sample(&config, deviceManager);
        UIPass uiPass(&sample, deviceManager);
        if(sample.Init() && uiPass.Init(sample.GetShaderFactory())) {
            deviceManager->AddRenderPassToBack(&sample);
            deviceManager->AddRenderPassToBack(&uiPass);
            deviceManager->RunMessageLoop();
            deviceManager->RemoveRenderPass(&uiPass);
            deviceManager->RemoveRenderPass(&sample);
        }
    }

    deviceManager->Shutdown();

    return 0;
}