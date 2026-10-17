#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "graphics_bindings.h"

#include "beach_scene.h"
#include "example_app.h"

class BeachRenderer;
class BeachLighting;
class OceanRenderer;
class SkyRenderer;
class UIRenderer;
struct MainState;

struct SceneRenderTargets {
    nvrhi::TextureHandle opaqueHdr;
    nvrhi::TextureHandle compositeHdr;
    nvrhi::TextureHandle linearDepth;
    nvrhi::TextureHandle depthStencil;
    nvrhi::FramebufferHandle opaqueFramebuffer, compositeFramebuffer;
    BeachRenderTargetSize size;

    bool IsComplete() const {
        return opaqueHdr && compositeHdr && linearDepth && depthStencil &&
               opaqueFramebuffer && compositeFramebuffer && size.IsValid();
    }
};

struct ScenePassTimings {
    float opaqueSkyMilliseconds = 0.f;
    float waterMilliseconds = 0.f;
    float foamMilliseconds = 0.f;
    float postMilliseconds = 0.f;
};

class BeachScenePipeline final : public IExampleScenePipeline {
    NVRHI_INHERIT_INTERFACE_TABLE()
 public:
    BeachScenePipeline(donut::app::DeviceManager* manager, MainState* state, SkyRenderer* sky,
                       OceanRenderer* ocean, UIRenderer* ui);
    ~BeachScenePipeline() override;

    bool Init(uint32_t width, uint32_t height, uint32_t sampleCount) override;
    void Shutdown() override;

    void Render(nvrhi::IFramebuffer* framebuffer) override;
    bool ShouldAnimateUnfocused() override { return true; }
    bool ShouldRenderUnfocused() override { return true; }
    void Animate(float elapsedTimeSeconds) override;
    void BackBufferResizing() override;
    void BackBufferResized(uint32_t width, uint32_t height, uint32_t sampleCount) override;
    bool KeyboardCharInput(unsigned int character, int mods) override;
    bool KeyboardUpdate(int key, int scancode, int action, int mods) override;
    bool MousePosUpdate(double xpos, double ypos) override;
    bool MouseScrollUpdate(double xoffset, double yoffset) override;
    bool MouseButtonUpdate(int button, int action, int mods) override;

    const ScenePassTimings& GetTimings() const { return m_timings; }

 private:
    enum TimingStage : uint32_t { OpaqueSky, Water, Foam, Post, TimingStageCount };
    struct TimingFrame {
        std::array<nvrhi::TimerQueryHandle, TimingStageCount> queries;
        bool pending = false;
    };

    bool CreateSceneTargets(uint32_t width, uint32_t height, uint32_t sampleCount);
    bool CreatePostResources();
    bool EnsurePostFramebuffer(nvrhi::ITexture* renderTarget, uint32_t swapChainIndex);
    void ReleaseSceneTargets();
    void ClearFrameTargets();
    void CaptureOpaqueScene();
    void CaptureTestFrame(nvrhi::ITexture* renderTarget);
    void ToneMap(nvrhi::ITexture* renderTarget, uint32_t swapChainIndex,
                 nvrhi::ITimerQuery* timerQuery);
    TimingFrame* BeginTimingFrame();
    void ResolveTimingFrame(TimingFrame& frame);

    nvrhi::IDevice* m_device = nullptr;
    nvrhi::GraphicsAPI m_backend = nvrhi::GraphicsAPI::D3D11;
    MainState* m_state = nullptr;
    nvrhi::AutoPtr<BeachRenderer> m_ownedBeach;
    std::unique_ptr<BeachLighting> m_lighting;
    BeachRenderer* m_beach = nullptr;
    SkyRenderer* m_sky = nullptr;
    OceanRenderer* m_ocean = nullptr;
    UIRenderer* m_ui = nullptr;

    SceneRenderTargets m_targets;
    ScenePassTimings m_timings;
    nvrhi::CommandListHandle m_commands;
    nvrhi::ShaderHandle m_fullscreenVs;
    nvrhi::ShaderHandle m_toneMapPs;
    nvrhi::SamplerHandle m_linearClamp;
    waveworks::graphics::GraphicsBindingLayouts m_postLayout;
    waveworks::graphics::GraphicsBindingSets m_postBindings;
    nvrhi::GraphicsPipelineHandle m_postPipeline;
    std::vector<nvrhi::FramebufferHandle> m_postFramebuffers;
    nvrhi::StagingTextureHandle m_captureStaging;
    std::string m_capturePath;
    std::array<TimingFrame, 4> m_timingRing;
    uint64_t m_frameNumber = 0;
    uint64_t m_captureFrameNumber = 8;
    bool m_targetsUsed = false;
    bool m_captureAttempted = false;
    bool m_initialized = false;
};
