// SPDX-License-Identifier: Apache-2.0
#include "PointFusionScene.h"
#include <gvdb/slang/GVDBTopologyBuilder.h>
#include <donut/core/math/math.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
namespace gvdb::samples {
namespace {
namespace dm = donut::math;
using slang::Float4;
Float4 f4(dm::float3 p, float w = 0) { return {p.x, p.y, p.z, w}; }
dm::float3 f3(Float4 p) { return {p.x, p.y, p.z}; }
struct Basis {
    dm::float3 back, right, up;
};
Basis basis(float yawDegrees, float pitchDegrees) {
    const float yaw = dm::radians(yawDegrees), pitch = dm::radians(pitchDegrees);
    Basis b;
    b.back = {std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)};
    const dm::float3 up = std::abs(b.back.y) > .999f ? dm::float3(0, 0, 1) : dm::float3(0, 1, 0);
    b.right = dm::normalize(dm::cross(up, b.back));
    b.up = dm::normalize(dm::cross(b.back, b.right));
    return b;
}
Float4 color(uint32_t rgba) {
    return {float(rgba & 255) / 255, float((rgba >> 8) & 255) / 255, float((rgba >> 16) & 255) / 255,
            float(rgba >> 24) / 255};
}
}  // namespace
PointFusionScene::PointFusionScene(ethereal::slang::ComputeRuntime& runtime, PointFusionSceneOptions options)
    : m_city(makePointFusionCity()),
      m_scanner(runtime, options.scanWidth, options.scanHeight, m_city.boxes),
      m_fusion(runtime, {1, 2, options.color}),
      m_options(options),
      m_eye(m_city.initialEye),
      m_orbitTarget(m_city.initialEye) {
    if (!std::isfinite(options.speed) || options.speed < 0)
        throw std::invalid_argument("Scanner speed must be finite and nonnegative");
    slang::TopologyBuildOptions topology;
    topology.logDimensions = {3, 3, 3, 3, 5};
    std::string error;
    if (!slang::buildTopology(
            {
                {0, 0, 0, 0}
    },
            topology, m_emptyPacked, error))
        throw std::runtime_error(error);
    const auto resolution = m_emptyPacked.volume.atlasResolutionAndApron;
    std::vector<float> far(size_t(resolution.x) * resolution.y * resolution.z, 3);
    m_emptyVolume = std::make_unique<slang::SlangVolume>(runtime, m_emptyPacked, far.data(), far.size());
    // The GPU fallback is uniform far distance, not actual scene topology.
    // Keep it out of the topology overlay while the scene has no valid scan.
    m_emptyPacked = {};
}
void PointFusionScene::camera(sample::VolumeViewOptions& view) const {
    const auto b =
        basis(m_options.firstPerson ? m_yaw : m_orbitYaw, m_options.firstPerson ? m_pitch : m_orbitPitch);
    view.explicitCamera = true;
    view.cameraEye = m_options.firstPerson ? m_eye : f4(f3(m_orbitTarget) + b.back * m_orbitDistance);
    view.cameraForward = f4(-b.back);
    view.cameraUp = f4(b.up);
    view.horizontalFov = 65;
    view.cameraNear = 1;
    view.cameraFar = 100000;
    view.scalarBackground = 3;
}
void PointFusionScene::animate(float, sample::VolumeViewOptions& view) {
    m_eye = f4(f3(m_eye) - basis(m_yaw, m_pitch).back * m_options.speed);
    if (m_scan) {
        const auto frame = m_scanner.scan(
            cityScanParameters(m_city, m_eye, m_yaw, m_pitch, m_options.scanWidth, m_options.scanHeight));
        m_lastHits = frame.count;
        m_fusion.fuse(frame);
    }
    camera(view);
}
bool PointFusionScene::key(int key, sample::VolumeViewOptions& view) {
    switch (key) {
        case 'V':
        case 'v':
        case 'I':
        case 'i':
            view.mode = sample::VolumeViewMode::Surface;
            view.sectionInset = false;
            return true;
        case '1':
            m_options.points = !m_options.points;
            return true;
        case '2':
            view.topology = !view.topology;
            return true;
        case ' ':
            m_scan = !m_scan;
            if (!m_scan)
                m_options.speed = 0;
            return true;
        case 'F':
        case 'f':
            if (m_options.firstPerson) {
                m_orbitTarget = m_eye;
                m_orbitYaw = 190;
                m_orbitPitch = 30;
            }
            m_options.firstPerson = !m_options.firstPerson;
            camera(view);
            return true;
        case 'O':
        case 'o':
            m_options.objects = !m_options.objects;
            return true;
        case 'C':
        case 'c':
            m_options.color = !m_options.color;
            m_fusion.reset(m_options.color);
            m_lastHits = 0;
            return true;
        case 'R':
        case 'r':
            m_fusion.reset(m_options.color);
            m_lastHits = 0;
            return true;
        default:
            return false;
    }
}
bool PointFusionScene::mouseDrag(int button, float dx, float dy, int modifiers,
                                 sample::VolumeViewOptions& view) {
    const bool car = m_options.firstPerson || (modifiers & 1) != 0;  // GLFW_MOD_SHIFT
    if (button == 0) {
        if (car) {
            m_yaw += dx * .2f;
            m_pitch -= dy * .2f;
        } else {
            m_orbitYaw += dx * .2f;
            m_orbitPitch -= dy * .2f;
        }
    } else if (button == 2) {
        if (car) {
            m_eye.y = std::clamp(m_eye.y + dy, 0.f, 500.f);
            m_options.speed = 0;
        } else {
            const auto b = basis(m_orbitYaw, m_orbitPitch);
            m_orbitTarget = f4(f3(m_orbitTarget) + (b.right * dx - b.up * dy) * (m_orbitDistance / 1000));
        }
    } else if (button == 1) {
        if (car)
            m_options.speed = std::max(0.f, m_options.speed + dy * .1f);
        else
            m_orbitDistance = std::max(.001f, m_orbitDistance - dy);
    } else
        return false;
    camera(view);
    return true;
}
bool PointFusionScene::mouseScroll(float delta, sample::VolumeViewOptions& view) {
    if (m_options.firstPerson)
        m_options.speed = std::max(0.f, m_options.speed + delta);
    else
        m_orbitDistance = std::max(.001f, m_orbitDistance * std::exp(-delta * .1f));
    camera(view);
    return true;
}
std::vector<sample::VolumeOverlayLine> PointFusionScene::overlayLines() const {
    std::vector<sample::VolumeOverlayLine> lines;
    if (m_options.objects) {
        lines.reserve(m_city.boxes.size() * 12 + 9);
        for (const auto& box : m_city.boxes) {
            auto vertex = [&](unsigned corner) {
                return Float4{corner & 1 ? box.maximum.x : box.minimum.x,
                              corner & 2 ? box.maximum.y : box.minimum.y,
                              corner & 4 ? box.maximum.z : box.minimum.z, 0};
            };
            for (unsigned corner = 0; corner < 8; ++corner)
                for (unsigned axis = 0; axis < 3; ++axis) {
                    const unsigned next = corner ^ (1u << axis);
                    if (corner < next)
                        lines.push_back({vertex(corner), vertex(next), color(box.color.x)});
                }
        }
    }
    if (!m_options.firstPerson) {
        lines.push_back({
            {m_eye.x, 0,   m_eye.z, 0},
            m_eye, {.5f,     .5f, .5f,     1}
        });
        const auto p =
            cityScanParameters(m_city, m_eye, m_yaw, m_pitch, m_options.scanWidth, m_options.scanHeight);
        Float4 corner[4];
        for (unsigned i = 0; i < 4; ++i) {
            const auto d =
                f3(p.direction00) + f3(p.directionDx) * float(i & 1) + f3(p.directionDy) * float((i >> 1) & 1);
            corner[i] = f4(f3(m_eye) + d * 300.f);
            lines.push_back({
                m_eye, corner[i], {1, 1, 0, 1}
            });
        }
        const unsigned edge[4][2] = {
            {0, 1},
            {1, 3},
            {3, 2},
            {2, 0}
        };
        for (const auto& e : edge)
            lines.push_back({
                corner[e[0]], corner[e[1]], {1, 1, 0, 1}
            });
    }
    return lines;
}
std::vector<sample::VolumeOverlayPoint> PointFusionScene::overlayPoints() const {
    std::vector<sample::VolumeOverlayPoint> result;
    if (!m_options.points || !m_scan)
        return result;
    const auto& points = m_fusion.lastPositions();
    const auto& colors = m_fusion.lastColors();
    result.reserve(points.size());
    for (size_t i = 0; i < points.size(); ++i)
        result.push_back({points[i], color(colors[i])});
    return result;
}
std::string PointFusionScene::describe() const {
    return std::to_string(m_fusion.frames()) + " scans; " + std::to_string(m_lastHits) + " hits; " +
           std::to_string(m_fusion.hasVolume() ? m_fusion.packed().levels[0].nodes.z : 0) + " bricks; speed " +
           std::to_string(m_options.speed) + (m_options.firstPerson ? " FPV" : " orbit") +
           (m_scan ? " scanning" : " paused") + "; 1 points, 2 topology, Space scan, F camera, O city, C color";
}
void runPointFusionInputTests(ethereal::slang::ComputeRuntime& runtime) {
    PointFusionSceneOptions o;
    o.scanWidth = 4;
    o.scanHeight = 3;
    PointFusionScene scene(runtime, o);
    sample::VolumeViewOptions view;
    auto require = [](bool valid, const char* message) {
        if (!valid)
            throw std::runtime_error(message);
    };
    require(!scene.fusion().hasVolume() && scene.packed().nodes.empty(),
            "Fresh point-fusion scene must report empty data and topology");
    scene.key(' ', view);
    scene.mouseDrag(0, 10, -5, 0, view);
    require(scene.scannerYaw() == 182 && scene.scannerPitch() == 1,
            "FPV left drag did not change scanner angles");
    scene.mouseDrag(2, 0, 1000, 0, view);
    require(scene.scannerEye().y == 500 && scene.speed() == 0, "FPV middle drag height clamp failed");
    scene.mouseDrag(1, 0, 30, 0, view);
    require(scene.speed() == 3, "FPV right drag speed failed");
    scene.key('F', view);
    const auto eye = scene.scannerEye();
    scene.mouseDrag(0, 10, 0, 0, view);
    require(scene.scannerEye().x == eye.x && scene.scannerYaw() == 182, "Orbit drag moved scanner");
    scene.mouseDrag(0, 10, 0, 1, view);
    require(scene.scannerYaw() == 184, "Shift orbit drag did not drive scanner");
    scene.key('1', view);
    scene.key('2', view);
    require(view.topology && scene.overlayPoints().empty(), "Scene-first points/topology input failed");
    view.mode = sample::VolumeViewMode::Volume;
    view.sectionInset = true;
    scene.key('V', view);
    scene.key('I', view);
    require(view.mode == sample::VolumeViewMode::Surface && !view.sectionInset,
            "Point fusion accepted an unsupported generic viewer mode");
    scene.key('C', view);
    require(scene.fusion().useColor() && !scene.fusion().hasVolume(),
            "Color toggle did not reset empty volume");
    scene.mouseDrag(1, 0, -100, 1, view);
    require(scene.speed() == 0, "Scanner negative speed was not clamped");
    scene.animate(0, view);
    require(!scene.fusion().hasVolume() && view.explicitCamera && view.scalarBackground == 3 &&
                view.horizontalFov == 65,
            "Paused scan or explicit camera contract failed");
    require(scene.overlayLines().size() == 9, "Scanner orbit frustum overlay is incomplete");
    std::puts(
        "PASS point-fusion input: FPV/orbit/Shift precedence, height/speed clamps, pause, color reset, "
        "topology and scanner frustum");
}
}  // namespace gvdb::samples
