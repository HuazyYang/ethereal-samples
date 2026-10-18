// SPDX-License-Identifier: Apache-2.0
#include "FluidScene.h"

#include <donut/app/ApplicationBase.h>
#include <donut/core/log.h>
#include <imgui.h>
#include <sample-utils/SceneAssets.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "ComputeRuntime.h"
namespace gvdb::samples::fluid {
using slang::Float4;
namespace {
Float4 f4(dm::float3 v, float w = 0) { return {v.x, v.y, v.z, w}; }
struct Basis {
    dm::float3 right, up, back;
};
Basis basis(dm::float3 a) {
    const float yaw = dm::radians(a.x), pitch = dm::radians(a.y);
    Basis b;
    b.back = {std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)};
    b.right = dm::normalize(
        dm::cross(std::abs(b.back.y) > .999f ? dm::float3(0, 0, 1) : dm::float3(0, 1, 0), b.back));
    b.up = dm::normalize(dm::cross(b.back, b.right));
    return b;
}
float milliseconds(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t).count();
}
sample::SceneMaterial material() {
    sample::SceneMaterial m;
    m.lightWidth = 1.2f;
    m.shadowWidth = .1f;
    m.shadowBias = .5f;
    m.ambColor = {.05f, .05f, .05f};
    m.diffColor = {.7f, .7f, .7f};
    m.specColor = {1, 1, 1};
    m.specPower = 400;
    m.envColor = dm::float3::zero();
    m.reflWidth = .5f;
    m.reflBias = .5f;
    m.reflColor = {.4f, .4f, .4f};
    m.refrWidth = 0;
    m.refrColor = {.1f, .1f, .1f};
    m.refrIor = 1.1f;
    m.refrAmount = .5f;
    m.refrOffset = 50;
    m.refrBias = .5f;
    return m;
}
}  // namespace
FluidScene::FluidScene(ethereal::slang::ComputeRuntime& runtime, AppOptions options)
    : m_runtime(&runtime), m_options(std::move(options)), m_surface(runtime) {
    if (m_options.shading < 0 || m_options.shading > 4 || (m_options.compute && m_options.shading > 1))
        throw std::invalid_argument(
            "Compute fluid rendering supports off or levelset; other shading requires RT");
    if (m_options.compact) {
        m_target = {200, 98, 200};
        m_distance = 130;
    }
    m_transfer.resize(16384);
    for (uint32_t i = 0; i < m_transfer.size(); ++i) {
        float u = float(i) / 16384;
        m_transfer[i] = {u, u, u, .1f * u};
    }
    m_scene.materials.push_back(material());
    m_scene.meshes.push_back(sample::loadSceneObj(m_options.assets / "ground.obj"));
    if (!m_options.compute) {
#if ETHEREAL_FLUID_RT
        if (!runtime.device()->queryFeatureSupport(nvrhi::Feature::RayTracingPipeline))
            throw std::runtime_error("Fluid ray tracing unavailable; select --renderer compute explicitly");
        const char* target = runtime.device()->getGraphicsAPI() == nvrhi::GraphicsAPI::CUDA
                                 ? "optix"
                                 : donut::app::GetShaderTypeName(runtime.device()->getGraphicsAPI());
        m_renderer = std::make_unique<sample::SlangSceneRT>(
            runtime,
            donut::app::GetDirectoryWithExecutable() / "shaders/gvdb_slang" / target / "SlangSceneRT.bin");
        try {
            m_renderer->setEnvironment(sample::loadSceneEnvironment(m_options.assets / "sky.png"));
        } catch (const std::exception& e) {
            donut::log::warning("Fluid environment: %s; white environment", e.what());
        }
#else
        throw std::runtime_error("Fluid RT was not built; select --renderer compute explicitly");
#endif
    }
    reset();
}
void FluidScene::reset() {
    auto initial = makeInitialFluid(m_options.particles, m_options.compact);
    m_simulation =
        std::make_unique<slang::SlangSphSimulation>(*m_runtime, initial.parameters, initial.positions);
    m_colors = m_runtime->buffer(initial.colors.size() * 4, 4, initial.colors.data());
    m_frame = 0;
    rebuild();
    resetSamples();
}
void FluidScene::rebuild() {
    auto start = std::chrono::steady_clock::now();
    m_surface.reconstruct(m_simulation->positions(), particleCount(), m_colors);
    m_runtime->flush();
    m_runtime->device()->waitForIdle();
    m_surfaceMs = milliseconds(start);
    updateScene();
}
void FluidScene::updateScene() {
    sample::SceneVolume v;
    const auto& gpu = m_surface.volume();
    v.info = gpu.info();
    v.scalar = gpu.atlas();
    v.nodes = gpu.nodes();
    v.children = gpu.children();
    v.levels = gpu.levels();
    v.color = m_options.color ? m_surface.color() : nullptr;
    v.transfer = m_transfer;
    v.transferLastIndex = 16300;
    v.steps = {.25f, 16, .25f, .001f};
    v.extinct = {-1, 1.5f, 0, 0};
    v.threshold = {0, 3, -1, 3};
    v.cutoff = {.005f, .01f, 0, 0};
    v.mode = m_options.shading == 2   ? sample::SceneVolumeMode::Surface
             : m_options.shading == 3 ? sample::SceneVolumeMode::Deep
             : m_options.shading == 4 ? sample::SceneVolumeMode::EmptySkip
                                      : sample::SceneVolumeMode::LevelSet;
    m_scene.volumes = {std::move(v)};
    m_scene.instances.clear();
    if (m_options.shading) {
        sample::SceneInstance i;
        i.kind = sample::SceneInstance::Kind::Volume;
        m_scene.instances.push_back(i);
    }
    sample::SceneInstance ground;
    ground.kind = sample::SceneInstance::Kind::Mesh;
    // Legacy row-vector translation * rotation: translation rotates with the mesh.
    const float a = dm::radians(5.f), c = std::cos(a), s = std::sin(a);
    const float transform[] = {c, -s, 0, -10 * s, s, c, 0, 10 * c, 0, 0, 1, 0};
    std::copy(std::begin(transform), std::end(transform), ground.transform);
    m_scene.instances.push_back(ground);
#if ETHEREAL_FLUID_RT
    if (m_renderer) {
        m_renderer->setScene(m_scene);
        m_renderer->setRandomSeed(4732, m_frame);
    }
#endif
    resetSamples();
}
void FluidScene::resetSamples() {
    m_samples = 0;
#if ETHEREAL_FLUID_RT
    if (m_renderer) m_renderer->resetAccumulation();
#endif
}
void FluidScene::setShading(int value) {
    if (value == m_options.shading) return;
    const bool wasOff = m_options.shading == 0;
    m_options.shading = value;
    if (wasOff && value)
        rebuild();
    else
        updateScene();
}
std::vector<Float4> FluidScene::transfer() const {
    // The portable level-set renderer uses white when no particle color is present.
    return {{1, 1, 1, 1}, {1, 1, 1, 1}};
}
void FluidScene::updateView(sample::VolumeViewOptions& view) {
    const auto b = basis(m_angles), light = basis(m_lightAngles);
    view.explicitCamera = true;
    view.cameraEye = f4(m_target + b.back * m_distance);
    view.cameraForward = f4(-b.back);
    view.cameraUp = f4(b.up);
    view.horizontalFov = 50;
    view.cameraNear = 1;
    view.cameraFar = 20000;
    view.scalarBackground = 3;
    view.mode = sample::VolumeViewMode::Surface;
    view.sectionInset = false;
    view.step = .25f;
    view.isovalue = 0;
    view.valueMin = -1;
    view.valueMax = 3;
    view.background = {.8f, .8f, .8f, 1};
    view.lightDirection = f4(light.back * m_lightDistance);
    view.topology = m_topology && m_showGui;
    view.showUI = m_showGui;
#if ETHEREAL_FLUID_RT
    if (m_renderer)
        m_renderer->setPointLight(f4(dm::float3(200, 100, 200) + light.back * m_lightDistance, 1));
#endif
}
void FluidScene::animate(float, sample::VolumeViewOptions& view) {
    if (m_options.simulate && (!m_options.captureSteps || m_frame < m_options.captureSteps)) {
        auto start = std::chrono::steady_clock::now();
        m_simulation->step();
        m_runtime->flush();
        m_runtime->device()->waitForIdle();
        m_fluidMs = milliseconds(start);
        ++m_frame;
        if (m_options.shading)
            rebuild();
        else
            resetSamples();
    }
    updateView(view);
}
slang::RaycastOutput FluidScene::renderCustom(const slang::RaycastParameters& camera,
                                              const sample::VolumeViewOptions&) {
    m_lastCamera = camera;
    m_hasCamera = true;
#if ETHEREAL_FLUID_RT
    if (m_renderer) {
        // Dataset time advances even when reconstruction is disabled by Off.
        m_renderer->setRandomSeed(4732,m_frame);
        auto output = m_renderer->render(camera);
        m_samples = m_renderer->sampleCount();
        return output;
    }
#endif
    ++m_samples;
    if (!m_options.shading) {
        const size_t n = size_t(camera.image.x) * camera.image.y;
        std::vector<Float4> color(n, camera.background);
        std::vector<float> depth(n, 1e10f);
        return {m_runtime->buffer(n * 16, 16, color.data()), m_runtime->buffer(n * 4, 4, depth.data()),
                camera.image.x, camera.image.y};
    }
    return {};
}
std::vector<sample::VolumeOverlayLine> FluidScene::overlayLines() const {
    std::vector<sample::VolumeOverlayLine> lines;
    if (!m_showGui || !m_showFluid) return lines;
    const auto positions = m_simulation->readPositions(), velocities = m_simulation->readVelocities();
    lines.reserve((positions.size() + 7) / 8);
    for (size_t i = 0; i < positions.size(); i += 8) {
        const auto p = positions[i], v = velocities[i];
        uint32_t c;
        std::memcpy(&c, &p.w, 4);
        if (p.x == -1000 && p.y == -1000 && p.z == -1000) continue;
        lines.push_back(
            {{p.x, p.y, p.z, 0},
             {p.x + v.x + .1f, p.y + v.y + .1f, p.z + v.z + .1f, 0},
             {float(c & 255) / 255, float((c >> 8) & 255) / 255, float((c >> 16) & 255) / 255, 1}});
    }
    return lines;
}
bool FluidScene::key(int key, sample::VolumeViewOptions& view) {
    switch (key) {
        case ' ':
            m_options.simulate = !m_options.simulate;
            break;
        case '1':
            m_topology = !m_topology;
            break;
        case '2':
            m_showFluid = !m_showFluid;
            break;
        case '3':
            setShading((m_options.shading + 1) % (m_options.compute ? 2 : 5));
            break;
        case '4':
            m_options.color = !m_options.color;
            updateScene();
            break;
        case 'R':
        case 'r':
            reset();
            break;
        case '`':
            m_showGui = !m_showGui;
            break;
        case 'V':
        case 'v':
        case 'I':
        case 'i':
            return true;
        default:
            return false;
    }
    updateView(view);
    return true;
}
bool FluidScene::mouseDrag(int button, float dx, float dy, int modifiers, sample::VolumeViewOptions& view) {
    auto& angles = (modifiers & 1) ? m_lightAngles : m_angles;
    if (button == 0) {
        angles.x += dx * .2f;
        angles.y -= dy * .2f;
    } else if (button == 2) {
        const auto b = basis(m_angles);
        m_target += b.right * (dx * m_distance / 1000) + b.up * (-dy * m_distance / 1000);
    } else if (button == 1) {
        auto& distance = (modifiers & 1) ? m_lightDistance : m_distance;
        distance = std::max(1.f, distance - dy);
    } else
        return false;
    resetSamples();
    updateView(view);
    return true;
}
bool FluidScene::mouseScroll(float amount, sample::VolumeViewOptions& view) {
    m_distance = std::max(1.f, m_distance - amount * 20);
    resetSamples();
    updateView(view);
    return true;
}
void FluidScene::buildUI(sample::VolumeViewOptions& view) {
    ImGui::Checkbox("Simulate", &m_options.simulate);
    ImGui::Checkbox("Topology", &m_topology);
    ImGui::Checkbox("Fluid", &m_showFluid);
    if (ImGui::Checkbox("Color", &m_options.color)) updateScene();
    int style = m_options.shading;
    if (ImGui::Combo("Shading", &style,
                     m_options.compute
                         ? "Off\0Level set\0"
                         : "Off\0Level set\0Trilinear surface\0Volume (deep)\0Empty skipping\0"))
        setShading(style);
    if (ImGui::Button("Reset")) reset();
    ImGui::TextUnformatted(describe().c_str());
    ImGui::TextUnformatted("Space simulate; 1 topology; 2 fluid; 3 shading; 4 color; R reset; ` UI");
    updateView(view);
}
std::string FluidScene::describe() const {
    char b[256];
    std::snprintf(
        b, sizeof(b),
        "Fluid %s: %u particles, frame %u sample %u, t %.3f; %u bricks; fluid %.1f ms surface %.1f ms",
        m_options.compute ? "compute" : "Slang RT", particleCount(), m_frame, m_samples,
        m_simulation->parameters().integration.x, m_surface.packed().levels[0].nodes.z, m_fluidMs,
        m_surfaceMs);
    return b;
}
uint32_t FluidScene::verifyVolumeContribution() {
#if ETHEREAL_FLUID_RT
    if (!m_renderer || !m_hasCamera || !m_options.shading)
        throw std::logic_error("Volume verification requires a rendered fluid RT scene");
    const char* target = m_runtime->device()->getGraphicsAPI() == nvrhi::GraphicsAPI::CUDA
                             ? "optix"
                             : donut::app::GetShaderTypeName(m_runtime->device()->getGraphicsAPI());
    sample::SlangSceneRT probe(*m_runtime, donut::app::GetDirectoryWithExecutable() / "shaders/gvdb_slang" /
                                               target / "SlangSceneRT.bin");
    probe.setScene(m_scene);
    probe.setRandomSeed(4732, m_frame);
    auto full = probe.render(m_lastCamera, 0);
    std::vector<float> a(size_t(full.width) * full.height), b(a.size());
    m_runtime->read(full.depth, a.data(), a.size() * 4);
    auto ground = m_scene;
    ground.instances.erase(ground.instances.begin());
    probe.setScene(ground);
    auto output = probe.render(m_lastCamera, 0);
    m_runtime->read(output.depth, b.data(), b.size() * 4);
    uint32_t hits = 0;
    for (size_t i = 0; i < a.size(); ++i)
        hits += std::isfinite(a[i]) && a[i] >= 0 && a[i] < m_lastCamera.march.z && a[i] + .01f < b[i];
    if (!hits) throw std::runtime_error("Fluid image has no verified fluid-volume contribution");
    return hits;
#else
    throw std::runtime_error("Fluid RT was not built");
#endif
}
}  // namespace gvdb::samples::fluid
