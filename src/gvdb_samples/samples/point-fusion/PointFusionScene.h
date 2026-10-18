// Interactive procedural-city scan/fusion scene for the shared volume viewer.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "PointFusionCity.h"
#include "PointFusionVolume.h"
#include "VolumeViewer.h"
#include "OwnedVolumeSurface.h"
namespace gvdb::samples
{
struct PointFusionSceneOptions
{
    uint32_t scanWidth = 240, scanHeight = 180;
    float speed = 0;
    bool firstPerson = true, color = false, points = false, objects = false;
};
class PointFusionScene final : public sample::VolumeScene
{
    PointFusionCity m_city;
    PointFusionScanner m_scanner;
    PointFusionVolume m_fusion;
    ethereal::slang::ComputeRuntime *m_runtime;
    sample::OwnedVolumeSurface m_surface;
    mutable portable::TopologyRevision m_overlayRevision;
    mutable std::vector<sample::VolumeOverlayLine> m_topologyLines;
    PointFusionSceneOptions m_options;
    slang::Float4 m_eye, m_orbitTarget;
    float m_yaw = 180, m_pitch = 0, m_orbitYaw = 190, m_orbitPitch = 30, m_orbitDistance = 4200;
    bool m_scan = true;
    uint32_t m_lastHits = 0;
    void camera(sample::VolumeViewOptions &) const;

  public:
    PointFusionScene(ethereal::slang::ComputeRuntime &, PointFusionSceneOptions = {});
    slang::VolumeInfo volumeInfo() override
    {
        return m_fusion.snapshot()->getDesc().volume;
    }
    slang::RaycastOutput renderCustom(const slang::RaycastParameters &, const sample::VolumeViewOptions &) override;
    bool topologyOverlay(std::vector<sample::VolumeOverlayLine> &) const override;
    std::vector<slang::Float4> transfer() const override
    {
        return {{1, 1, 1, 1}, {1, 1, 1, 1}};
    }
    void animate(float, sample::VolumeViewOptions &) override;
    bool key(int, sample::VolumeViewOptions &) override;
    bool mouseDrag(int, float, float, int, sample::VolumeViewOptions &) override;
    bool mouseScroll(float, sample::VolumeViewOptions &) override;
    std::vector<sample::VolumeOverlayLine> overlayLines() const override;
    std::vector<sample::VolumeOverlayPoint> overlayPoints() const override;
    std::string describe() const override;
    const PointFusionVolume &fusion() const
    {
        return m_fusion;
    }
    slang::Float4 scannerEye() const
    {
        return m_eye;
    }
    float scannerYaw() const
    {
        return m_yaw;
    }
    float scannerPitch() const
    {
        return m_pitch;
    }
    float speed() const
    {
        return m_options.speed;
    }
    bool scanning() const
    {
        return m_scan;
    }
    bool firstPerson() const
    {
        return m_options.firstPerson;
    }
};
void runPointFusionInputTests(ethereal::slang::ComputeRuntime &);
} // namespace gvdb::samples
