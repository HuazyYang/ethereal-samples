// Portable FLUIDS-v3 SPH, sparse reconstruction, and mixed Slang ray tracing.
// SPDX-License-Identifier: Apache-2.0
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "ExampleDevice.h"
#include "FluidScene.h"
namespace {
uint32_t number(const char* text, uint32_t limit, const char* name) {
    std::string s(text);
    size_t consumed = 0;
    const auto n = std::stoull(s, &consumed);
    if (consumed != s.size() || !n || n > limit || s.front() == '-')
        throw std::invalid_argument(std::string(name) + " is out of range");
    return uint32_t(n);
}
}  // namespace
int main(int argc, const char** argv) {
    try {
        gvdb::samples::fluid::AppOptions options;
        options.assets = GVDB_SAMPLES_ASSETS_DIR;
        gvdb::sample::VolumeViewOptions view;
        view.showUI = true;
        bool headless = false, test = false, topologyTest = false, verify = false, particleOverride = false;
        uint32_t frames = 1, samples = 8;
        std::filesystem::path output = "fluid-surface.png";
        for (int i = 1; i < argc; ++i) {
            const auto value = [&]() {
                if (++i >= argc) throw std::invalid_argument("Missing fluid option value");
                return argv[i];
            };
            if (!std::strcmp(argv[i], "--help")) {
                std::puts(
                    "fluid-surface [-dx11|-dx12|-vk|-cuda] [--renderer rt|compute]\n"
                    "  [--headless] [--frames N] [--samples M] [--screenshot image.png]\n"
                    "  [--particles 1..1500000] [--width N] [--height N] [--paused]\n"
                    "  [--shading off|levelset|trilinear|volume|emptyskip] [--no-color]\n"
                    "RT defaults to the original 1.5M-particle scene; compute explicitly renders only the "
                    "level set.\n"
                    "Capture simulates N steps then accumulates M samples of the final state.\n"
                    "Space simulate; 1 topology; 2 velocity lines; 3 shading; 4 color; R reset; ` UI.\n"
                    "Mouse: left orbit, middle pan, right distance, Shift light. --test-scene selects a "
                    "compact fixture.");
                return 0;
            }
            if (!std::strcmp(argv[i], "--renderer")) {
                const std::string s = value();
                if (s != "rt" && s != "compute")
                    throw std::invalid_argument("Renderer must be rt or compute");
                options.compute = s == "compute";
            } else if (!std::strcmp(argv[i], "--frames")) {
                frames = number(value(), 1000000, "--frames");
                view.frameLimit = frames;
            } else if (!std::strcmp(argv[i], "--samples"))
                samples = number(value(), 1000000, "--samples");
            else if (!std::strcmp(argv[i], "--particles")) {
                options.particles = number(value(), 1500000, "--particles");
                particleOverride = true;
            } else if (!std::strcmp(argv[i], "--width"))
                view.width = number(value(), 16384, "--width");
            else if (!std::strcmp(argv[i], "--height"))
                view.height = number(value(), 16384, "--height");
            else if (!std::strcmp(argv[i], "--headless") || !std::strcmp(argv[i], "-cuda"))
                headless = true;
            else if (!std::strcmp(argv[i], "--topology-test"))
                topologyTest = true;
            else if (!std::strcmp(argv[i], "--self-test"))
                test = true;
            else if (!std::strcmp(argv[i], "--verify-volume"))
                verify = true;
            else if (!std::strcmp(argv[i], "--test-scene"))
                options.compact = true;
            else if (!std::strcmp(argv[i], "--paused"))
                options.simulate = false;
            else if (!std::strcmp(argv[i], "--no-color"))
                options.color = false;
            else if (!std::strcmp(argv[i], "--assets"))
                options.assets = value();
            else if (!std::strcmp(argv[i], "--screenshot") || !std::strcmp(argv[i], "--output")) {
                output = value();
                view.screenshot = output;
            } else if (!std::strcmp(argv[i], "--shading")) {
                const std::string s = value();
                const char* names[] = {"off", "levelset", "trilinear", "volume", "emptyskip"};
                bool found = false;
                for (int j = 0; j < 5; ++j)
                    if (s == names[j]) {
                        options.shading = j;
                        found = true;
                    }
                if (!found) throw std::invalid_argument("Unknown fluid shading mode");
            }
        }
        if (topologyTest) {
            gvdb::samples::fluid::runFluidTopologyTests();
            return 0;
        }
        if (options.compact && !particleOverride) options.particles = 4096;
        view.requestRayTracing = !options.compute;
        if (test) {
            ethereal::slang::ExampleDevice device(argc, argv, !options.compute);
            ethereal::slang::ComputeRuntime runtime(device.get(), device.shaderPath());
            gvdb::samples::fluid::runFluidTests(runtime, options.assets, !options.compute);
            return 0;
        }
        if (headless || view.frameLimit || !view.screenshot.empty()) {
            options.captureSteps = frames;
            view.frameLimit = frames + samples - 1;
        }
        if (headless) {
            ethereal::slang::ExampleDevice device(argc, argv, !options.compute);
            ethereal::slang::ComputeRuntime runtime(device.get(), device.shaderPath());
            gvdb::samples::fluid::FluidScene scene(runtime, options);
            gvdb::slang::RaycastOutput image;
            for (uint32_t n = 0; n < view.frameLimit; ++n) {
                scene.animate(0, view);
                image = gvdb::sample::renderVolumeView(runtime, scene, view);
            }
            std::vector<gvdb::slang::Float4> colors(size_t(image.width) * image.height);
            std::vector<float> depth(colors.size());
            runtime.read(image.color, colors.data(), colors.size() * 16);
            runtime.read(image.depth, depth.data(), depth.size() * 4);
            uint32_t hits = 0;
            for (size_t i = 0; i < colors.size(); ++i) {
                const auto c = colors[i];
                if (!std::isfinite(c.x) || !std::isfinite(c.y) || !std::isfinite(c.z) ||
                    !std::isfinite(c.w) || std::isnan(depth[i]))
                    throw std::runtime_error("Fluid renderer produced nonfinite output");
                hits += depth[i] >= 0 && depth[i] < view.cameraFar;
            }
            if (options.shading && !hits) throw std::runtime_error("Fluid capture contains no scene hit");
            if (scene.frame() != (options.simulate ? frames : 0))
                throw std::runtime_error("Fluid capture simulation step count is incorrect");
            if (verify && !options.compute)
                std::printf("Verified fluid-volume contribution: %u pixels\n",
                            scene.verifyVolumeContribution());
            gvdb::sample::writeVolumeImage(runtime, image, output);
            std::printf("PASS %u scene pixels; %s\n", hits, scene.describe().c_str());
            return 0;
        }
        return gvdb::sample::runVolumeWindow(
            argc, argv, "GVDB Slang fluid surface",
            [&](ethereal::slang::ComputeRuntime& runtime) {
                return std::make_unique<gvdb::samples::fluid::FluidScene>(runtime, options);
            },
            view);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fluid-surface: %s\n", e.what());
        return 1;
    }
}
