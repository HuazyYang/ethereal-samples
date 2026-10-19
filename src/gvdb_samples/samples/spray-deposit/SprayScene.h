// Portable spray sample state and Inspector/input behavior.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "SprayRender.h"
#include "VolumeViewer.h"
namespace gvdb::samples::spray {
struct SprayOptions {
    std::filesystem::path asset;
    bool simulate = true, rays = true;
    Shading shading = Shading::Surface;
    WandStyle wand = WandStyle::Rotate;
};
class SprayScene final : public sample::VolumeScene {
    ethereal::slang::ComputeRuntime* m_runtime;
    SprayOptions m_options;
    std::unique_ptr<SprayVolume> m_volume;
    std::vector<slang::Float4> m_transfer;
    dm::float3 m_cameraAngles{45, 40, 0}, m_cameraTarget{200, 200, 200}, m_lightAngles{80, 50, 0},
        m_lightTarget{200, 200, 200};
    float m_cameraDistance = 1000, m_lightDistance = 800;
    void updateView(sample::VolumeViewOptions&);
    void changeShading(Shading);

 public:
    SprayScene(ethereal::slang::ComputeRuntime&, SprayOptions);
    portable::SnapshotHandle ownedSnapshot() const override { return m_volume->snapshot(); }
    bool topologyOverlay(std::vector<sample::VolumeOverlayLine>&) const override;
    std::vector<slang::Float4> transfer() const override { return m_transfer; }
    void animate(float, sample::VolumeViewOptions&) override;
    slang::RaycastOutput renderCustom(const slang::RaycastParameters&,
                                      const sample::VolumeViewOptions&) override;
    std::vector<sample::VolumeOverlayLine> overlayLines() const override;
    void buildUI(sample::VolumeViewOptions&) override;
    bool key(int, sample::VolumeViewOptions&) override;
    bool mouseDrag(int, float, float, int, sample::VolumeViewOptions&) override;
    bool mouseScroll(float, sample::VolumeViewOptions&) override;
    std::string describe() const override;
    const SprayVolume& simulation() const { return *m_volume; }
    Shading shading() const { return m_options.shading; }
};
}  // namespace gvdb::samples::spray
