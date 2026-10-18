// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <sample-utils/SlangSceneRT.h>

#include "PortableFluid.h"
#include "VolumeViewer.h"
namespace gvdb::samples::fluid {
struct AppOptions {
    std::filesystem::path assets;
    uint32_t particles = 1500000, captureSteps = 0;
    int shading = 1;
    bool compute = false, compact = false, simulate = true, color = true;
};
class FluidScene final : public sample::VolumeScene {
    ethereal::slang::ComputeRuntime* m_runtime;
    AppOptions m_options;
    std::unique_ptr<slang::SlangSphSimulation> m_simulation;
    FluidSurface m_surface;
    nvrhi::BufferHandle m_colors;
    std::vector<slang::Float4> m_transfer;
    sample::SceneDescription m_scene;
#if ETHEREAL_FLUID_RT
    std::unique_ptr<sample::SlangSceneRT> m_renderer;
#endif
    dm::float3 m_angles{50, 30, 0}, m_target{200, 100, 200}, m_lightAngles{20, 60, 0};
    float m_distance = 1200, m_lightDistance = 1000;
    bool m_showFluid = false, m_showGui = true, m_topology = false;
    uint32_t m_frame = 0, m_samples = 0;
    float m_fluidMs = 0, m_surfaceMs = 0;
    slang::RaycastParameters m_lastCamera{};
    bool m_hasCamera = false;
    void reset();
    void rebuild();
    void updateScene();
    void updateView(sample::VolumeViewOptions&);
    void resetSamples();
    void setShading(int);

   public:
    FluidScene(ethereal::slang::ComputeRuntime&, AppOptions);
    slang::SlangVolume& volume() override { return m_surface.volume(); }
    const slang::PackedVolume& packed() const override { return m_surface.packed(); }
    std::vector<slang::Float4> transfer() const override;
    nvrhi::IBuffer* colorAtlas() const override { return m_options.color ? m_surface.color() : nullptr; }
    void animate(float, sample::VolumeViewOptions&) override;
    slang::RaycastOutput renderCustom(const slang::RaycastParameters&,
                                      const sample::VolumeViewOptions&) override;
    std::vector<sample::VolumeOverlayLine> overlayLines() const override;
    bool key(int, sample::VolumeViewOptions&) override;
    bool mouseDrag(int, float, float, int, sample::VolumeViewOptions&) override;
    bool mouseScroll(float, sample::VolumeViewOptions&) override;
    void buildUI(sample::VolumeViewOptions&) override;
    std::string describe() const override;
    uint32_t frame() const { return m_frame; }
    uint32_t sampleCount() const { return m_samples; }
    uint32_t particleCount() const { return m_simulation->parameters().gridResolutionAndCount.w; }
    int shading() const { return m_options.shading; }
    const sample::SceneDescription& sceneDescription() const { return m_scene; }
    const FluidSurface& surface() const { return m_surface; }
    uint32_t verifyVolumeContribution();
};
void runFluidTopologyTests();
void runFluidTests(ethereal::slang::ComputeRuntime&, const std::filesystem::path& assets, bool rt);
}  // namespace gvdb::samples::fluid
