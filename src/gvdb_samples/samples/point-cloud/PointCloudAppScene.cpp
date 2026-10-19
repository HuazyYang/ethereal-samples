// SPDX-License-Identifier: Apache-2.0
#include "PointCloudAppScene.h"
#include <imgui.h>
#include <sample-utils/ObjLoader.h>
#include <cstring>
#include <donut/app/ApplicationBase.h>
#include <donut/core/log.h>
#include <stb_image.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
namespace gvdb::samples::pointcloud {
using slang::Float4;
namespace {
Float4 f4(dm::float3 p, float w = 0) { return {p.x, p.y, p.z, w}; }
struct Basis {
    dm::float3 right, up, back;
};
Basis basis(dm::float3 angles) {
    const float yaw = dm::radians(angles.x), pitch = dm::radians(angles.y);
    Basis b;
    b.back = {std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)};
    const dm::float3 worldUp = std::abs(b.back.y) > .999f ? dm::float3(0, 0, 1) : dm::float3(0, 1, 0);
    b.right = dm::normalize(dm::cross(worldUp, b.back));
    b.up = dm::normalize(dm::cross(b.back, b.right));
    return b;
}
int advanceFrame(int frame, int step) {
    const int64_t next = int64_t(frame) + step;
    if (next < INT32_MIN || next > INT32_MAX)
        throw std::overflow_error("Point-cloud time-series frame overflow");
    return int(next);
}
std::vector<Float4> pointTransfer() {
    std::vector<Float4> table(16384);
    for (uint32_t i = 0; i < 8192; ++i) {
        const float u = float(i) / 8192;
        table[i] = {0, u, 1 - u, .1f * u};
        table[i + 8192] = {u, 1 - u, 0, .1f};
    }
    return table;
}
}  // namespace
PointCloudAppScene::PointCloudAppScene(ethereal::slang::ComputeRuntime& runtime, AppOptions options)
    : m_runtime(&runtime),
      m_options(std::move(options)),
      m_reconstruction(runtime),
      m_transfer(pointTransfer()) {
    if (m_options.compute)
        m_surface = std::make_unique<sample::OwnedVolumeSurface>(
            runtime.device(), donut::app::GetDirectoryWithExecutable() / "shaders/gvdb_owned" /
                                  donut::app::GetShaderTypeName(runtime.device()->getGraphicsAPI()) /
                                  "OwnedVolumeSurface_renderSurface.bin");
    const auto& s = m_options.scene;
    if (!std::isfinite(s.renderScale) || s.renderScale <= 0)
        throw std::invalid_argument("Point-cloud scale must be finite and positive");
    if (!m_options.compute) {
#if ETHEREAL_POINT_CLOUD_RT
        if (!runtime.device()->queryFeatureSupport(nvrhi::Feature::RayTracingPipeline))
            throw std::runtime_error(
                "Point-cloud ray tracing is unavailable; select --renderer compute explicitly");
        const char* target = runtime.device()->getGraphicsAPI() == nvrhi::GraphicsAPI::CUDA
                                 ? "optix"
                                 : donut::app::GetShaderTypeName(runtime.device()->getGraphicsAPI());
        m_renderer = std::make_unique<sample::SlangSceneRT>(
            runtime,
            donut::app::GetDirectoryWithExecutable() / "shaders/gvdb_slang" / target / "SlangSceneRT.bin");
#else
        throw std::runtime_error("Point-cloud ray tracing was not built; select --renderer compute explicitly");
#endif
    }
    m_frame = s.frame < 0 ? 0 : s.frame;
    m_polyFrame = s.polyFrame;
    m_maxSamples = uint32_t(std::max(1, s.maxSamples));
    if (m_options.captureSamples) {
        if (m_options.captureSamples == UINT32_MAX)
            throw std::invalid_argument("Capture sample count exceeds supported range");
        m_maxSamples = std::max(m_maxSamples, m_options.captureSamples + 1);
    }
    m_cameraAngles = s.cameraAngles;
    m_cameraTarget = s.cameraTarget * s.renderScale;
    m_cameraDistance = s.cameraDistance * s.renderScale;
    m_lightAngles = s.lightAngles;
    m_lightTarget = s.lightTarget * s.renderScale;
    m_lightDistance = s.lightDistance * s.renderScale;
    m_scene.materials = s.materials;
    if (!m_options.compute && m_scene.materials.empty())
        throw std::invalid_argument("Point-cloud RT scene requires materials");
    if (s.pointsOn) {
        loadPoints(m_frame);
        m_scene.volumes.push_back(sceneVolume());
        sample::SceneInstance instance;
        instance.kind = sample::SceneInstance::Kind::Volume;
        instance.material = uint32_t(s.pointMaterial);
        m_scene.instances.push_back(instance);
    }
    const auto addMesh = [&](sample::SceneMesh mesh, int material) {
        sample::SceneInstance instance;
        instance.kind = sample::SceneInstance::Kind::Mesh;
        instance.geometry = uint32_t(m_scene.meshes.size());
        instance.material = uint32_t(material);
        instance.transform[0] = instance.transform[5] = instance.transform[10] = s.renderScale;
        m_scene.meshes.push_back(std::move(mesh));
        m_scene.instances.push_back(instance);
    };
    for (const auto& model : s.models)
        addMesh(loadMesh(resolveInputPath(m_options.assets, model.path, model.file), model.scale, model.offset),
                model.material);
    if (s.polysOn) {
        m_polyGeometry = uint32_t(m_scene.meshes.size());
        addMesh(
            loadMesh(resolveInputPath(m_options.assets, s.polyPath, formatFrameName(s.polyFile, m_polyFrame)),
                     s.polyScale, s.polyOffset),
            s.polyMaterial);
    }
    if (m_options.compute && !s.pointsOn)
        throw std::invalid_argument("Compute point-cloud rendering requires a point volume");
#if ETHEREAL_POINT_CLOUD_RT
    if (m_renderer) {
        m_renderer->setScene(m_scene);
        m_renderer->setRandomSeed(4732, uint32_t(m_frame));
        if (!s.envmap.empty()) {
            const auto path = resolveInputPath(m_options.assets, "", s.envmap);
            int w = 0, h = 0, components = 0;
            stbi_uc* pixels = stbi_load(path.string().c_str(), &w, &h, &components, 4);
            if (pixels) {
                sample::SceneEnvironment environment;
                environment.width = uint32_t(w);
                environment.height = uint32_t(h);
                environment.rgba.assign(pixels, pixels + size_t(w) * h * 4);
                stbi_image_free(pixels);
                m_renderer->setEnvironment(environment);
            } else
                donut::log::warning("Cannot load point-cloud environment %s", path.string().c_str());
        }
    }
#endif
}
void PointCloudAppScene::loadPoints(int frame) {
    const auto& s = m_options.scene;
    const auto path = resolveInputPath(m_options.assets, s.pointPath, formatFrameName(s.pointFile, frame));
    const auto points = loadPointFrame(path);
    m_reconstruction.reconstruct(
        points, {s.renderScale, uint32_t(std::max(0, s.smooth)), s.smoothParams.x, s.smoothParams.y});
}
sample::SceneMesh PointCloudAppScene::loadMesh(const std::filesystem::path& path, float scale,
                                               dm::float3 offset) const {
    auto fs = MAKE_RC_OBJ_PTR(donut::vfs::NativeFileSystem);
    const auto source = SampleUtils::ObjLoader()(fs, path.generic_string().c_str());
    if (source.positions.empty() || source.indices.empty())
        throw std::runtime_error("Cannot load point-cloud mesh: " + path.string());
    sample::SceneMesh result;
    result.positions.reserve(source.positions.size());
    result.normals.reserve(source.positions.size());
    std::vector<dm::float3> normals = source.normals;
    if (normals.size() != source.positions.size()) {
        normals.assign(source.positions.size(), dm::float3::zero());
        for (const auto& tri : source.indices) {
            const auto n = dm::cross(source.positions[tri.y] - source.positions[tri.x],
                                     source.positions[tri.z] - source.positions[tri.x]);
            normals[tri.x] += n;
            normals[tri.y] += n;
            normals[tri.z] += n;
        }
        for (auto& n : normals)
            n = dm::lengthSquared(n) > 0 ? dm::normalize(n) : dm::float3(0, 1, 0);
    }
    for (size_t i = 0; i < source.positions.size(); ++i) {
        result.positions.push_back(f4(source.positions[i] * scale + offset, 1));
        result.normals.push_back(f4(dm::snorm8ToVector<3>(dm::vectorToSnorm8(normals[i]))));
    }
    for (const auto& tri : source.indices) {
        result.indices.push_back(uint32_t(tri.x));
        result.indices.push_back(uint32_t(tri.y));
        result.indices.push_back(uint32_t(tri.z));
    }
    return result;
}
sample::SceneVolume PointCloudAppScene::sceneVolume() const {
    const auto& s = m_options.scene;
    auto volume = sample::SceneVolume::from(m_reconstruction.snapshot());
    volume.mode = sample::SceneVolumeMode::LevelSet;
    volume.transfer = m_transfer;
    volume.transferLastIndex = 16300;
    volume.steps = f4(s.steps, .001f);
    volume.extinct = f4(s.extinct);
    volume.cutoff = f4(s.cutoff);
    volume.threshold = f4(s.range, 3);
    return volume;
}
bool PointCloudAppScene::topologyOverlay(std::vector<sample::VolumeOverlayLine>& output) const {
    output.clear();
    const auto& host = m_reconstruction.topology();
    const Float4 colors[] = {
        {.1f, 1,   .2f, 1},
        {1,   .7f, .1f, 1},
        {.1f, .6f, 1,   1},
        {1,   .2f, .8f, 1}
    };
    for (uint32_t level = 0; level < host.getConfiguration().levelCount; ++level) {
        const auto pool = host.getPool(0, level);
        if (pool.count && (!pool.data || pool.stride != sizeof(portable::Node)))
            throw std::logic_error("Point-cloud node pool");
        if (pool.count > (1000000 - output.size()) / 12)
            throw std::invalid_argument("Point-cloud overlay too large");
        for (uint64_t i = 0; i < pool.count; ++i) {
            portable::Node node;
            std::memcpy(&node, static_cast<const uint8_t*>(pool.data) + i * pool.stride, sizeof(node));
            const float width = float(host.getRange(int32_t(level)));
            auto vertex = [&](uint32_t c) {
                return Float4{float(node.position.x) + (c & 1 ? width : 0),
                              float(node.position.y) + (c & 2 ? width : 0),
                              float(node.position.z) + (c & 4 ? width : 0), 0};
            };
            for (uint32_t c = 0; c < 8; ++c)
                for (uint32_t axis = 0; axis < 3; ++axis)
                    if (!(c & (1u << axis)))
                        output.push_back({vertex(c), vertex(c | (1u << axis)), colors[level % 4]});
        }
    }
    return true;
}
void PointCloudAppScene::resetSamples() {
    m_samples = 0;
    m_advance = false;
#if ETHEREAL_POINT_CLOUD_RT
    if (m_renderer)
        m_renderer->resetAccumulation();
#endif
}
void PointCloudAppScene::updateView(sample::VolumeViewOptions& view) {
    const auto b = basis(m_cameraAngles);
    const auto light = m_lightTarget + basis(m_lightAngles).back * m_lightDistance;
    view.explicitCamera = true;
    view.cameraEye = f4(m_cameraTarget + b.back * m_cameraDistance);
    view.cameraForward = f4(-b.back);
    view.cameraUp = f4(b.up);
    view.horizontalFov = m_options.scene.cameraFov;
    view.cameraNear = 1;
    view.cameraFar = 10000;
    view.scalarBackground = 3;
    view.mode = sample::VolumeViewMode::Surface;
    view.sectionInset = false;
    view.lightDirection = f4(light - m_cameraTarget);
    view.background = f4(dm::float3(m_options.scene.backgroundColor.x, m_options.scene.backgroundColor.y,
                                    m_options.scene.backgroundColor.z),
                         1);
    view.step = m_options.scene.steps.z;
    view.isovalue = m_options.scene.range.x;
    view.valueMin = m_options.scene.range.y;
    view.valueMax = m_options.scene.range.z;
#if ETHEREAL_POINT_CLOUD_RT
    if (m_renderer)
        m_renderer->setPointLight(f4(light, 1));
#endif
}
void PointCloudAppScene::animate(float, sample::VolumeViewOptions& view) {
    const auto& s = m_options.scene;
    if (m_advance) {
        m_frame = advanceFrame(m_frame, s.frameStep);
        m_samples = 0;
        m_advance = false;
        if (s.pointsOn && s.frameStep != 0) {
            loadPoints(m_frame);
            m_scene.volumes[0] = sceneVolume();
#if ETHEREAL_POINT_CLOUD_RT
            if (m_renderer)
                m_renderer->updateVolume(0, m_scene.volumes[0]);
#endif
        }
        if (s.polysOn && s.polyFrameStep != 0) {
            m_polyFrame = advanceFrame(m_polyFrame, s.polyFrameStep);
            auto mesh = loadMesh(
                resolveInputPath(m_options.assets, s.polyPath, formatFrameName(s.polyFile, m_polyFrame)),
                s.polyScale, s.polyOffset);
            m_scene.meshes.at(m_polyGeometry) = std::move(mesh);
#if ETHEREAL_POINT_CLOUD_RT
            if (m_renderer)
                m_renderer->updateMesh(m_polyGeometry, m_scene.meshes[m_polyGeometry]);
#endif
        }
#if ETHEREAL_POINT_CLOUD_RT
        if (m_renderer) {
            m_renderer->setRandomSeed(4732, uint32_t(m_frame));
            m_renderer->resetAccumulation();
        }
#endif
    }
    if (m_options.keyAnimation && m_frame >= 1100 && m_frame <= 1340) {
        const float u = float(m_frame - 1100) / 240;
        m_cameraAngles = dm::float3(0, 35, 0) + (dm::float3(0, 24.6f, 0) - dm::float3(0, 35, 0)) * u;
        m_cameraTarget =
            (dm::float3(240, 0, 0) + (dm::float3(362, -201, 20) - dm::float3(240, 0, 0)) * u) * s.renderScale;
        m_cameraDistance = (1600 + (2132 - 1600) * u) * s.renderScale;
    }
    updateView(view);
}
slang::RaycastOutput PointCloudAppScene::renderCustom(const slang::RaycastParameters& camera,
                                                      const sample::VolumeViewOptions&) {
    m_lastCamera = camera;
    m_hasCamera = true;
    slang::RaycastOutput result;
#if ETHEREAL_POINT_CLOUD_RT
    if (m_renderer) {
        // Legacy OptiX primary rays ignore the raster camera's near/far planes.
        // Keep the shared viewer clipping for overlays and compute, while retaining
        // distant ground and geometry closer than the camera near plane in RT.
        m_lastCamera.direction00.w = 0;
        m_lastCamera.march.y = 0;
        m_lastCamera.march.z = 1e16f;
        result = m_renderer->render(m_lastCamera);
        // Camera/resolution changes can reset accumulation inside the renderer.
        m_samples = m_renderer->sampleCount();
    } else
#endif
    {
        m_runtime->flush();
        result = m_surface->render(m_reconstruction.snapshot(), 0, UINT32_MAX, camera, m_transfer, 3);
        ++m_samples;
    }
    m_advance = m_samples >= m_maxSamples;
    return result;
}
std::vector<sample::VolumeOverlayLine> PointCloudAppScene::overlayLines() const {
    std::vector<sample::VolumeOverlayLine> result;
    if (!m_options.points)
        return result;
    const auto lines = m_reconstruction.pointLines();
    result.reserve(lines.size());
    for (const auto& l : lines)
        result.push_back({l.start, l.end, l.color});
    return result;
}
void PointCloudAppScene::buildUI(sample::VolumeViewOptions& view) {
    ImGui::Checkbox("Points", &m_options.points);
    ImGui::SameLine();
    ImGui::Checkbox("Topology", &view.topology);
    ImGui::Text("Frame %d, sample %u / %u, %zu points", m_frame, m_samples, m_maxSamples,
                m_reconstruction.hostPositions().size());
    if (m_reconstruction.hasVolume()) {
        const auto r = m_reconstruction.snapshot()->getDesc().volume.atlasResolutionAndApron;
        const double voxels = double(r.x) * r.y * r.z;
        ImGui::Text("Bricks: %u, %.1f MVoxels, scalar atlas %.1f MB",
                    m_reconstruction.snapshot()->getDesc().activeLeafCount, voxels / 1e6,
                    voxels * 4 / (1024 * 1024));
    }
    if (m_options.compute)
        ImGui::TextUnformatted("Compute level-set preview");
    ImGui::TextUnformatted("Left: orbit, Middle: pan, Right: distance, Shift: light");
}
bool PointCloudAppScene::key(int key, sample::VolumeViewOptions& view) {
    if (key == '1') {
        m_options.points = !m_options.points;
        return true;
    }
    if (key == '2') {
        view.topology = !view.topology;
        return true;
    }
    return key == 'V' || key == 'v' || key == 'I' || key == 'i';
}
bool PointCloudAppScene::mouseDrag(int button, float dx, float dy, int modifiers,
                                   sample::VolumeViewOptions& view) {
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
    resetSamples();
    updateView(view);
    return true;
}
bool PointCloudAppScene::mouseScroll(float amount, sample::VolumeViewOptions& view) {
    m_cameraDistance -= amount * 20;
    resetSamples();
    updateView(view);
    return true;
}
uint32_t PointCloudAppScene::verifyVolumeContribution() {
#if ETHEREAL_POINT_CLOUD_RT
    if (!m_renderer || !m_hasCamera || !m_reconstruction.hasVolume())
        throw std::logic_error("Volume contribution verification requires a rendered RT point scene");
    const char* target = m_runtime->device()->getGraphicsAPI() == nvrhi::GraphicsAPI::CUDA
                             ? "optix"
                             : donut::app::GetShaderTypeName(m_runtime->device()->getGraphicsAPI());
    sample::SlangSceneRT probe(*m_runtime, donut::app::GetDirectoryWithExecutable() / "shaders/gvdb_slang" /
                                               target / "SlangSceneRT.bin");
    probe.setScene(m_scene);
    probe.setRandomSeed(4732, uint32_t(m_frame));
    probe.setPointLight(f4(m_lightTarget + basis(m_lightAngles).back * m_lightDistance, 1));
    const auto full = probe.render(m_lastCamera, 0);
    std::vector<float> fullDepth(size_t(full.width) * full.height), meshDepth(fullDepth.size(), 1e10f);
    m_runtime->read(full.depth, fullDepth.data(), fullDepth.size() * 4);
    auto meshes = m_scene;
    meshes.instances.erase(std::remove_if(meshes.instances.begin(), meshes.instances.end(),
                                          [](const sample::SceneInstance& i) {
                                              return i.kind == sample::SceneInstance::Kind::Volume;
                                          }),
                           meshes.instances.end());
    if (!meshes.instances.empty()) {
        probe.setScene(meshes);
        const auto output = probe.render(m_lastCamera, 0);
        m_runtime->read(output.depth, meshDepth.data(), meshDepth.size() * 4);
    }
    uint32_t count = 0;
    for (size_t i = 0; i < fullDepth.size(); ++i)
        if (std::isfinite(fullDepth[i]) && fullDepth[i] >= 0 && fullDepth[i] != 1e10f &&
            fullDepth[i] < m_lastCamera.march.z && fullDepth[i] + .01f < meshDepth[i])
            ++count;
    if (!count)
        throw std::runtime_error("Mixed point-cloud image has no verified point-volume depth contribution");
    return count;
#else
    throw std::runtime_error("Mixed point-cloud renderer was not built");
#endif
}
std::string PointCloudAppScene::describe() const {
    char text[256];
    std::snprintf(text, sizeof(text), "Point-cloud %s frame%d poly%d sample%u/%u points%zu bricks%u",
                  m_options.compute ? "compute" : "Slang RT", m_frame, m_polyFrame, m_samples, m_maxSamples,
                  m_reconstruction.hostPositions().size(),
                  m_reconstruction.hasVolume() ? m_reconstruction.snapshot()->getDesc().activeLeafCount : 0);
    return text;
}
}  // namespace gvdb::samples::pointcloud
