// SPDX-License-Identifier: Apache-2.0
#include "InteractiveScene.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <donut/app/ApplicationBase.h>
#include <imgui.h>
#include <sample-utils/SceneAssets.h>
#include <sample-utils/OwnedVolumeIO.h>
#include <stdexcept>
namespace gvdb::samples::interactive
{
namespace
{
slang::Float4 f4(dm::float3 v, float w = 0)
{
    return {v.x, v.y, v.z, w};
}
std::shared_ptr<const Input> checkedInput(std::shared_ptr<const Input> input)
{
    if (!input)
        throw std::invalid_argument("Interactive scene requires CPU input");
    return input;
}
struct Basis
{
    dm::float3 right, up, back;
};
Basis basis(dm::float3 angles)
{
    const float yaw = dm::radians(angles.x), pitch = dm::radians(angles.y);
    Basis result;
    result.back = {std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)};
    const dm::float3 up = std::abs(result.back.y) > .999f ? dm::float3(0, 0, 1) : dm::float3(0, 1, 0);
    result.right = dm::normalize(dm::cross(up, result.back));
    result.up = dm::cross(result.back, result.right);
    return result;
}
std::filesystem::path artifact(nvrhi::IDevice *device)
{
    return donut::app::GetDirectoryWithExecutable() / "shaders/gvdb_slang" /
           (device->getGraphicsAPI() == nvrhi::GraphicsAPI::CUDA
                ? "optix"
                : donut::app::GetShaderTypeName(device->getGraphicsAPI())) /
           "SlangSceneRT.bin";
}
sample::SceneMaterial referenceMaterial()
{
    sample::SceneMaterial m;
    m.lightWidth = .5f;
    m.shadowWidth = .5f;
    m.diffColor = {.5f, .54f, .5f};
    m.specColor = {.7f, .7f, .7f};
    m.specPower = 80;
    m.envColor = dm::float3::zero();
    m.reflWidth = .3f;
    m.reflColor = {.8f, .8f, .8f};
    m.refrWidth = 0;
    m.refrColor = dm::float3::zero();
    m.refrIor = 1.2f;
    m.refrAmount = 1;
    m.refrOffset = 15;
    return m;
}
} // namespace
std::shared_ptr<Input> loadInput(const std::filesystem::path &volume, const std::filesystem::path &mesh)
{
    auto result = std::make_shared<Input>();
    std::string error;
    if (!slang::loadVBXFile(volume, result->volume, error))
        throw std::runtime_error(error);
    result->mesh = sample::loadSceneObj(mesh, 100);
    return result;
}
std::vector<slang::Float4> referenceTransfer()
{
    std::vector<slang::Float4> result(16384);
    const auto range = [&](uint32_t first, uint32_t last, dm::float4 a, dm::float4 b) {
        for (uint32_t i = first; i < last; ++i)
        {
            auto v = a + (b - a) * (float(i - first) / float(last - first));
            result[i] = {v.x, v.y, v.z, v.w};
        }
    };
    range(0, 4096, {0, 0, 0, 0}, {0, 0, 0, 0});
    range(4096, 8192, {0, .5f, 1, 0}, {0, 0, 1, .2f});
    range(8192, 12288, {0, 0, 1, .2f}, {0, 1, 1, .3f});
    range(12288, 16384, {0, 1, 1, .3f}, {0, 1, 1, 1});
    return result;
}
InteractiveScene::InteractiveScene(ethereal::slang::ComputeRuntime &runtime, std::shared_ptr<const Input> input,
                                   Options options)
    : m_runtime(runtime), m_input(checkedInput(std::move(input))), m_options(options),
      m_renderer(runtime, artifact(runtime.device()))
{
    setMaxSamples(options.maxSamples);
    if (!std::isfinite(options.secondaryBias) || options.secondaryBias < 0)
        throw std::invalid_argument("Secondary ray bias must be finite and nonnegative");
    // CPU input is encoded once; public import owns the only GPU publication.
    std::vector<uint8_t> encoded;
    std::string error;
    if (!slang::saveVBX(m_input->volume, encoded, error))
        throw std::runtime_error(error);
    runtime.flush();
    const auto shaders = donut::app::GetDirectoryWithExecutable() / "shaders/gvdb_owned" /
                         donut::app::GetShaderTypeName(runtime.device()->getGraphicsAPI());
    sample::OwnedVolumeLoadOptions load;
    load.refreshApronChannels = 1;
    load.mutationArtifact = shaders / "GVDBAtlasMutation_mutateAtlas.bin";
    m_volume = sample::loadOwnedVBX(runtime.device(), shaders / "GVDBAtlasRepack_relocateAtlas.bin",
                                   encoded.data(), encoded.size(), load);
    auto volume = sample::SceneVolume::from(m_volume);
    volume.transfer = referenceTransfer();
    volume.transferLastIndex = 16300;
    volume.mode = options.shading;
    volume.steps = {.5f, 16, .5f, .001f};
    volume.extinct = {-.25f, 1, 0, 0};
    volume.threshold = {.1f, 0, .3f, 0};
    volume.cutoff = {.001f, .001f, 0, 0};
    auto material = referenceMaterial();
    material.shadowBias = material.reflBias = material.refrBias = options.secondaryBias;
    if (m_input->analytic)
    {
        volume.transfer = {{.6f, .2f, .1f, .2f}, {.6f, .2f, .1f, .2f}};
        volume.transferLastIndex = UINT32_MAX;
        volume.steps = {.25f, 16, .25f, .001f};
        volume.extinct = {-1, 1, 0, 0};
        volume.threshold = {.4f, 0, 1, 0};
        material.ambColor = {.2f, .4f, .6f};
        material.diffColor = material.specColor = material.reflColor = material.refrColor = dm::float3::zero();
        material.lightWidth = material.reflWidth = material.refrWidth = 0;
        material.shadowBias = 10;
        m_options.translation = {0, 0, 0, 0};
    }
    m_scene.meshes = {m_input->mesh};
    m_scene.volumes = {volume};
    m_scene.materials = {material, material};
    sample::SceneInstance mesh, fog;
    fog.kind = sample::SceneInstance::Kind::Volume;
    fog.material = options.shading == sample::SceneVolumeMode::Deep ? 1 : 0;
    m_scene.instances = {mesh, fog};
    applyTransform();
    m_renderer.setScene(m_scene);
    m_renderer.setRandomSeed(4732, 0);
}
void InteractiveScene::applyTransform()
{
    auto &t = m_scene.instances[1].transform;
    const float scale = m_input->analytic ? 1 : .25f;
    t[0] = t[5] = t[10] = scale;
    t[3] = m_options.translation.x + (m_input->analytic ? 0 : -125 * scale);
    t[7] = m_options.translation.y + (m_input->analytic ? 0 : -160 * scale);
    t[11] = m_options.translation.z + (m_input->analytic ? 0 : -125 * scale);
}
void InteractiveScene::reset()
{
    m_samples = 0;
    m_advance = false;
    m_renderer.resetAccumulation();
}
void InteractiveScene::setShading(sample::SceneVolumeMode mode)
{
    if (uint32_t(mode) > uint32_t(sample::SceneVolumeMode::EmptySkip))
        throw std::invalid_argument("Unknown volume shading mode");
    m_options.shading = mode;
    m_scene.volumes[0].mode = mode;
    m_scene.instances[1].material = mode == sample::SceneVolumeMode::Deep ? 1 : 0;
    m_renderer.setScene(m_scene);
    reset();
}
void InteractiveScene::setTranslation(slang::Float4 t)
{
    if (!std::isfinite(t.x) || !std::isfinite(t.y) || !std::isfinite(t.z))
        throw std::invalid_argument("Nonfinite volume translation");
    m_options.translation = t;
    applyTransform();
    m_renderer.updateTransform(1, m_scene.instances[1].transform);
    reset();
}
void InteractiveScene::setMaxSamples(uint32_t n)
{
    if (!n || n > 1000000)
        throw std::invalid_argument("Sample limit must be 1..1000000");
    m_options.maxSamples = n;
}
void InteractiveScene::updateView(sample::VolumeViewOptions &view)
{
    const auto b = basis(m_cameraAngles), lightBasis = basis(m_lightAngles);
    auto light = m_lightTarget + lightBasis.back * m_lightDistance;
    view.explicitCamera = true;
    view.requestRayTracing = true;
    view.horizontalFov = 30;
    view.cameraNear = .1f;
    view.cameraFar = 5000;
    view.cameraEye = f4(m_cameraTarget + b.back * m_cameraDistance);
    view.cameraForward = f4(-b.back);
    view.cameraUp = f4(b.up);
    view.mode = sample::VolumeViewMode::Volume;
    view.topology = view.sectionInset = false;
    view.step = m_scene.volumes[0].steps.x;
    view.background = {.1f, .2f, .4f, 1};
    view.lightDirection = f4(light - m_cameraTarget);
    if (m_input->analytic)
    {
        view.cameraEye = {-1, 1, 1, 0};
        view.cameraForward = {1, 0, 0, 0};
        view.cameraUp = {0, 1, 0, 0};
        light = {5, 100, 1};
    }
    m_renderer.setPointLight(f4(light, 1));
}
void InteractiveScene::animate(float, sample::VolumeViewOptions &view)
{
    if (m_advance)
    {
        if (m_frame == UINT32_MAX)
            throw std::overflow_error("Dataset frame exhausted");
        ++m_frame;
        reset();
        m_renderer.setRandomSeed(4732, m_frame);
    }
    updateView(view);
}
slang::RaycastOutput InteractiveScene::renderCustom(const slang::RaycastParameters &p,
                                                    const sample::VolumeViewOptions &)
{
    m_lastCamera = p;
    auto image = m_renderer.render(p);
    m_samples = m_renderer.sampleCount();
    m_advance = m_samples >= m_options.maxSamples;
    return image;
}
bool InteractiveScene::acceptsKeyRepeat(int key) const
{
    return key == 'X' || key == 'Y' || key == 'Z' || key == 'A' || key == 'S' || key == 'D' ||
           key == '[' || key == ']';
}
bool InteractiveScene::key(int key, sample::VolumeViewOptions &view)
{
    const sample::SceneVolumeMode modes[] = {sample::SceneVolumeMode::Deep, sample::SceneVolumeMode::Surface,
                                             sample::SceneVolumeMode::EmptySkip, sample::SceneVolumeMode::LevelSet};
    if (key >= '1' && key <= '4')
        setShading(modes[key - '1']);
    else if (key == 'R')
        reset();
    else if (key == '[')
        setMaxSamples(std::max(1u, m_options.maxSamples / 2));
    else if (key == ']')
        setMaxSamples(std::min(1000000u, m_options.maxSamples * 2));
    else if (key == 'X' || key == 'Y' || key == 'Z' || key == 'A' || key == 'S' || key == 'D')
    {
        auto t = m_options.translation;
        const float step = (key == 'A' || key == 'S' || key == 'D') ? -1.f : 1.f;
        if (key == 'X' || key == 'A')
            t.x += step;
        else if (key == 'Y' || key == 'S')
            t.y += step;
        else
            t.z += step;
        setTranslation(t);
    }
    else
        return key == 'V' || key == 'I';
    updateView(view);
    return true;
}
bool InteractiveScene::mouseDrag(int button, float dx, float dy, int mods, sample::VolumeViewOptions &view)
{
    auto &angles = (mods & 1) ? m_lightAngles : m_cameraAngles;
    auto &distance = (mods & 1) ? m_lightDistance : m_cameraDistance;
    if (button == 0)
    {
        angles.x += dx * .2f;
        angles.y -= dy * .2f;
    }
    else if (button == 2)
    {
        const auto b = basis(m_cameraAngles);
        m_cameraTarget += b.right * (dx * m_cameraDistance / 1000) + b.up * (-dy * m_cameraDistance / 1000);
    }
    else if (button == 1)
        distance = std::max(.1f, distance - dy);
    else
        return false;
    reset();
    updateView(view);
    return true;
}
bool InteractiveScene::mouseScroll(float amount, sample::VolumeViewOptions &view)
{
    m_cameraDistance = std::max(.1f, m_cameraDistance - amount * 20);
    reset();
    updateView(view);
    return true;
}
void InteractiveScene::buildUI(sample::VolumeViewOptions &view)
{
    static const char *names[] = {"Volume (deep)", "Trilinear surface", "Empty skipping", "Level set"};
    const sample::SceneVolumeMode modes[] = {sample::SceneVolumeMode::Deep, sample::SceneVolumeMode::Surface,
                                             sample::SceneVolumeMode::EmptySkip, sample::SceneVolumeMode::LevelSet};
    int current = 0;
    for (int i = 0; i < 4; ++i)
        if (m_options.shading == modes[i])
            current = i;
    if (ImGui::Combo("Shading", &current, names, 4))
        setShading(modes[current]);
    auto translation = m_options.translation;
    if (ImGui::DragFloat3("Volume translate", &translation.x, 1))
        setTranslation(translation);
    int count = int(m_options.maxSamples);
    if (ImGui::SliderInt("Max samples", &count, 1, 4096))
        setMaxSamples(uint32_t(count));
    ImGui::Text("Frame %u, sample %u / %u", m_frame, m_samples, m_options.maxSamples);
    ImGui::TextUnformatted("Left: orbit, Middle: pan, Right: distance, Shift: light");
    updateView(view);
}
std::string InteractiveScene::describe() const
{
    const char *names[] = {"surface", "deep volume", "level set", "empty skip"};
    char text[256];
    std::snprintf(text, sizeof(text), "Lucy + explosion; %s; frame %u sample %u/%u; 1..4 shading, R reset",
                  names[uint32_t(m_options.shading)], m_frame, m_samples, m_options.maxSamples);
    return text;
}
} // namespace gvdb::samples::interactive
