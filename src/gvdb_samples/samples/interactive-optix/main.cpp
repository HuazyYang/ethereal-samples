// Lucy and explosion through NVRHI Slang RT on D3D12, Vulkan and CUDA/OptiX.
// SPDX-License-Identifier: Apache-2.0
#include "ExampleDevice.h"
#include "InteractiveScene.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
namespace
{
uint32_t positive(const char *text)
{
    size_t consumed = 0;
    auto value = std::stoll(text, &consumed);
    if (text[consumed] || value < 1 || value > 1000000)
        throw std::invalid_argument("Expected an integer in 1..1000000");
    return uint32_t(value);
}
} // namespace
int main(int argc, const char **argv)
{
    try
    {
        namespace app = gvdb::samples::interactive;
        namespace sample = gvdb::sample;
        app::Options sceneOptions;
        sample::VolumeViewOptions view;
        view.requestRayTracing = true;
        view.showUI = true;
        std::filesystem::path volume = std::filesystem::path(GVDB_SAMPLES_ASSETS_DIR) / "explosion.vbx",
                              mesh = std::filesystem::path(GVDB_SAMPLES_ASSETS_DIR) / "lucy.obj", output;
        bool headless = false, selfTest = false, verify = false;
        uint32_t frames = 1, captureSamples = 0;
        for (int i = 1; i < argc; ++i)
        {
            const auto value = [&]() {
                if (++i >= argc)
                    throw std::invalid_argument("Missing option value");
                return argv[i];
            };
            if (!std::strcmp(argv[i], "--help"))
            {
                std::puts(
                    "interactive-optix [-dx12|-vk|-cuda] [--adapter N] [--validation]\n"
                    "  [--shading volume|trilinear|emptyskip|levelset] [--translate X Y Z]\n"
                    "  [--mesh lucy.obj] [--volume explosion.vbx] [--max-samples N]\n"
                    "  [--secondary-bias world-distance]\n"
                    "  [--headless] [--frames N] [--samples N] [--screenshot image.png]\n"
                    "  [--width N] [--height N] [--verify] [--self-test]\n"
                    "Inspector retains shading, volume translation and max samples.\n"
                    "Keys1..4 select modes; R resets; [/] sample limit; X/Y/Z and A/S/D translate +/-axes.\n"
                    "Left orbit, middle pan, right distance, Shift light; wheel camera distance.\n"
                    "CUDA runs headless. Screenshot defaults to64 samples; --frames is a bounded window/sample count.");
                return 0;
            }
            if (!std::strcmp(argv[i], "--headless") || !std::strcmp(argv[i], "-cuda"))
                headless = true;
            else if (!std::strcmp(argv[i], "--self-test"))
                selfTest = true;
            else if (!std::strcmp(argv[i], "--verify"))
                verify = true;
            else if (!std::strcmp(argv[i], "--volume"))
                volume = value();
            else if (!std::strcmp(argv[i], "--mesh"))
                mesh = value();
            else if (!std::strcmp(argv[i], "--width"))
                view.width = positive(value());
            else if (!std::strcmp(argv[i], "--height"))
                view.height = positive(value());
            else if (!std::strcmp(argv[i], "--max-samples"))
                sceneOptions.maxSamples = positive(value());
            else if (!std::strcmp(argv[i], "--secondary-bias")) {
                const std::string text = value();
                size_t end = 0;
                sceneOptions.secondaryBias = std::stof(text, &end);
                if (end != text.size()) throw std::invalid_argument("Invalid secondary ray bias");
            }
            else if (!std::strcmp(argv[i], "--frames"))
            {
                frames = positive(value());
                view.frameLimit = frames;
            }
            else if (!std::strcmp(argv[i], "--samples"))
                captureSamples = positive(value());
            else if (!std::strcmp(argv[i], "--screenshot") || !std::strcmp(argv[i], "--output"))
            {
                output = value();
                view.screenshot = output;
            }
            else if (!std::strcmp(argv[i], "--translate"))
            {
                sceneOptions.translation.x = std::stof(value());
                sceneOptions.translation.y = std::stof(value());
                sceneOptions.translation.z = std::stof(value());
            }
            else if (!std::strcmp(argv[i], "--shading"))
            {
                const std::string mode = value();
                if (mode == "volume")
                    sceneOptions.shading = sample::SceneVolumeMode::Deep;
                else if (mode == "trilinear")
                    sceneOptions.shading = sample::SceneVolumeMode::Surface;
                else if (mode == "emptyskip")
                    sceneOptions.shading = sample::SceneVolumeMode::EmptySkip;
                else if (mode == "levelset")
                    sceneOptions.shading = sample::SceneVolumeMode::LevelSet;
                else
                    throw std::invalid_argument("Unknown shading mode");
            }
        }
        if (!output.empty() && !captureSamples && !view.frameLimit)
            captureSamples = 64;
        if (captureSamples)
        {
            frames = captureSamples;
            view.frameLimit = frames;
            sceneOptions.maxSamples = std::max(sceneOptions.maxSamples, captureSamples);
        }
        if (!view.width || !view.height || uint64_t(view.width) * view.height > 16 * 1024 * 1024)
            throw std::invalid_argument("Invalid output dimensions");
        if (selfTest)
        {
            ethereal::slang::ExampleDevice device(argc, argv, true);
            ethereal::slang::ComputeRuntime runtime(device.get(), device.shaderPath());
            app::runSelfTests(runtime);
            return 0;
        }
        const auto input = app::loadInput(volume, mesh);
        if (headless)
        {
            ethereal::slang::ExampleDevice device(argc, argv, true);
            ethereal::slang::ComputeRuntime runtime(device.get(), device.shaderPath());
            app::InteractiveScene scene(runtime, input, sceneOptions);
            gvdb::slang::RaycastOutput image;
            for (uint32_t frame = 0; frame < frames; ++frame)
            {
                scene.animate(0, view);
                image = sample::renderVolumeView(runtime, scene, view);
            }
            std::vector<gvdb::slang::Float4> pixels(size_t(image.width) * image.height);
            std::vector<float> depth(pixels.size());
            runtime.read(image.color, pixels.data(), pixels.size() * 16);
            runtime.read(image.depth, depth.data(), depth.size() * 4);
            size_t hits = 0;
            for (size_t i = 0; i < pixels.size(); ++i)
            {
                auto p = pixels[i];
                if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || !std::isfinite(p.w) ||
                    std::isnan(depth[i]))
                    throw std::runtime_error("Nonfinite interactive scene output");
                hits += std::isfinite(depth[i]) && depth[i] >= 0 && depth[i] < view.cameraFar;
            }
            if (!hits)
                throw std::runtime_error("Interactive scene contains no visible geometry");
            if (verify)
                app::verifyAssetCoverage(runtime, scene);
            if (output.empty())
                output = "interactive-optix.png";
            sample::writeVolumeImage(runtime, image, output);
            std::printf("PASS %zu scene hit pixels; %s\n", hits, scene.describe().c_str());
            return 0;
        }
        return sample::runVolumeWindow(
            argc, argv, "GVDB Slang interactive ray tracing",
            [&](ethereal::slang::ComputeRuntime &runtime) {
                return std::make_unique<app::InteractiveScene>(runtime, input, sceneOptions);
            },
            view);
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "interactive-optix: %s\n", error.what());
        return 1;
    }
}
