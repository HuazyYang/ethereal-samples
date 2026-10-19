// SPDX-License-Identifier: Apache-2.0
#include "SprayScene.h"
#include <sample-utils/SceneAssets.h>
#include <imgui.h>
#include <cmath>
#include <cstdio>
#include <cstring>
namespace gvdb::samples::spray {
using slang::Float4;
namespace {
Float4 f4(dm::float3 v, float w = 0) { return {v.x, v.y, v.z, w}; }
struct Basis {
    dm::float3 right, up, back;
};
Basis basis(dm::float3 angle) {
    const float x = dm::radians(angle.x), y = dm::radians(angle.y);
    Basis b;
    b.back = {std::cos(y) * std::sin(x), std::sin(y), std::cos(y) * std::cos(x)};
    b.right = dm::normalize(
        dm::cross(std::abs(b.back.y) > .999f ? dm::float3(0, 0, 1) : dm::float3(0, 1, 0), b.back));
    b.up = dm::normalize(dm::cross(b.back, b.right));
    return b;
}
}  // namespace
SprayScene::SprayScene(ethereal::slang::ComputeRuntime& runtime, SprayOptions options)
    : m_runtime(&runtime), m_options(std::move(options)) {
    m_volume = voxelizeSprayPart(runtime, sample::loadSceneObj(m_options.asset));
    changeShading(m_options.shading);
}
void SprayScene::changeShading(Shading mode) {
    m_options.shading = mode;
    m_transfer = sprayTransfer(mode);
}
void SprayScene::updateView(sample::VolumeViewOptions& view) {
    const auto camera = basis(m_cameraAngles);
    view.explicitCamera = true;
    view.mode = sample::VolumeViewMode::Volume;
    view.sectionInset = false;
    view.cameraEye = f4(m_cameraTarget + camera.back * m_cameraDistance);
    view.cameraForward = f4(-camera.back);
    view.cameraUp = f4(camera.up);
    view.cameraNear = 1;
    view.cameraFar = 10000;
    view.horizontalFov = 50;
    view.isovalue = .3f;
    view.step = .25f;
    view.valueMin = 0;
    view.valueMax = 1;
    view.lightDirection = f4(m_lightTarget + basis(m_lightAngles).back * m_lightDistance - m_cameraTarget);
}
void SprayScene::animate(float, sample::VolumeViewOptions& view) {
    updateView(view);
    if (m_options.simulate)
        m_volume->step(m_options.wand);
}
slang::RaycastOutput SprayScene::renderCustom(const slang::RaycastParameters& camera,
                                              const sample::VolumeViewOptions&) {
    auto parameters = camera;
    parameters.light = f4(m_lightTarget + basis(m_lightAngles).back * m_lightDistance, .1f);
    return renderSpray(*m_runtime, *m_volume, parameters, m_options.shading, m_transfer);
}
std::vector<sample::VolumeOverlayLine> SprayScene::overlayLines() const {
    std::vector<sample::VolumeOverlayLine> result;
    if (!m_options.rays || !m_options.simulate)
        return result;
    const auto& rays = m_volume->rays();
    for (size_t i = 0; i < rays.hits.size(); ++i) {
        if (rays.hits[i].w <= 0)
            continue;
        const auto color = rays.colors[i];
        result.push_back({
            rays.origins[i],
            rays.hits[i],
            {float(color & 255) / 255, float((color >> 8) & 255) / 255, float((color >> 16) & 255) / 255,
                      float(color >> 24) / 255}
        });
    }
    return result;
}
void SprayScene::buildUI(sample::VolumeViewOptions& view) {
    ImGui::Checkbox("Simulate", &m_options.simulate);
    ImGui::Checkbox("Topology", &view.topology);
    ImGui::Checkbox("Rays", &m_options.rays);
    int shading = int(m_options.shading), wand = int(m_options.wand);
    if (ImGui::Combo("Shading", &shading, "Off\0Voxel\0Surface\0Section\0Volume\0\0"))
        changeShading(Shading(shading));
    if (ImGui::Combo("Wand Style", &wand, "Rotate\0Sweep\0Wave\0\0"))
        m_options.wand = WandStyle(wand);
}
bool SprayScene::key(int key, sample::VolumeViewOptions& view) {
    switch (key) {
        case '`':
            view.showUI = !view.showUI;
            return true;
        case '1':
        case ' ':
            m_options.simulate = !m_options.simulate;
            return true;
        case '2':
            view.topology = !view.topology;
            return true;
        case '3':
            m_options.rays = !m_options.rays;
            return true;
        case '4':
            changeShading(Shading((uint32_t(m_options.shading) + 1) % 5));
            return true;
        case '5':
            m_options.wand = WandStyle((uint32_t(m_options.wand) + 1) % 3);
            return true;
        default:
            return key == 'V' || key == 'v' || key == 'I' || key == 'i';
    }
}
bool SprayScene::mouseDrag(int button, float dx, float dy, int modifiers, sample::VolumeViewOptions& view) {
    auto& angles = (modifiers & 1) ? m_lightAngles : m_cameraAngles;
    auto& distance = (modifiers & 1) ? m_lightDistance : m_cameraDistance;
    if (button == 0) {
        angles.x += dx * .2f;
        angles.y -= dy * .2f;
    } else if (button == 2) {
        const auto b = basis(m_cameraAngles);
        m_cameraTarget += b.right * (dx * m_cameraDistance / 1000) + b.up * (-dy * m_cameraDistance / 1000);
    } else if (button == 1)
        distance -= dy;
    else
        return false;
    updateView(view);
    return true;
}
bool SprayScene::mouseScroll(float amount, sample::VolumeViewOptions& view) {
    m_cameraDistance -= amount * 20;
    updateView(view);
    return true;
}
bool SprayScene::topologyOverlay(std::vector<gvdb::sample::VolumeOverlayLine>& output) const {
    const auto& host = m_volume->topology();
    const gvdb::slang::Float4 colors[] = {
        {.1f, 1,   .2f, 1},
        {1,   .7f, .1f, 1},
        {.1f, .6f, 1,   1},
        {1,   .2f, .8f, 1}
    };
    output.clear();
    for (uint32_t level = 0; level < host.getConfiguration().levelCount; ++level) {
        const auto pool = host.getPool(0, level);
        if (pool.count && (!pool.data || pool.stride != sizeof(gvdb::portable::Node)))
            throw std::logic_error("Spray node pool");
        if (pool.count > (1000000 - output.size()) / 12)
            throw std::invalid_argument("Spray topology overlay too large");
        for (uint64_t i = 0; i < pool.count; ++i) {
            gvdb::portable::Node node;
            std::memcpy(&node, static_cast<const uint8_t*>(pool.data) + i * pool.stride, sizeof(node));
            const float w = float(host.getRange(int32_t(level)));
            const auto vertex = [&](uint32_t c) {
                return gvdb::slang::Float4{float(node.position.x) + (c & 1 ? w : 0),
                                           float(node.position.y) + (c & 2 ? w : 0),
                                           float(node.position.z) + (c & 4 ? w : 0), 0};
            };
            for (uint32_t c = 0; c < 8; ++c)
                for (uint32_t axis = 0; axis < 3; ++axis)
                    if (!(c & (1u << axis)))
                        output.push_back({vertex(c), vertex(c | (1u << axis)), colors[level % 4]});
        }
    }
    return true;
}

std::string SprayScene::describe() const {
    char text[180];
    std::snprintf(text, sizeof(text), "Spray step%u hits%u bricks%u shading%u wand%u", m_volume->time(),
                  m_volume->hitCount(), uint32_t(m_volume->topology().getPool(0, 0).count),
                  uint32_t(m_options.shading), uint32_t(m_options.wand));
    return text;
}
}  // namespace gvdb::samples::spray
