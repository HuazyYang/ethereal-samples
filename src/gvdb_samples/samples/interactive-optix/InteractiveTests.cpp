// Analytic input, original scene contract, controls, and real asset coverage.
// SPDX-License-Identifier: Apache-2.0
#include "../../tests/PortableFixtures.h"
#include "InteractiveScene.h"
#include <cmath>
#include <cstdio>
#include <donut/app/ApplicationBase.h>
#include <stdexcept>
namespace gvdb::samples::interactive
{
namespace
{
void require(bool v, const char *text)
{
    if (!v)
        throw std::runtime_error(text);
}
void close(float actual, float expected, const char *text, float tolerance = .0003f)
{
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance)
    {
        std::fprintf(stderr, "%s actual%g expected%g\n", text, actual, expected);
        throw std::runtime_error(text);
    }
}
size_t countHits(ethereal::slang::ComputeRuntime &runtime, const slang::RaycastOutput &image)
{
    std::vector<slang::Float4> color(size_t(image.width) * image.height);
    std::vector<float> depth(color.size());
    runtime.read(image.color, color.data(), color.size() * 16);
    runtime.read(image.depth, depth.data(), depth.size() * 4);
    size_t count = 0;
    for (size_t i = 0; i < color.size(); ++i)
    {
        auto p = color[i];
        require(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) && std::isfinite(p.w) &&
                    !std::isnan(depth[i]),
                "Nonfinite scene color or NaN depth");
        count += std::isfinite(depth[i]) && depth[i] >= 0 && depth[i] < 1e9f;
    }
    return count;
}
std::filesystem::path artifact(nvrhi::IDevice *device)
{
    return donut::app::GetDirectoryWithExecutable() / "shaders/gvdb_slang" /
           (device->getGraphicsAPI() == nvrhi::GraphicsAPI::CUDA
                ? "optix"
                : donut::app::GetShaderTypeName(device->getGraphicsAPI())) /
           "SlangSceneRT.bin";
}
} // namespace
void runSelfTests(ethereal::slang::ComputeRuntime &runtime)
{
    auto input = std::make_shared<Input>();
    input->analytic = true;
    auto fixture = slang::fixtures::twoBrickVolume(.5f);
    std::string error;
    require(slang::packVolume(slang::fixtures::volumeSource(fixture), input->volume.packed, error), error.c_str());
    input->volume.atlas = fixture.atlas;
    input->mesh.positions = {{6, 0, 0, 1}, {6, 1, 2, 1}, {6, 2, 0, 1}};
    input->mesh.indices = {0, 1, 2};
    InteractiveScene scene(runtime, input);
    sample::VolumeViewOptions view;
    view.width = view.height = 1;
    scene.animate(0, view);
    auto p = slang::fixtures::raycastParameters(fixture);
    p.image = {1, 1, 0, 128};
    p.background = {.1f, .2f, .4f, 1};
    const auto check = [&](sample::SceneVolumeMode mode, slang::Float4 expected, float expectedDepth) {
        scene.setShading(mode);
        scene.animate(0, view);
        auto image = scene.renderCustom(p, view);
        slang::Float4 pixel{};
        float depth = 0;
        runtime.read(image.color, &pixel, 16);
        runtime.read(image.depth, &depth, 4);
        close(pixel.x, expected.x, "analytic red");
        close(pixel.y, expected.y, "analytic green");
        close(pixel.z, expected.z, "analytic blue");
        close(pixel.w, 1, "analytic alpha");
        close(depth, expectedDepth, "analytic first-hit distance");
    };
    const auto pack = [](float v) { return std::floor(v * 255 + .5f) / 255; };
    const float opacity = 1 - std::exp(-.2f * 14 * .25f), a = pack(opacity);
    check(sample::SceneVolumeMode::Deep,
          {.2f * (1 - a) + pack(.6f * opacity) * a, .4f * (1 - a) + pack(.2f * opacity) * a,
           .6f * (1 - a) + pack(.1f * opacity) * a, 1},
          1.25f);
    check(sample::SceneVolumeMode::Surface, {.2f, .4f, .6f, 1}, .5f);
    check(sample::SceneVolumeMode::LevelSet, {.2f, .4f, .6f, 1}, 1.25f);
    check(sample::SceneVolumeMode::EmptySkip, {.2f, .4f, .6f, 1}, 1.001f);
    scene.setShading(sample::SceneVolumeMode::Deep);
    scene.setTranslation({20, 0, 0, 0});
    scene.animate(0, view);
    auto output = scene.renderCustom(p, view);
    float depth = 0;
    runtime.read(output.depth, &depth, 4);
    close(depth, 6.9994f, "translated volume exposes mesh", .002f);
    scene.setTranslation({0, 0, 0, 0});
    require(scene.sampleCount() == 0, "translation resets samples");
    scene.setMaxSamples(2);
    scene.renderCustom(p, view);
    scene.renderCustom(p, view);
    require(scene.sampleCount() == 2 && scene.frame() == 0, "sample limit reached before frame advance");
    scene.animate(0, view);
    require(scene.frame() == 1 && scene.sampleCount() == 0, "sample limit advances frame and resets accumulation");
    scene.renderCustom(p, view);
    scene.mouseDrag(0, 10, -5, 0, view);
    require(scene.sampleCount() == 0, "camera orbit resets samples");
    scene.renderCustom(p, view);
    scene.mouseDrag(1, 0, 2, 1, view);
    require(scene.sampleCount() == 0, "light distance resets samples");
    scene.renderCustom(p, view);
    scene.mouseScroll(1, view);
    require(scene.sampleCount() == 0, "wheel resets samples");
    scene.renderCustom(p, view);
    scene.key('R', view);
    require(scene.sampleCount() == 0, "reset key clears samples");
    scene.key('2', view);
    require(scene.description().volumes[0].mode == sample::SceneVolumeMode::Surface, "shading key selects pipeline");
    bool rejected = false;
    try
    {
        scene.setMaxSamples(0);
    }
    catch (const std::invalid_argument &)
    {
        rejected = true;
    }
    require(rejected, "zero sample limit rejected");
    const auto table = referenceTransfer();
    require(table.size() == 16384, "legacy table size");
    close(table[4096].y, .5f, "legacy transfer discontinuity");
    close(table[8192].w, .2f, "legacy transfer second segment");
    close(table[12288].y, 1, "legacy transfer final segment");
    auto original = std::make_shared<Input>(*input);
    original->analytic = false;
    InteractiveScene defaults(runtime, original);
    defaults.animate(0, view);
    const auto &transform = defaults.description().instances[1].transform;
    close(transform[0], .25f, "original volume scale");
    close(transform[3], 12.75f, "original volume X translation");
    close(transform[7], -40, "original volume Y translation");
    close(transform[11], -15.25f, "original volume Z translation");
    close(view.cameraEye.x, 400 * std::cos(dm::radians(30.f)) * std::sin(dm::radians(-20.f)), "original camera X");
    close(view.cameraEye.y, 200, "original camera Y");
    close(view.horizontalFov, 30, "original horizontal FOV");
    require(defaults.description().volumes[0].transferLastIndex == 16300, "legacy transfer lookup endpoint");
    require(defaults.description().materials.size() == 2, "distinct surface/deep material records");
    std::puts("PASS interactive scene modes, attenuation, transforms, material/transfer defaults, controls and "
              "progressive rollover");
}
void verifyAssetCoverage(ethereal::slang::ComputeRuntime &runtime, const InteractiveScene &app)
{
    sample::SlangSceneRT probe(runtime, artifact(runtime.device()));
    auto full = app.description();
    auto mesh = full, volume = full;
    mesh.instances.erase(mesh.instances.begin() + 1);
    volume.instances.erase(volume.instances.begin());
    probe.setPointLight({100, 141.42136f, 100, 1});
    probe.setScene(mesh);
    const auto meshImage = probe.render(app.lastCamera(), 0);
    const auto meshHits = countHits(runtime, meshImage);
    probe.setScene(volume);
    const auto volumeHits = countHits(runtime, probe.render(app.lastCamera(), 0));
    probe.setScene(full);
    const auto fullImage = probe.render(app.lastCamera(), 0);
    const auto fullHits = countHits(runtime, fullImage);
    require(meshHits > 0 && volumeHits > 0 && fullHits > 0,
            "Lucy/explosion asset coverage requires both geometry types");
    std::vector<float> meshDepth(size_t(meshImage.width) * meshImage.height), fullDepth(meshDepth.size());
    runtime.read(meshImage.depth, meshDepth.data(), meshDepth.size() * sizeof(float));
    runtime.read(fullImage.depth, fullDepth.data(), fullDepth.size() * sizeof(float));
    size_t visibleVolume = 0;
    for (size_t i = 0; i < fullDepth.size(); ++i)
        visibleVolume += std::isfinite(fullDepth[i]) && fullDepth[i] >= 0 && fullDepth[i] + .01f < meshDepth[i];
    require(visibleVolume > 0, "Mixed asset image must contain visible volume in front of mesh-only depth");
    std::printf("PASS asset coverage: %zu mesh, %zu volume, %zu mixed, %zu visible-volume hit pixels\n", meshHits,
                volumeHits, fullHits, visibleVolume);
}
} // namespace gvdb::samples::interactive
