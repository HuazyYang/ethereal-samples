// Fixed-topology spray deposition through portable Slang and NVRHI.
// SPDX-License-Identifier: Apache-2.0
#include "SprayScene.h"
#include "SprayAppTests.h"
#include "ExampleDevice.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
int main(int argc, const char** argv) {
    try {
        gvdb::samples::spray::SprayOptions scene;
        scene.asset = std::filesystem::path(GVDB_SAMPLES_ASSETS_DIR) / "metal.obj";
        gvdb::sample::VolumeViewOptions view;
        view.showUI = true;
        view.mode = gvdb::sample::VolumeViewMode::Volume;
        bool headless = false, selfTest = false;
        uint32_t frames = 1;
        std::filesystem::path output = "spray-deposit.png";
        for (int i = 1; i < argc; ++i) {
            const auto value = [&]() {
                if (++i >= argc)
                    throw std::invalid_argument("Missing spray option value");
                return argv[i];
            };
            if (!std::strcmp(argv[i], "--help")) {
                std::puts(
                    "spray-deposit [-dx11|-dx12|-vk|-cuda] [--headless] [--frames N] [--shade 0..4] [--wand "
                    "0..2]\n  [--paused] [--no-rays] [--topology] [--width N] [--height N] [--screenshot "
                    "image.png]\nKeys: 1/space simulate,2 topology,3 rays,4 shading,5 wand. Left orbit,middle "
                    "pan,right/wheel zoom,Shift light.");
                return 0;
            }
            if (!std::strcmp(argv[i], "--self-test")) {
                selfTest = true;
                headless = true;
            } else if (!std::strcmp(argv[i], "--headless") || !std::strcmp(argv[i], "-cuda"))
                headless = true;
            else if (!std::strcmp(argv[i], "--frames")) {
                frames = uint32_t(std::stoul(value()));
                if (!frames || frames > 1000000)
                    throw std::invalid_argument("Invalid frame count");
                view.frameLimit = frames;
            } else if (!std::strcmp(argv[i], "--width"))
                view.width = uint32_t(std::stoul(value()));
            else if (!std::strcmp(argv[i], "--height"))
                view.height = uint32_t(std::stoul(value()));
            else if (!std::strcmp(argv[i], "--screenshot") || !std::strcmp(argv[i], "--output")) {
                output = value();
                view.screenshot = output;
            } else if (!std::strcmp(argv[i], "--shade")) {
                const auto n = std::stoul(value());
                if (n > 4)
                    throw std::invalid_argument("Shading must be0..4");
                scene.shading = gvdb::samples::spray::Shading(n);
            } else if (!std::strcmp(argv[i], "--wand")) {
                const auto n = std::stoul(value());
                if (n > 2)
                    throw std::invalid_argument("Wand must be0..2");
                scene.wand = gvdb::samples::spray::WandStyle(n);
            } else if (!std::strcmp(argv[i], "--paused"))
                scene.simulate = false;
            else if (!std::strcmp(argv[i], "--no-rays"))
                scene.rays = false;
            else if (!std::strcmp(argv[i], "--topology"))
                view.topology = true;
        }
        if (!view.width || !view.height || view.width > 16384 || view.height > 16384)
            throw std::invalid_argument("Invalid image extent");
        if (!view.screenshot.empty() && !view.frameLimit)
            view.frameLimit = frames;
        if (headless) {
            ethereal::slang::ExampleDevice device(argc, argv);
            ethereal::slang::ComputeRuntime runtime(device.get(), device.shaderPath());
            if (selfTest) {
                gvdb::samples::spray::runSprayAppTests(runtime, scene.asset);
                return 0;
            }
            gvdb::samples::spray::SprayScene app(runtime, scene);
            gvdb::slang::RaycastOutput image;
            for (uint32_t frame = 0; frame < frames; ++frame) {
                app.animate(0, view);
                image = gvdb::sample::renderVolumeView(runtime, app, view);
            }
            std::vector<gvdb::slang::Float4> colors(size_t(image.width) * image.height);
            std::vector<float> depths(colors.size());
            runtime.read(image.color, colors.data(), colors.size() * 16);
            runtime.read(image.depth, depths.data(), depths.size() * 4);
            size_t hits = 0;
            for (size_t i = 0; i < colors.size(); ++i) {
                const auto c = colors[i];
                if (!std::isfinite(c.x) || !std::isfinite(c.y) || !std::isfinite(c.z) || !std::isfinite(c.w) ||
                    (std::isnan(depths[i]) || depths[i] < 0))
                    throw std::runtime_error("Spray rendering produced nonfinite output");
                hits += std::isfinite(depths[i]) && depths[i] >= 0;
            }
            if (scene.shading != gvdb::samples::spray::Shading::Off &&
                scene.shading != gvdb::samples::spray::Shading::Section && !hits)
                throw std::runtime_error("Spray rendering produced no visible volume");
            if (scene.simulate && !app.simulation().hitCount())
                throw std::runtime_error("Spray rays produced no metal hits");
            gvdb::sample::writeVolumeImage(runtime, image, output);
            std::printf("PASS %zu image hits; %s\n", hits, app.describe().c_str());
            return 0;
        }
        return gvdb::sample::runVolumeWindow(
            argc, argv, "GVDB Slang spray deposit",
            [&](ethereal::slang::ComputeRuntime& runtime) {
                return std::make_unique<gvdb::samples::spray::SprayScene>(runtime, scene);
            },
            view);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "spray-deposit: %s\n", e.what());
        return 1;
    }
}
