// Original Lucy solid-voxelization and cross-section example on portable Slang.
// SPDX-License-Identifier: Apache-2.0
#include "PrintVolume.h"
#include "PrintTests.h"
#include "ExampleDevice.h"
#include "VolumeViewer.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <set>
#include <tuple>
namespace {
using ethereal::slang::ComputeRuntime;
struct Options {
    float voxelSize = .5f;
    bool headless = false, selfTest = false, verify = false;
    std::filesystem::path input = std::filesystem::path(GVDB_SAMPLES_ASSETS_DIR) / "lucy.obj",
                          output = "3d-print.png";
    gvdb::sample::VolumeViewOptions view;
};
class PrintScene final : public gvdb::sample::VolumeScene {
    gvdb::samples::PrintVolume m_pipeline;

 public:
    PrintScene(ComputeRuntime& runtime, const SampleUtils::TriangleMesh& mesh, float size)
        : m_pipeline(runtime, mesh, configuration(size)) {}
    static gvdb::samples::PrintConfiguration configuration(float size) {
        gvdb::samples::PrintConfiguration c;
        c.voxelSize = size;
        return c;
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
        return gvdb::samples::printTransferFunction();
    }
    bool key(int key, gvdb::sample::VolumeViewOptions& view) override {
        using Mode = gvdb::sample::VolumeViewMode;
        if (key == '2') {
            const Mode modes[] = {Mode::Voxel, Mode::Surface, Mode::Section, Mode::Volume};
            size_t next = 0;
            for (size_t i = 0; i < 4; ++i)
                if (view.mode == modes[i])
                    next = (i + 1) % 4;
            view.mode = modes[next];
            return true;
        }
        if (key == '3') {
            const float sizes[] = {.5f, .4f, .3f, .2f};
            size_t next = 0;
            for (size_t i = 0; i < 4; ++i)
                if (std::abs(m_pipeline.voxelSize() - sizes[i]) < 1e-6f)
                    next = (i + 1) % 4;
            m_pipeline.rebuild(sizes[next]);
            std::puts(describe().c_str());
            return true;
        }
        return false;
    }
    void verifyLucy() const {
        const auto snapshot = m_pipeline.snapshot()->getDesc();
        gvdb::portable::LevelInfo level;
        if (NVRHI_FAILED(m_pipeline.snapshot()->getLevel(0, level)))
            throw std::runtime_error("Print snapshot level");
        if (std::abs(m_pipeline.voxelSize() - .5f) > 1e-6f || level.nodeExtent.x != 32 ||
            snapshot.volume.atlasBrickCountAndWidth.w != 34 || m_pipeline.surfaceCount() != 56839 ||
            m_pipeline.interiorCount() != 131750)
            throw std::runtime_error(
                "Lucy 0.5 mm legacy configuration or geometric voxel classification changed");
        // Derive expected leaf membership independently from dense occupied voxels,
        // not from the generated GPU hierarchy or its leaf count.
        using Cell = std::tuple<int32_t, int32_t, int32_t>;
        std::set<Cell> expected, actual;
        const auto resolution = m_pipeline.resolution();
        const auto origin = m_pipeline.origin();
        const auto& dense = m_pipeline.dense();
        for (uint32_t z = 0; z < resolution.z; ++z)
            for (uint32_t y = 0; y < resolution.y; ++y)
                for (uint32_t x = 0; x < resolution.x; ++x)
                    if (dense[(size_t(z) * resolution.y + y) * resolution.x + x] != 0)
                        expected.emplace(origin.x + int32_t(x / 32) * 32, origin.y + int32_t(y / 32) * 32,
                                         origin.z + int32_t(z / 32) * 32);
        const auto& host = m_pipeline.topology();
        const auto pool = host.getPool(0, 0);
        if (pool.count && (!pool.data || pool.stride != sizeof(gvdb::portable::Node)))
            throw std::logic_error("Print leaf pool");
        for (uint64_t i = 0; i < pool.count; ++i) {
            gvdb::portable::Node node;
            std::memcpy(&node, static_cast<const uint8_t*>(pool.data) + i * pool.stride, sizeof(node));
            actual.emplace(node.position.x, node.position.y, node.position.z);
        }
        if (expected != actual)
            throw std::runtime_error("Lucy leaf32 topology differs from independently classified dense voxels");
        std::printf(
            "PASS Lucy 0.5 mm: %zu leaf32 bricks independently classified, 56839 surface / 131750 interior "
            "voxels\n",
            expected.size());
    }
    std::string describe() const override {
        return std::to_string(m_pipeline.voxelSize()) + " mm; " +
               std::to_string(m_pipeline.snapshot()->getDesc().activeLeafCount) + " bricks; " +
               std::to_string(m_pipeline.surfaceCount()) + " surface / " +
               std::to_string(m_pipeline.interiorCount()) + " interior voxels; 2 shading, 3 voxel size";
    }
};
gvdb::sample::VolumeViewMode mode(const char* value) {
    using M = gvdb::sample::VolumeViewMode;
    if (!std::strcmp(value, "voxel"))
        return M::Voxel;
    if (!std::strcmp(value, "surface"))
        return M::Surface;
    if (!std::strcmp(value, "section"))
        return M::Section;
    if (!std::strcmp(value, "volume"))
        return M::Volume;
    if (!std::strcmp(value, "section2d"))
        return M::Section2D;
    throw std::invalid_argument("Unknown print shading mode");
}
}  // namespace
int main(int argc, const char** argv) {
    try {
        Options options;
        options.view.mode = gvdb::sample::VolumeViewMode::Voxel;
        options.view.sectionInset = true;
        options.view.yaw = -.7853981634f;
        options.view.pitch = .5235987756f;
        options.view.extinction = 1.1f;
        options.view.isovalue = .25f;
        for (int i = 1; i < argc; ++i) {
            const auto value = [&]() {
                if (++i >= argc)
                    throw std::invalid_argument("Missing option value");
                return argv[i];
            };
            if (!std::strcmp(argv[i], "--help")) {
                std::puts(
                    "3d-print [-dx11|-dx12|-vk|-cuda] [--adapter N] [--validation]\n"
                    "  [--input lucy.obj] [--voxel-size 0.5|0.4|0.3|0.2] [--shading "
                    "voxel|surface|section|volume|section2d]\n"
                    "  [--section-height 0..1] [--no-inset] [--topology] [--headless] [--output image.png]\n"
                    "  [--width N] [--height N] [--self-test] [--verify] [--frames N]\n"
                    "Interactive: 1 topology, 2 shading, 3 voxel size; mouse height selects section. CUDA runs "
                    "headless.");
                return 0;
            }
            if (!std::strcmp(argv[i], "--input"))
                options.input = value();
            else if (!std::strcmp(argv[i], "--voxel-size"))
                options.voxelSize = std::stof(value());
            else if (!std::strcmp(argv[i], "--shading"))
                options.view.mode = mode(value());
            else if (!std::strcmp(argv[i], "--section-height"))
                options.view.sectionHeight = std::stof(value());
            else if (!std::strcmp(argv[i], "--frames")) {
                options.view.frameLimit = uint32_t(std::stoul(value()));
                if (!options.view.frameLimit || options.view.frameLimit > 10000)
                    throw std::invalid_argument("Frames must be in [1,10000]");
            } else if (!std::strcmp(argv[i], "--width"))
                options.view.width = uint32_t(std::stoul(value()));
            else if (!std::strcmp(argv[i], "--height"))
                options.view.height = uint32_t(std::stoul(value()));
            else if (!std::strcmp(argv[i], "--output") || !std::strcmp(argv[i], "--screenshot")) {
                options.output = value();
                options.headless = true;
            } else if (!std::strcmp(argv[i], "--headless") || !std::strcmp(argv[i], "-cuda"))
                options.headless = true;
            else if (!std::strcmp(argv[i], "--self-test"))
                options.selfTest = true;
            else if (!std::strcmp(argv[i], "--verify")) {
                options.verify = true;
                options.headless = true;
            } else if (!std::strcmp(argv[i], "--no-inset"))
                options.view.sectionInset = false;
            else if (!std::strcmp(argv[i], "--topology"))
                options.view.topology = true;
        }
        const float supportedSizes[] = {.5f, .4f, .3f, .2f};
        if (!std::isfinite(options.voxelSize) ||
            std::none_of(std::begin(supportedSizes), std::end(supportedSizes),
                         [&](float v) { return std::abs(v - options.voxelSize) < 1e-6f; }))
            throw std::invalid_argument("Voxel size must be 0.5,0.4,0.3 or0.2 mm");
        if (options.selfTest) {
            ethereal::slang::ExampleDevice device(argc, argv);
            ComputeRuntime runtime(device.get(), device.shaderPath());
            gvdb::samples::runPrintSelfTests(runtime);
            return 0;
        }
        const auto mesh = gvdb::samples::loadPrintMesh(options.input);
        if (options.headless) {
            ethereal::slang::ExampleDevice device(argc, argv);
            ComputeRuntime runtime(device.get(), device.shaderPath());
            PrintScene scene(runtime, mesh, options.voxelSize);
            std::puts(scene.describe().c_str());
            if (options.verify)
                scene.verifyLucy();
            const auto output = gvdb::sample::renderVolumeView(runtime, scene, options.view);
            std::vector<gvdb::slang::Float4> pixels(size_t(output.width) * output.height);
            runtime.read(output.color, pixels.data(), pixels.size() * sizeof(pixels[0]));
            size_t changed = 0;
            const uint32_t insetWidth = std::max(1u, output.width / 4),
                           insetHeight = std::max(1u, output.height / 4);
            for (uint32_t y = 0; y < output.height; ++y)
                for (uint32_t x = 0; x < output.width; ++x) {
                    const auto& p = pixels[size_t(y) * output.width + x];
                    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
                        !std::isfinite(p.w))
                        throw std::runtime_error("Print render produced nonfinite pixels");
                    const bool sectionOnly = options.view.mode == gvdb::sample::VolumeViewMode::Section2D;
                    if (options.view.sectionInset && !sectionOnly && x < insetWidth &&
                        y >= output.height - insetHeight)
                        continue;
                    changed += sectionOnly ? (std::abs(p.x) + std::abs(p.y) + std::abs(p.z) > .01f)
                                           : (std::abs(p.x - options.view.background.x) +
                                                  std::abs(p.y - options.view.background.y) +
                                                  std::abs(p.z - options.view.background.z) >
                                              .01f);
                }
            if (!changed)
                throw std::runtime_error("Print render contains no model contribution");
            gvdb::sample::writeVolumeImage(runtime, output, options.output);
            std::printf("%zu non-background pixels\n", changed);
            return 0;
        }
        return gvdb::sample::runVolumeWindow(
            argc, argv, "GVDB Slang 3D print",
            [&](ComputeRuntime& runtime) {
                return std::make_unique<PrintScene>(runtime, mesh, options.voxelSize);
            },
            options.view);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "3d-print: %s\n", error.what());
        return 1;
    }
}
