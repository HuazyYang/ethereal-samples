// Procedural city scanning and persistent Slang point fusion through NVRHI.
// SPDX-License-Identifier: Apache-2.0
#include "PointFusionScene.h"
#include "PointFusionTests.h"
#include "ExampleDevice.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
namespace {
struct Options {
    gvdb::samples::PointFusionSceneOptions scene;
    gvdb::sample::VolumeViewOptions view;
    uint32_t frames = 1;
    bool headless = false, selfTest = false, inputTest = false;
    std::filesystem::path output = "point-fusion.png";
};
}  // namespace
int main(int argc, const char** argv) {
    try {
        Options options;
        options.view.mode = gvdb::sample::VolumeViewMode::Surface;
        options.view.isovalue = 0;
        options.view.scalarBackground = 3;
        options.view.background = {.1f, .2f, .3f, 1};
        options.view.step = .5f;
        for (int i = 1; i < argc; ++i) {
            const auto value = [&]() {
                if (++i >= argc)
                    throw std::invalid_argument("Missing point-fusion option value");
                return argv[i];
            };
            if (!std::strcmp(argv[i], "--help")) {
                std::puts(
                    "point-fusion [-dx11|-dx12|-vk|-cuda] [--adapter N] [--validation]\n"
                    "  [--speed V] [--orbit] [--color] [--points] [--objects] [--topology]\n"
                    "  [--headless] [--frames N] [--output image.png] [--width N] [--height N]\n"
                    "  [--scan-width N] [--scan-height N] [--self-test]\n"
                    "Keys: 1 points, 2 topology, Space scan, F camera, O city, C color/reset, R reset.\n"
                    "FPV or Shift: left scanner angles, middle height, right speed. Orbit: left orbit, middle "
                    "pan, right distance.");
                return 0;
            }
            if (!std::strcmp(argv[i], "--speed"))
                options.scene.speed = std::stof(value());
            else if (!std::strcmp(argv[i], "--orbit"))
                options.scene.firstPerson = false;
            else if (!std::strcmp(argv[i], "--color"))
                options.scene.color = true;
            else if (!std::strcmp(argv[i], "--points"))
                options.scene.points = true;
            else if (!std::strcmp(argv[i], "--objects"))
                options.scene.objects = true;
            else if (!std::strcmp(argv[i], "--topology"))
                options.view.topology = true;
            else if (!std::strcmp(argv[i], "--headless") || !std::strcmp(argv[i], "-cuda"))
                options.headless = true;
            else if (!std::strcmp(argv[i], "--self-test"))
                options.selfTest = true;
            else if (!std::strcmp(argv[i], "--input-test"))
                options.inputTest = true;
            else if (!std::strcmp(argv[i], "--frames")) {
                options.frames = uint32_t(std::stoul(value()));
                options.view.frameLimit = options.frames;
            } else if (!std::strcmp(argv[i], "--width"))
                options.view.width = uint32_t(std::stoul(value()));
            else if (!std::strcmp(argv[i], "--height"))
                options.view.height = uint32_t(std::stoul(value()));
            else if (!std::strcmp(argv[i], "--scan-width"))
                options.scene.scanWidth = uint32_t(std::stoul(value()));
            else if (!std::strcmp(argv[i], "--scan-height"))
                options.scene.scanHeight = uint32_t(std::stoul(value()));
            else if (!std::strcmp(argv[i], "--output") || !std::strcmp(argv[i], "--screenshot")) {
                options.output = value();
                options.view.screenshot = options.output;
            }
        }
        if (!options.frames || options.frames > 10000)
            throw std::invalid_argument("Point fusion requires 1..10000 scans per headless run");
        if (!options.view.screenshot.empty() && !options.view.frameLimit)
            options.view.frameLimit = options.frames;
        if (options.selfTest || options.inputTest) {
            ethereal::slang::ExampleDevice device(argc, argv);
            ethereal::slang::ComputeRuntime runtime(device.get(), device.shaderPath());
            if (options.selfTest)
                gvdb::samples::runPointFusionTests(runtime);
            gvdb::samples::runPointFusionInputTests(runtime);
            return 0;
        }
        if (options.headless) {
            ethereal::slang::ExampleDevice device(argc, argv);
            ethereal::slang::ComputeRuntime runtime(device.get(), device.shaderPath());
            gvdb::samples::PointFusionScene scene(runtime, options.scene);
            for (uint32_t frame = 0; frame < options.frames; ++frame) {
                scene.animate(0, options.view);
                std::puts(scene.describe().c_str());
            }
            if (!scene.fusion().hasVolume())
                throw std::runtime_error("No valid scanner hit was fused");
            const auto output = gvdb::sample::renderVolumeView(runtime, scene, options.view);
            std::vector<gvdb::slang::Float4> pixels(size_t(output.width) * output.height);
            runtime.read(output.color, pixels.data(), pixels.size() * 16);
            std::vector<float> depth(pixels.size());
            runtime.read(output.depth, depth.data(), depth.size() * 4);
            size_t hits = 0;
            for (size_t i = 0; i < pixels.size(); ++i) {
                const auto& p = pixels[i];
                if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || !std::isfinite(p.w) ||
                    std::isnan(depth[i]))
                    throw std::runtime_error("Point-fusion render contains nonfinite color or NaN depth");
                if (std::isfinite(depth[i]))
                    ++hits;
            }
            if (!hits)
                throw std::runtime_error("Point-fusion render contains no finite surface hit");
            gvdb::sample::writeVolumeImage(runtime, output, options.output);
            std::printf("PASS %zu surface pixels; %llu accumulated scan hits\n", hits,
                        static_cast<unsigned long long>(scene.fusion().totalHits()));
            return 0;
        }
        return gvdb::sample::runVolumeWindow(
            argc, argv, "GVDB Slang point fusion",
            [&](ethereal::slang::ComputeRuntime& runtime) {
                return std::make_unique<gvdb::samples::PointFusionScene>(runtime, options.scene);
            },
            options.view);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "point-fusion: %s\n", error.what());
        return 1;
    }
}
