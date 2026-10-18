// Slang point-cloud application state shared by headless and windowed rendering.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "PointCloudScene.h"
#include "PointCloudReconstruction.h"
#include "VolumeViewer.h"
#include <sample-utils/SlangSceneRT.h>
namespace gvdb::samples::pointcloud {
struct AppOptions {
    SceneDesc scene;
    std::filesystem::path assets;
    bool compute = false, keyAnimation = false, points = false;
    uint32_t captureSamples = 0;
};
class PointCloudAppScene final : public sample::VolumeScene {
    ethereal::slang::ComputeRuntime* m_runtime;
    AppOptions m_options;
    PointCloudReconstruction m_reconstruction;
    sample::SceneDescription m_scene;
#if ETHEREAL_POINT_CLOUD_RT
    std::unique_ptr<sample::SlangSceneRT> m_renderer;
#endif
    std::unique_ptr<slang::SlangVolume> m_emptyVolume;
    slang::PackedVolume m_emptyPacked;
    std::vector<slang::Float4> m_transfer;
    dm::float3 m_cameraAngles, m_cameraTarget, m_lightAngles, m_lightTarget;
    float m_cameraDistance, m_lightDistance;
    int m_frame = 0, m_polyFrame = 0;
    uint32_t m_samples = 0, m_maxSamples = 1, m_polyGeometry = UINT32_MAX;
    bool m_advance = false;
    slang::RaycastParameters m_lastCamera{};
    bool m_hasCamera = false;
    void loadPoints(int frame);
    sample::SceneMesh loadMesh(const std::filesystem::path&, float, dm::float3) const;
    sample::SceneVolume sceneVolume() const;
    void updateView(sample::VolumeViewOptions&);
    void resetSamples();

 public:
    PointCloudAppScene(ethereal::slang::ComputeRuntime&, AppOptions);
    slang::SlangVolume& volume() override;
    const slang::PackedVolume& packed() const override;
    std::vector<slang::Float4> transfer() const override { return m_transfer; }
    void animate(float, sample::VolumeViewOptions&) override;
    void buildUI(sample::VolumeViewOptions&) override;
    slang::RaycastOutput renderCustom(const slang::RaycastParameters&,
                                      const sample::VolumeViewOptions&) override;
    std::vector<sample::VolumeOverlayLine> overlayLines() const override;
    bool key(int, sample::VolumeViewOptions&) override;
    bool mouseDrag(int, float, float, int, sample::VolumeViewOptions&) override;
    bool mouseScroll(float, sample::VolumeViewOptions&) override;
    std::string describe() const override;
    uint32_t verifyVolumeContribution();
    int frame() const { return m_frame; }
    int polygonFrame() const { return m_polyFrame; }
    uint32_t sampleCount() const { return m_samples; }
    const PointCloudReconstruction& reconstruction() const { return m_reconstruction; }
    const sample::SceneDescription& sceneDescription() const { return m_scene; }
};
}  // namespace gvdb::samples::pointcloud
