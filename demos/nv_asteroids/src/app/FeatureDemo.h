#pragma once

#include <chrono>

// Asteroids.exe: FeatureDemo (vtable 0x140257F68, ctor 0x1400230B0 "FeatureDemo::Init", 1504 bytes)
//
// The demo's main render pass, derived from the framework's ApplicationBase. The implementation is split into
// FeatureDemo.cpp (constructor), FeatureDemo_Input.cpp (Animate, input), FeatureDemo_Data.cpp (data files),
// FeatureDemo_Scene.cpp (LoadScene, SceneLoaded, CreateRenderPasses, light probes) and FeatureDemo_Render.cpp
// (RenderScene and its helpers, splash screen, SceneUnloading).
//
// Members carry their 2018 offset. Demo classes from other modules are only forward-declared and held
// through std::shared_ptr (so that this header and the destructor do not need their definitions).
// deviation: several of them were std::unique_ptr in the binary.

#include "app/Animation.h"
#include "app/InputReplay.h"
#include "app/DemoLightProbe.h"
#include "meshlets/MeshletDrawStrategy.h"

#include <donut/app/ApplicationBase.h>
#include <donut/app/Camera.h>
// nvrhi::AutoPtr destroys through a complete type, so the passes held by value below
// need their definitions here and not just a forward declaration.
#include <donut/engine/FramebufferFactory.h>
#include <donut/engine/ShadowMap.h>
#include <donut/render/SsaoPass.h>
#include <donut/render/TemporalAntiAliasingPass.h>
#include <donut/core/math/math.h>
#include <nvrhi/nvrhi.h>

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

struct UIData;
class RenderTargets;
class ThirdPersonCamera;

namespace donut::engine
{
    class ShaderFactory;
    class IView;
    class PlanarView;
    class ThreadPool;
    class SceneGraph;
    class DirectionalLight;
    class FramebufferFactory;
    struct LightProbe;
    struct LoadedTexture;
}

namespace donut::render
{
    class InstancedOpaqueDrawStrategy;
    class TransparentDrawStrategy;
    class TemporalAntiAliasingPass;
    class SsaoPass;
    class ToneMappingPass;
    class BloomPass;
    class CascadedShadowMap;
    class LightProbeProcessingPass;
    class PixelReadbackPass;
}

// Demo classes reconstructed by the other modules (src/scene, src/meshlets, src/passes, src/fx).
class SpaceScene;
class PlayerShip;
class SceneLight;
class SceneDirectionalLight;
struct SceneMaterial;
class MeshletDrawStrategy;
class MeshletRenderResources;
class PipelineStatisticsQuery;
class GBufferFillPass2018;
class ForwardShadingPass2018;
class DeferredLightingPass2018;
class ToneMappingPass2018;
class BloomPass2018;
class LightProbeProcessingPass2018;
class HbaoPlusPass;
class SharpenBlitPass2018;
class TemporalAAPass2018;

namespace fx
{
    class RealStarFieldResources;
    class RealStarFieldPass;
    class EnvironmentMapPass;
    class PlanetSet;
    class SunDisk;
    class FogPass;
    class ParticleSystem;
    class ShieldPass;
    class LensFlarePass;
    class HqBlitPass;
    class ShowCubemapPass;
}

// Asteroids.exe: DemoLightProbe (RTTI std::_Ref_count_obj<DemoLightProbe>, 232 bytes, created by 0x14002B920).
// The 2018 framework LightProbe (same fields as donut's) plus the capture parameters set in SceneLoaded.
struct DemoLightProbe;

class FeatureDemo : public donut::app::ApplicationBase
{
    // Adds no interface and no class ID of its own: ApplicationBase's table is correct for it.
    NVRHI_INHERIT_INTERFACE_TABLE()

public:
    // Camera preset of Camera%d.json (40 bytes in the vector at +584).
    struct CameraPreset
    {
        dm::float3 position = 0.f;                          // "pos"
        dm::float3 lookAt = dm::float3(0.f, 0.f, -1.f);     // "look_at"
        dm::float3 up = dm::float3(0.f, 1.f, 0.f);          // "up"
        bool valid = false;
    };

    static constexpr int c_NumCameraPresets = 10;

    FeatureDemo(donut::app::DeviceManager* deviceManager, UIData* uiData, bool asyncLoad);
    ~FeatureDemo() override;

    // IRenderPass (2018 slots 1, 4, 6..10)
    void Animate(float elapsedTimeSeconds) override;                                    // 0x14002A4B0
    // deviation: donut pauses unfocused windows; the 2018 DeviceManager never checked focus, so keep rendering.
    bool ShouldAnimateUnfocused() override { return true; }
    bool ShouldRenderUnfocused() override { return true; }
    bool KeyboardUpdate(int key, int scancode, int action, int mods) override;          // 0x14002EC20
    bool MousePosUpdate(double xpos, double ypos) override;                             // 0x140031900
    bool MouseScrollUpdate(double xoffset, double yoffset) override;                    // 0x140031960
    bool MouseButtonUpdate(int button, int action, int mods) override;                  // 0x1400318E0
    bool JoystickButtonUpdate(int button, bool pressed) override;                       // 0x14002EB90
    bool JoystickAxisUpdate(int axis, float value) override;                            // 0x14002EB00

    // ApplicationBase (2018 slots 11..16)
    void RenderScene(nvrhi::IFramebuffer* framebuffer) override;                        // 0x1400342A0
    void RenderSplashScreen(nvrhi::IFramebuffer* framebuffer) override;                 // 0x1400354A0
    bool LoadScene(donut::vfs::IFileSystem* fs,
        const std::filesystem::path& sceneFileName) override;                           // 0x140031280
    void SceneUnloading() override;                                                     // 0x140037020
    void SceneLoaded() override;                                                        // 0x140036540

    // Used by UIRenderer and main.
    const nvrhi::AutoPtr<donut::vfs::IFileSystem>& GetRootFileSystem() const { return m_RootFs; }
    const std::filesystem::path& GetMediaPath() const { return m_MediaPath; }
    const nvrhi::AutoPtr<donut::engine::ShaderFactory>& GetShaderFactory() const { return m_ShaderFactory; }
    const nvrhi::AutoPtr<donut::engine::TextureCache>& GetTextureCache() const { return m_TextureCache; }
    SpaceScene* GetScene() const { return m_Scene.get(); }
    bool HasSunLight() const { return m_SunLight != nullptr; }
    UIData& GetUIData() const { return *m_UI; }

    // "Play Opening Sequence (P)" and "Reset (O)".
    void PlayOpeningSequence();
    void ResetOpeningSequence();

private:
    // ---- data files (FeatureDemo_Data.cpp) ------------------------------------------------------
    void LoadReplay(int index);                                                         // 0x1400310B0
    void LoadAnimations();                                                              // 0x140030580
    void ApplyAnimation(const Animation::Sequence& sequence, std::optional<float> time,
        bool holdLastValue);                                                            // 0x14002AE20
    void LoadCameraPresets(int count);                                                  // 0x140030760
    void SaveCameraPreset(int index);                                                   // 0x140035810
    void LoadLightProbes(donut::engine::ThreadPool& threadPool);                        // 0x140030D40
    void SaveLightProbes();                                                             // 0x140035D40
    void SaveTextureToFile(nvrhi::ITexture* texture, const nvrhi::AutoPtr<donut::vfs::IFileSystem>& fs,
        const std::filesystem::path& path);                                             // 0x1400360F0

    // ---- input (FeatureDemo_Input.cpp) ------------------------------------------------------------
    bool HandleJoystickAxis(int axis, float value);                                     // 0x1400380D0
    bool HandleJoystickButton(int button, bool pressed);                                // 0x1400381C0
    void ApplyCameraPreset(const CameraPreset& preset);
    void LimitFrameRate();

    // ---- scene and passes (FeatureDemo_Scene.cpp) --------------------------------------------------
    void CreateRenderPasses(bool& exposureResetRequired, nvrhi::IFramebuffer* framebuffer); // 0x14002BD60
    void CreateLightProbeTextures(uint32_t numProbes);                                  // 0x14002B590
    void CreateLightProbeObjects(uint32_t numProbes);                                   // 0x14002B920
    void RenderLightProbes();                                                           // 0x1400327F0

    // ---- per frame (FeatureDemo_Render.cpp) ------------------------------------------------------
    void SetupViewsAndTargets(uint32_t width, uint32_t height, nvrhi::IFramebuffer* framebuffer,
        bool& exposureResetRequired);                                                   // 0x140031EF0
    bool UpdateViews();                                                                 // 0x140037C50
    void UpdateSun();                                                                   // inlined in 0x1400342A0
    void UpdateLights();                                                                // 0x140031980
    void CollectLightProbes(std::vector<nvrhi::AutoPtr<donut::engine::LightProbe>>& probes); // 0x1400377C0
    void UpdateScene(nvrhi::ICommandList* commandList);                                 // 0x140038380
    void UpdateViewDistanceMap(nvrhi::ICommandList* commandList, float cameraHeight, float farDistance); // 0x1400384F0
    void UpdateAsteroidRendererSettings();                                              // 0x140037370
    void RenderShadows();                                                               // 0x140034F70
    void RenderGBuffer();                                                               // 0x140032630
    void RenderLightingAndEffects();                                                    // 0x140033CA0
    void ResolveOrAccumulate(const nvrhi::Viewport& viewport);                          // 0x14002B2A0
    nvrhi::ITexture* PostProcess(const nvrhi::AutoPtr<donut::engine::FramebufferFactory>& framebufferWithDepth,
        const nvrhi::AutoPtr<donut::engine::FramebufferFactory>& framebuffer, nvrhi::ITexture* source); // 0x140033960
    void RenderOverlay(const nvrhi::Viewport& windowViewport, nvrhi::IFramebuffer* framebuffer); // 0x1400321F0
    void RenderShadowView(const donut::engine::IView& view);
    void SaveScreenshotIfRequested(nvrhi::IFramebuffer* framebuffer);

    // ---- 2018 members ---------------------------------------------------------------------------
    // +8 DeviceManager* (IRenderPass), +16 scene-loaded flag, +24 m_TextureCache, +40 loading thread,
    // +48 m_CommonPasses, +64 m_IsAsyncLoad: inherited from ApplicationBase.
    bool m_PreviousViewsValid = false;                                          // +96
    float m_CurrentTime = 0.f;                                                  // +100 demo time (stops when paused)
    float m_ElapsedTime = 0.f;                                                  // +104 last frame step (0 when paused)
    donut::app::DeviceManager* m_DeviceManager = nullptr;                       // +112
    nvrhi::AutoPtr<donut::engine::ShaderFactory> m_ShaderFactory;              // +120
    nvrhi::AutoPtr<donut::vfs::IFileSystem> m_RootFs;                          // +136
    std::filesystem::path m_MediaPath;                                          // +152

    std::shared_ptr<SpaceScene> m_Scene;                                        // +184
    nvrhi::AutoPtr<donut::render::InstancedOpaqueDrawStrategy> m_OpaqueDrawStrategy;   // +200
    nvrhi::AutoPtr<donut::render::TransparentDrawStrategy> m_TransparentDrawStrategy;  // +216
    std::shared_ptr<MeshletRenderResources> m_MeshletResources;                 // +232
    nvrhi::AutoPtr<MeshletDrawStrategy> m_GBufferRenderer;                     // +248 gbuffer_ps
    nvrhi::AutoPtr<MeshletDrawStrategy> m_DepthRenderer;                       // +264 no pixel shader (depth/shadows)
    nvrhi::AutoPtr<MeshletDrawStrategy> m_MaterialIdRenderer;                  // +280 material_id_ps
    nvrhi::AutoPtr<MeshletDrawStrategy> m_ForwardRenderer;                     // +296 forward_ps
    nvrhi::AutoPtr<MeshletDrawStrategy> m_ShipForwardRenderer;                 // +312 forward_ps (mode 3)
    std::shared_ptr<PipelineStatisticsQuery> m_PipelineStatsQuery;              // +328

    donut::app::BaseCamera* m_ActiveCamera = nullptr;                           // +336
    donut::app::FirstPersonCamera m_FirstPersonCamera;                          // +344
    // deviation: the binary stored a raw pointer (never deleted); owned here.
    std::unique_ptr<ThirdPersonCamera> m_ShipCamera;                            // +576
    std::vector<CameraPreset> m_CameraPresets;                                  // +584

    nvrhi::AutoPtr<donut::engine::PlanarView> m_View;                          // +608 (2018: PlanarView or StereoView)
    nvrhi::AutoPtr<donut::engine::PlanarView> m_ViewPrevious;                  // +624
    dm::float3 m_ViewOrigin = 0.f;                                              // +640
    dm::float3 m_ViewOriginPrevious = 0.f;                                      // +652
    nvrhi::ShaderHandle m_GBufferPixelShader;                                   // +664 gbuffer_ps.hlsl
    nvrhi::ShaderHandle m_DeferredLightingPixelShader;                          // +672 deferred_lighting_ps.hlsl
    nvrhi::BindingLayoutHandle m_GBufferBindingLayout;                          // +680 (depth, GBuffer0..2)
    nvrhi::BindingSetHandle m_GBufferBindingSet;                                // +688
    nvrhi::CommandListHandle m_CommandList;                                     // +696
    // +704 / +712 / +720 / +728: 2018 blit binding sets for LdrColor, HdrColor and ResolvedColor1/2
    // (CommonRenderPasses blit cache); donut's BlitTexture takes the texture directly.
    nvrhi::ShaderHandle m_AccumulationPixelShader;                              // +736 accumulation_ps.hlsl (created, never used)
    // +744..+760: three handles never written by the binary.
    std::unique_ptr<RenderTargets> m_RenderTargets;                             // +768
    std::shared_ptr<ForwardShadingPass2018> m_ForwardPass;                      // +776
    // +784: shared_ptr never written by the binary.
    std::shared_ptr<fx::EnvironmentMapPass> m_EnvironmentMapPass;               // +800 textured starfield
    std::shared_ptr<fx::RealStarFieldResources> m_RealStarFieldResources;       // +808
    std::shared_ptr<fx::RealStarFieldPass> m_RealStarFieldPass;                 // +824
    std::shared_ptr<ToneMappingPass2018> m_ToneMappingPass;                     // +832 (2018 framework pass)
    std::shared_ptr<DeferredLightingPass2018> m_DeferredLightingPass;           // +840
    std::shared_ptr<GBufferFillPass2018> m_GBufferPass;                         // +848
    nvrhi::AutoPtr<donut::render::SsaoPass> m_SsaoPass;                        // +856
    std::shared_ptr<LightProbeProcessingPass2018> m_LightProbePass;             // +864 (2018 framework pass)
    nvrhi::AutoPtr<donut::render::TemporalAntiAliasingPass> m_TemporalAntiAliasingPass; // +880
    std::shared_ptr<BloomPass2018> m_BloomPass;                                 // +888 (2018 framework pass)
    std::shared_ptr<HbaoPlusPass> m_HbaoPlus;                                   // +896
    std::shared_ptr<GBufferFillPass2018> m_MaterialIdPass;                      // +904 never created by the binary
    std::shared_ptr<donut::render::PixelReadbackPass> m_MaterialIdReadback;     // +912 never created by the binary
    std::shared_ptr<fx::FogPass> m_FogPass;                                     // +920
    std::shared_ptr<fx::ParticleSystem> m_Particles;                            // +928
    std::shared_ptr<fx::LensFlarePass> m_LensFlarePass;                         // +936
    std::shared_ptr<fx::HqBlitPass> m_HqBlitPass;                               // +944 "Upscale"
    std::shared_ptr<TemporalAAPass2018> m_TaaPass2018;                          // 2018 TAA resolve (+880, resolve half)
    std::shared_ptr<SharpenBlitPass2018> m_SharpenBlitPass;                     // 2018 CommonRenderPasses sharpen blit (+48)
    std::shared_ptr<fx::ShowCubemapPass> m_ShowCubemapPass;                     // +952 probe debug view
    std::shared_ptr<fx::ShieldPass> m_ShieldPass;                               // +960
    nvrhi::AutoPtr<donut::render::CascadedShadowMap> m_ShadowMap;              // +968
    std::shared_ptr<SceneDirectionalLight> m_SunLight;                          // +984
    std::vector<std::shared_ptr<SceneLight>> m_Lights;                          // +1000
    nvrhi::AutoPtr<donut::engine::LoadedTexture> m_SplashTexture;              // +1024 splash.jpg
    nvrhi::AutoPtr<donut::engine::LoadedTexture> m_SkyTexture;                 // +1040 SkyAndStars/...
    nvrhi::AutoPtr<donut::engine::LoadedTexture> m_RandomsTexture;             // +1056 randoms_texture.dds
    nvrhi::AutoPtr<donut::engine::LoadedTexture> m_ColorLutTexture;            // +1072 color LUT
    std::vector<nvrhi::AutoPtr<DemoLightProbe>> m_LightProbes;                 // +1088
    nvrhi::AutoPtr<donut::engine::LoadedTexture> m_LightProbeDiffuse;          // +1112 LightProbeDiffuse.dds
    nvrhi::AutoPtr<donut::engine::LoadedTexture> m_LightProbeSpecular;         // +1128 LightProbeSpecular.dds
    nvrhi::AutoPtr<donut::engine::LoadedTexture> m_EnvironmentBrdf;            // +1144 EnvironmentBrdf.dds
    std::shared_ptr<fx::PlanetSet> m_Planets;                                   // +1160
    std::shared_ptr<fx::SunDisk> m_SunDisk;                                     // +1168
    nvrhi::TextureHandle m_ViewDistanceMap;                                     // +1176
    int64_t m_ConstructionTimestamp = 0;                                        // +1184 ns, for "Loading time"
    int64_t m_FirstFrameTimestamp = 0;                                          // +1192 ns
    UIData* m_UI = nullptr;                                                     // +1200

    // Gamepad and keyboard ship controls. The keyboard parts are -1/0/+1 while a key is held; the
    // gamepad parts accumulate the axis events of one frame and are reset by Animate.
    std::array<bool, InputReplay::c_NumButtons> m_JoystickButtons = {};         // +1208
    int32_t m_KeyThrottle = 0;                                                  // +1224 W/S
    float m_JoystickThrottle = 0.f;                                             // +1228 triggers
    int32_t m_KeyYaw = 0;                                                       // +1232 A/D, Left/Right
    float m_JoystickYaw = 0.f;                                                  // +1236 left stick X
    int32_t m_KeyPitch = 0;                                                     // +1240 Up/Down
    float m_JoystickPitch = 0.f;                                                // +1244 left stick Y
    int32_t m_KeyRoll = 0;                                                      // +1248 Q/E
    float m_JoystickRoll = 0.f;                                                 // +1252 LB/RB, left stick X
    dm::int2 m_MousePosition = 0;                                               // +1256
    bool m_PickRequested = false;                                               // +1264 never set in the binary

    InputReplay m_Replay;                                                       // +1272
    Animation::Sequence m_OpeningSequence;                                      // +1320 "start"
    Animation::Sequence m_OpeningSequenceFast;                                  // +1400 "start_fast"
    Animation::Sequence* m_ActiveSequence = nullptr;                            // +1480
    float m_CollisionIntensity = 0.f;                                           // +1488 decays with exp(-dt / 0.1)
    dm::float3 m_CollisionNormal = 0.f;                                         // +1492 -> Shield

    // deviation: donut's CascadedShadowMap takes a donut DirectionalLight that lives in a scene graph; the
    // 2018 map took the free-standing sun. A one-node graph carries a donut light that mirrors the sun.
    nvrhi::AutoPtr<donut::engine::SceneGraph> m_ShadowLightGraph;
    nvrhi::AutoPtr<donut::engine::DirectionalLight> m_ShadowLight;
    nvrhi::AutoPtr<donut::engine::FramebufferFactory> m_ShadowFramebuffer;

public:
    // Developer option (deviation, not in the binary): "-screenshot <file.png> <frame>" saves the final image.
    void RequestScreenshot(const std::filesystem::path& path, uint32_t frame, uint32_t count = 1)
    { m_ScreenshotPath = path; m_ScreenshotFrame = frame; m_ScreenshotCount = count ? count : 1; }

private:
    std::filesystem::path m_ScreenshotPath;
    uint32_t m_ScreenshotFrame = 0;
    uint32_t m_ScreenshotCount = 1;     // developer option: consecutive frames saved as <name>_<frame>.png
    uint32_t m_RenderedFrames = 0;
    std::chrono::steady_clock::time_point m_PerfLogStart;  // -perfLog (developer option)
};
