// Original interactive OptiX scene expressed through the unified Slang renderer.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "VolumeViewer.h"
#include <gvdb/slang/GVDBPortableVbx.h>
#include <sample-utils/SlangSceneRT.h>
namespace gvdb::samples::interactive
{
struct Input
{
    slang::VbxVolume volume;
    sample::SceneMesh mesh;
    bool analytic = false;
};
struct Options
{
    sample::SceneVolumeMode shading = sample::SceneVolumeMode::Deep;
    slang::Float4 translation{44, 0, 16, 0};
    uint32_t maxSamples = 1024;
};
std::shared_ptr<Input> loadInput(const std::filesystem::path &volume, const std::filesystem::path &mesh);
std::vector<slang::Float4> referenceTransfer();
class InteractiveScene final : public sample::VolumeScene
{
    ethereal::slang::ComputeRuntime &m_runtime;
    std::shared_ptr<const Input> m_input;
    Options m_options;
    slang::SlangVolume m_volume;
    sample::SceneDescription m_scene;
    sample::SlangSceneRT m_renderer;
    dm::float3 m_cameraAngles{-20, 30, 0}, m_cameraTarget{0, 0, 0};
    dm::float3 m_lightAngles{45, 45, 0}, m_lightTarget{0, 0, 0};
    float m_cameraDistance = 400, m_lightDistance = 200;
    uint32_t m_frame = 0, m_samples = 0;
    bool m_advance = false;
    slang::RaycastParameters m_lastCamera{};
    void reset();
    void updateView(sample::VolumeViewOptions &);
    void applyTransform();

  public:
    InteractiveScene(ethereal::slang::ComputeRuntime &, std::shared_ptr<const Input>, Options = {});
    slang::SlangVolume &volume() override
    {
        return m_volume;
    }
    const slang::PackedVolume &packed() const override
    {
        return m_input->volume.packed;
    }
    std::vector<slang::Float4> transfer() const override
    {
        return m_scene.volumes[0].transfer;
    }
    void animate(float, sample::VolumeViewOptions &) override;
    slang::RaycastOutput renderCustom(const slang::RaycastParameters &, const sample::VolumeViewOptions &) override;
    bool key(int, sample::VolumeViewOptions &) override;
    bool acceptsKeyRepeat(int) const override;
    bool mouseDrag(int, float, float, int, sample::VolumeViewOptions &) override;
    bool mouseScroll(float, sample::VolumeViewOptions &) override;
    std::string describe() const override;
    void buildUI(sample::VolumeViewOptions &) override;
    void setShading(sample::SceneVolumeMode);
    void setTranslation(slang::Float4);
    void setMaxSamples(uint32_t);
    uint32_t frame() const
    {
        return m_frame;
    }
    uint32_t sampleCount() const
    {
        return m_samples;
    }
    uint32_t maxSamples() const
    {
        return m_options.maxSamples;
    }
    const sample::SceneDescription &description() const
    {
        return m_scene;
    }
    const slang::RaycastParameters &lastCamera() const
    {
        return m_lastCamera;
    }
};
void runSelfTests(ethereal::slang::ComputeRuntime &);
void verifyAssetCoverage(ethereal::slang::ComputeRuntime &, const InteractiveScene &);
} // namespace gvdb::samples::interactive
