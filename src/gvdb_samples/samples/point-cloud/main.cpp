// Point-frame reconstruction and mixed Slang mesh/volume rendering through NVRHI.
// SPDX-License-Identifier: Apache-2.0
#include "PointCloudAppScene.h"
#include "PointCloudAppTests.h"
#include "ExampleDevice.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
namespace {
struct Options {
    gvdb::samples::pointcloud::AppOptions scene;
    gvdb::sample::VolumeViewOptions view;
    uint32_t frames = 1;
    bool headless = false, info = false, verifyVolume = false, inputTest = false, rayPolicyTest = false;
    std::filesystem::path output = "point-cloud.png";
};
uint32_t positive(const char* text, const char* option) {
    const auto value = std::stoull(text);
    if (!value || value > 1000000)
        throw std::invalid_argument(std::string(option) + " requires 1..1000000");
    return uint32_t(value);
}
}  // namespace
int main(int argc, const char** argv) {
    try {
        Options options;
        options.view.showUI = true;
        options.scene.assets = GVDB_SAMPLES_ASSETS_DIR;
        std::string sceneName = "teapot.scn";
        int frameOverride = -1;
        uint32_t width = 0, height = 0;
        for (int i = 1; i < argc; ++i) {
            const auto value = [&]() {
                if (++i >= argc)
                    throw std::invalid_argument("Missing point-cloud option value");
                return argv[i];
            };
            const auto is = [&](const char* a, const char* b) {
                return !std::strcmp(argv[i], a) || !std::strcmp(argv[i], b);
            };
            if (!std::strcmp(argv[i], "--help")) {
                std::puts(
                    "point-cloud [-dx11|-dx12|-vk|-cuda] [--renderer rt|compute]\n  [--in scene.scn] [--frame "
                    "N] [--scale S] [--key] [--info]\n  [--headless] [--frames N] [--samples N] [--screenshot "
                    "image.png]\n  [--width N] [--height N] [--points] [--topology] [--validation]\nDefault RT "
                    "requires D3D12, Vulkan RT or CUDA/OptiX. Compute renders the point volume only.\nKeys: 1 "
                    "points, 2 topology. Left orbit, middle pan, right distance, Shift controls "
                    "light.\n--samples freezes the dataset during capture; --frames counts rendered samples.");
                return 0;
            }
            if (!std::strcmp(argv[i], "--verify-volume"))
                options.verifyVolume = true;
            else if (!std::strcmp(argv[i], "--ray-policy-test"))
                options.rayPolicyTest = true;
            else if (!std::strcmp(argv[i], "--input-test"))
                options.inputTest = true;
            else if (is("--in", "-in"))
                sceneName = value();
            else if (is("--frame", "-frame"))
                frameOverride = std::stoi(value());
            else if (is("--scale", "-scale"))
                options.scene.scene.renderScale = std::stof(value());
            else if (is("--key", "-key"))
                options.scene.keyAnimation = true;
            else if (is("--info", "-info"))
                options.info = true;
            else if (!std::strcmp(argv[i], "--renderer")) {
                const std::string renderer = value();
                if (renderer != "rt" && renderer != "compute")
                    throw std::invalid_argument("Renderer must be rt or compute");
                options.scene.compute = renderer == "compute";
            } else if (!std::strcmp(argv[i], "--headless") || !std::strcmp(argv[i], "-cuda"))
                options.headless = true;
            else if (!std::strcmp(argv[i], "--cuda")) {
                options.headless = true;
                options.scene.compute = true;
            } else if (!std::strcmp(argv[i], "--frames")) {
                options.frames = positive(value(), "--frames");
                options.view.frameLimit = options.frames;
            } else if (!std::strcmp(argv[i], "--samples")) {
                options.scene.captureSamples = positive(value(), "--samples");
            } else if (!std::strcmp(argv[i], "--width"))
                width = positive(value(), "--width");
            else if (!std::strcmp(argv[i], "--height"))
                height = positive(value(), "--height");
            else if (!std::strcmp(argv[i], "--screenshot") || !std::strcmp(argv[i], "--output")) {
                options.output = value();
                options.view.screenshot = options.output;
            } else if (!std::strcmp(argv[i], "--points"))
                options.scene.points = true;
            else if (!std::strcmp(argv[i], "--topology"))
                options.view.topology = true;
        }
        std::filesystem::path scenePath = sceneName;
        if (!std::filesystem::exists(scenePath))
            scenePath = options.scene.assets / sceneName;
        if (!gvdb::samples::pointcloud::parseSceneFile(scenePath, options.scene.scene))
            throw std::runtime_error("Cannot load scene: " + scenePath.string());
        auto& scene = options.scene.scene;
        if (scene.renderScale == 0)
            scene.renderScale = 1;
        if (scene.frame < 0)
            scene.frame = 0;
        if (frameOverride >= 0)
            scene.frame = frameOverride;
        options.view.width = width ? width : uint32_t(std::max(64, scene.width));
        options.view.height = height ? height : uint32_t(std::max(64, scene.height));
        options.view.mode = gvdb::sample::VolumeViewMode::Surface;
        options.view.requestRayTracing = !options.scene.compute;
        if (options.scene.captureSamples) {
            options.frames = options.scene.captureSamples;
            options.view.frameLimit = options.frames;
        }
        if (!options.view.screenshot.empty() && !options.view.frameLimit)
            options.view.frameLimit = options.frames;
        // The legacy --cuda spelling explicitly requested the compute renderer.
        // -cuda now selects the unified CUDA backend and supports either renderer.
        std::vector<const char*> deviceArgs(argv, argv + argc);
        for (auto& argument : deviceArgs)
            if (!std::strcmp(argument, "--cuda"))
                argument = "-cuda";
        if (options.rayPolicyTest) {
            ethereal::slang::ExampleDevice device(argc, deviceArgs.data(), true);
            ethereal::slang::ComputeRuntime runtime(device.get(), device.shaderPath());
            gvdb::samples::pointcloud::runPointCloudRayPolicyTests(runtime);
            return 0;
        }
        if (options.inputTest) {
            ethereal::slang::ExampleDevice device(argc, deviceArgs.data());
            ethereal::slang::ComputeRuntime runtime(device.get(), device.shaderPath());
            gvdb::samples::pointcloud::runPointCloudAppTests(runtime);
            return 0;
        }
        if (options.headless) {
            ethereal::slang::ExampleDevice device(argc, deviceArgs.data(), !options.scene.compute);
            ethereal::slang::ComputeRuntime runtime(device.get(), device.shaderPath());
            gvdb::samples::pointcloud::PointCloudAppScene app(runtime, options.scene);
            gvdb::slang::RaycastOutput image;
            for (uint32_t frame = 0; frame < options.frames; ++frame) {
                app.animate(0, options.view);
                image = gvdb::sample::renderVolumeView(runtime, app, options.view);
                if (options.info)
                    std::puts(app.describe().c_str());
            }
            std::vector<gvdb::slang::Float4> colors(size_t(image.width) * image.height);
            std::vector<float> depth(colors.size());
            runtime.read(image.color, colors.data(), colors.size() * 16);
            runtime.read(image.depth, depth.data(), depth.size() * 4);
            size_t hits = 0;
            for (size_t i = 0; i < colors.size(); ++i) {
                const auto c = colors[i];
                if (!std::isfinite(c.x) || !std::isfinite(c.y) || !std::isfinite(c.z) || !std::isfinite(c.w) ||
                    std::isnan(depth[i]))
                    throw std::runtime_error("Point-cloud renderer returned nonfinite color or NaN depth");
                hits += std::isfinite(depth[i]) && depth[i] >= 0 && depth[i] != 1e10f;
            }
            if (!hits)
                throw std::runtime_error("Point-cloud renderer produced no finite scene hit");
            if (options.verifyVolume && !options.scene.compute)
                std::printf("Verified point-volume contribution: %u pixels\n", app.verifyVolumeContribution());
            gvdb::sample::writeVolumeImage(runtime, image, options.output);
            std::printf("PASS %zu scene pixels; %s\n", hits, app.describe().c_str());
            return 0;
        }
        return gvdb::sample::runVolumeWindow(
            argc, deviceArgs.data(), "GVDB Slang point cloud",
            [&](ethereal::slang::ComputeRuntime& runtime) {
                return std::make_unique<gvdb::samples::pointcloud::PointCloudAppScene>(runtime, options.scene);
            },
            options.view);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "point-cloud: %s\n", e.what());
        return 1;
    }
}
