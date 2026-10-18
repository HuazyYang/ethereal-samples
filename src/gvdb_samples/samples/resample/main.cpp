// Original RAW resample example through NVRHI and offline Slang on every API.
// SPDX-License-Identifier: Apache-2.0
#include "ResampleVolume.h"
#include "ResampleTests.h"
#include "ExampleDevice.h"
#include "VolumeViewer.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
namespace {
using ethereal::slang::ComputeRuntime;
using gvdb::samples::ResampleVolume;
struct Options {
    bool sparse = false, halo = true, headless = false, selfTest = false, verify = false;
    std::filesystem::path input = std::filesystem::path(GVDB_SAMPLES_ASSETS_DIR) / "head.raw",
                          output = "resample.png";
    gvdb::sample::VolumeViewOptions view;
};
class ResampleScene final : public gvdb::sample::VolumeScene {
    ResampleVolume m_pipeline;

 public:
    ResampleScene(ComputeRuntime& runtime, const std::vector<float>& source, const Options& options)
        : m_pipeline(runtime, source) {
        if (options.verify)
            gvdb::samples::verifyResampleModes(
                m_pipeline, source,
                options.input == std::filesystem::path(GVDB_SAMPLES_ASSETS_DIR) / "head.raw");
        m_pipeline.rebuild(options.sparse, options.halo);
    }
    gvdb::portable::SnapshotHandle ownedSnapshot() const override { return m_pipeline.snapshot(); }
    bool topologyOverlay(std::vector<gvdb::sample::VolumeOverlayLine>& output) const override {
        const auto& host = m_pipeline.topology();
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
                throw std::logic_error("Resample node pool");
            if (pool.count > (1000000 - output.size()) / 12)
                throw std::invalid_argument("Resample topology overlay too large");
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
    std::vector<gvdb::slang::Float4> transfer() const override {
        return gvdb::samples::resampleTransferFunction();
    }
    bool key(int key) override {
        if (key == '2')
            m_pipeline.rebuild(!m_pipeline.sparse(), m_pipeline.halo());
        else if (key == '3')
            m_pipeline.rebuild(m_pipeline.sparse(), !m_pipeline.halo());
        else
            return false;
        std::puts(describe().c_str());
        return true;
    }
    std::string describe() const override {
        const auto snapshot = m_pipeline.snapshot()->getDesc();
        const auto r = snapshot.volume.atlasResolutionAndApron;
        return std::string(m_pipeline.sparse() ? "Sparse" : "Dense") +
               (m_pipeline.halo() ? "; halo on" : "; halo off") + "; " +
               std::to_string(snapshot.activeLeafCount) + " bricks; " +
               std::to_string(uint64_t(r.x) * r.y * r.z) + " atlas voxels; 2 sparse, 3 halo";
    }
};
}  // namespace
int main(int argc, const char** argv) {
    try {
        Options options;
        options.view.yaw = 1.57079632679f;
        options.view.pitch = .3490658504f;
        options.view.distanceScale = 4.5f;
        options.view.step = .5f;
        options.view.extinction = 1.5f;
        for (int i = 1; i < argc; ++i) {
            const auto argument = [&]() {
                if (++i >= argc)
                    throw std::invalid_argument("Missing option value");
                return argv[i];
            };
            if (!std::strcmp(argv[i], "--help")) {
                std::puts(
                    "resample [-dx11|-dx12|-vk|-cuda] [--adapter N] [--validation]\n"
                    "  [--input head.raw] [--dense|--sparse] [--halo|--no-halo] [--topology]\n"
                    "  [--headless] [--output image.png] [--width N] [--height N] [--step N]\n"
                    "  [--verify] [--self-test] [--frames N]\n"
                    "Interactive: 1 topology, 2 sparse, 3 halo; arrows/mouse orbit, wheel zoom.\n"
                    "--verify checks all three topology modes against CPU references. CUDA runs headless.");
                return 0;
            }
            if (!std::strcmp(argv[i], "--input"))
                options.input = argument();
            else if (!std::strcmp(argv[i], "--output") || !std::strcmp(argv[i], "--screenshot")) {
                options.output = argument();
                options.headless = true;
            } else if (!std::strcmp(argv[i], "--headless") || !std::strcmp(argv[i], "-cuda"))
                options.headless = true;
            else if (!std::strcmp(argv[i], "--sparse"))
                options.sparse = true;
            else if (!std::strcmp(argv[i], "--dense"))
                options.sparse = false;
            else if (!std::strcmp(argv[i], "--halo"))
                options.halo = true;
            else if (!std::strcmp(argv[i], "--no-halo"))
                options.halo = false;
            else if (!std::strcmp(argv[i], "--topology"))
                options.view.topology = true;
            else if (!std::strcmp(argv[i], "--frames")) {
                options.view.frameLimit = uint32_t(std::stoul(argument()));
                if (!options.view.frameLimit || options.view.frameLimit > 10000)
                    throw std::invalid_argument("Frames must be in [1,10000]");
            } else if (!std::strcmp(argv[i], "--width"))
                options.view.width = uint32_t(std::stoul(argument()));
            else if (!std::strcmp(argv[i], "--height"))
                options.view.height = uint32_t(std::stoul(argument()));
            else if (!std::strcmp(argv[i], "--step"))
                options.view.step = std::stof(argument());
            else if (!std::strcmp(argv[i], "--verify"))
                options.verify = true;
            else if (!std::strcmp(argv[i], "--self-test"))
                options.selfTest = true;
        }
        if (options.selfTest) {
            ethereal::slang::ExampleDevice device(argc, argv);
            ComputeRuntime runtime(device.get(), device.shaderPath());
            gvdb::samples::runResampleSelfTests(runtime);
            return 0;
        }
        const auto source = gvdb::samples::loadRawBytes(options.input, {128, 256, 256, 0});
        if (options.headless) {
            ethereal::slang::ExampleDevice device(argc, argv);
            ComputeRuntime runtime(device.get(), device.shaderPath());
            ResampleScene scene(runtime, source, options);
            std::puts(scene.describe().c_str());
            const auto output = gvdb::sample::renderVolumeView(runtime, scene, options.view);
            std::vector<gvdb::slang::Float4> pixels(size_t(output.width) * output.height);
            runtime.read(output.color, pixels.data(), pixels.size() * sizeof(pixels[0]));
            size_t changed = 0;
            for (const auto& pixel : pixels) {
                if (!std::isfinite(pixel.x) || !std::isfinite(pixel.y) || !std::isfinite(pixel.z) ||
                    !std::isfinite(pixel.w))
                    throw std::runtime_error("Resample render produced nonfinite pixels");
                if (std::abs(pixel.x - options.view.background.x) +
                        std::abs(pixel.y - options.view.background.y) +
                        std::abs(pixel.z - options.view.background.z) >
                    .01f)
                    ++changed;
            }
            if (!changed)
                throw std::runtime_error("Resample render contains no volume contribution");
            std::printf("%zu non-background pixels\n", changed);
            gvdb::sample::writeVolumeImage(runtime, output, options.output);
            return 0;
        }
        return gvdb::sample::runVolumeWindow(
            argc, argv, "GVDB Slang resample",
            [&](ComputeRuntime& runtime) { return std::make_unique<ResampleScene>(runtime, source, options); },
            options.view);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "resample: %s\n", error.what());
        return 1;
    }
}
