// Independent display fixtures for the portable volume viewer.
// SPDX-License-Identifier: Apache-2.0
#include "ExampleDevice.h"
#include "VolumeViewer.h"
#include "../gvdb_samples/gvdb/slang/GVDBTopologyBuilder.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
namespace g = gvdb::slang;
namespace v = gvdb::sample;
namespace es = ethereal::slang;
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
void expectNear(float got, float expected, float tolerance, const char* message) {
    if (!std::isfinite(got) || std::abs(got - expected) > tolerance) {
        std::fprintf(stderr, "%s: got %.9g expected %.9g\n", message, got, expected);
        throw std::runtime_error(message);
    }
}
class Fixture : public v::VolumeScene {
    g::ScalarVolumeData m_data;
    std::unique_ptr<g::SlangVolume> m_volume;

 public:
    std::vector<v::VolumeOverlayLine> lines;
    std::vector<v::VolumeOverlayPoint> points;
    std::vector<v::VolumeOverlayLine> overlayLines() const override { return lines; }
    std::vector<v::VolumeOverlayPoint> overlayPoints() const override { return points; }
    explicit Fixture(es::ComputeRuntime& runtime) {
        std::vector<float> values(64);
        for (unsigned z = 0; z < 4; ++z)
            for (unsigned y = 0; y < 4; ++y)
                for (unsigned x = 0; x < 4; ++x)
                    values[(z * 4 + y) * 4 + x] = .1f + .2f * y;
        g::TopologyBuildOptions options;
        options.logDimensions = {2, 2};
        std::string error;
        require(g::buildScalarVolume(values.data(), values.size(), {4, 4, 4, 0}, options, 0, 0, m_data, error),
                error.c_str());
        m_volume =
            std::make_unique<g::SlangVolume>(runtime, m_data.packed, m_data.atlas.data(), m_data.atlas.size());
    }
    g::SlangVolume& volume() override { return *m_volume; }
    const g::PackedVolume& packed() const override { return m_data.packed; }
    std::vector<g::Float4> transfer() const override {
        return {
            {0, 0,   0,    0},
            {1, .5f, .25f, 1}
        };
    }
};
std::vector<g::Float4> colors(es::ComputeRuntime& runtime, const g::RaycastOutput& image) {
    std::vector<g::Float4> result(size_t(image.width) * image.height);
    runtime.read(image.color, result.data(), result.size() * sizeof(result[0]));
    for (const auto& c : result)
        require(std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z) && std::isfinite(c.w),
                "Nonfinite viewer pixel");
    return result;
}
float denseSample(float x, float y, float z) {
    const float p[3] = {x - .5f, y - .5f, z - .5f};
    const int lower[3] = {int(std::floor(p[0])), int(std::floor(p[1])), int(std::floor(p[2]))};
    const float weight[3] = {p[0] - lower[0], p[1] - lower[1], p[2] - lower[2]};
    float result = 0;
    for (int corner = 0; corner < 8; ++corner) {
        int q[3];
        float w = 1;
        for (unsigned axis = 0; axis < 3; ++axis) {
            const bool upper = (corner & (1 << axis)) != 0;
            q[axis] = lower[axis] + int(upper);
            w *= upper ? weight[axis] : 1 - weight[axis];
        }
        if (q[0] >= 0 && q[0] < 4 && q[1] >= 0 && q[1] < 4 && q[2] >= 0 && q[2] < 4)
            result += (.1f + .2f * q[1]) * w;
    }
    return result;
}
}  // namespace
int main(int argc, const char** argv) {
    try {
        es::ExampleDevice device(argc, argv);
        es::ComputeRuntime runtime(device.get(), device.shaderPath());
        Fixture scene(runtime);
        v::VolumeViewOptions options;
        options.width = 17;
        options.height = 13;
        options.yaw = 0;
        options.pitch = 0;
        options.step = .25f;
        options.isovalue = .2f;
        options.mode = v::VolumeViewMode::Section2D;
        options.step = 1e-9f;
        auto output = v::renderVolumeView(runtime, scene, options);
        auto image = colors(runtime, output);
        for (unsigned y = 0; y < options.height; ++y)
            for (unsigned x = 0; x < options.width; ++x) {
                const float density =
                                denseSample((x + .5f) * 4 / options.width, 2, (y + .5f) * 4 / options.height),
                            value = density * density;
                const auto& c = image[y * options.width + x];
                expectNear(c.x, value, 5e-6f, "section scalar interpolation/alpha");
                expectNear(c.y, value * .5f, 5e-6f, "section green");
                expectNear(c.z, value * .25f, 5e-6f, "section blue");
                expectNear(c.w, 1, 1e-6f, "section alpha");
            }
        const size_t center = (options.height / 2) * options.width + options.width / 2;
        options.step = .25f;
        options.mode = v::VolumeViewMode::Voxel;
        output = v::renderVolumeView(runtime, scene, options);
        image = colors(runtime, output);
        expectNear(image[center].x, .5f * .15f, 2e-5f, "voxel face lighting");
        std::vector<float> depth(image.size());
        runtime.read(output.depth, depth.data(), depth.size() * 4);
        expectNear(depth[center], std::sqrt(12.f) * options.distanceScale - 2 + 1e-4f, 1e-3f,
                   "voxel first occupied cell depth");
        options.distanceScale = (8192.f - 2) / std::sqrt(12.f);
        output = v::renderVolumeView(runtime, scene, options);
        image = colors(runtime, output);
        runtime.read(output.depth, depth.data(), depth.size() * 4);
        expectNear(depth[center], 8188.f, .002f, "voxel DDA at distant exact negative-direction boundary");
        expectNear(image[center].x, .5f * .15f, 2e-5f, "distant voxel face");
        options.distanceScale = 2.8f;
        // Section plane passes through the center. Its .4 alpha blends the
        // transfer color with the behind-plane surface's .15 ambient term.
        options.mode = v::VolumeViewMode::Section;
        options.pitch = .5f;
        output = v::renderVolumeView(runtime, scene, options);
        image = colors(runtime, output);
        expectNear(image[center].x, .4f * .4f + .2f * .15f * .6f, 5e-5f, "section plane/surface composition");
        runtime.read(output.depth, depth.data(), depth.size() * 4);
        expectNear(depth[center], std::sqrt(12.f) * options.distanceScale, 1e-3f, "section plane depth");
        options.mode = v::VolumeViewMode::Volume;
        output = v::renderVolumeView(runtime, scene, options);
        const auto base = colors(runtime, output);
        options.sectionInset = true;
        output = v::renderVolumeView(runtime, scene, options);
        image = colors(runtime, output);
        const uint32_t iw = options.width / 4, ih = options.height / 4;
        for (unsigned y = 0; y < options.height; ++y)
            for (unsigned x = 0; x < options.width; ++x) {
                const auto& c = image[y * options.width + x];
                if (x < iw && y >= options.height - ih) {
                    const float d =
                        denseSample((x + .5f) * 4 / iw, 2, (y - (options.height - ih) + .5f) * 4 / ih);
                    expectNear(c.x, d * d, 5e-6f, "inset odd dimensions");
                } else
                    expectNear(c.x, base[y * options.width + x].x, 1e-6f, "inset preserves outside pixels");
            }
        options.sectionInset = false;
        options.topology = true;
        image = colors(runtime, v::renderVolumeView(runtime, scene, options));
        size_t changed = 0;
        for (size_t i = 0; i < image.size(); ++i)
            if (std::abs(image[i].x - base[i].x) + std::abs(image[i].y - base[i].y) > .05f)
                ++changed;
        require(changed > 0, "Topology overlay produced no projected edges");
        options.topology = false;
        options.mode = v::VolumeViewMode::Surface;
        image = colors(runtime, v::renderVolumeView(runtime, scene, options));
        require(image[center].w == 1, "Surface mode has no opaque center hit");
        if (device.get()->getGraphicsAPI() != nvrhi::GraphicsAPI::CUDA) {
            const auto pixels = v::renderVolumeOffscreen(
                argc, argv, [](es::ComputeRuntime& r) { return std::make_unique<Fixture>(r); }, options);
            require(pixels.size() == image.size() * 4, "Display size");
            for (size_t i = 0; i < image.size(); ++i) {
                const float channels[4] = {image[i].x, image[i].y, image[i].z, image[i].w};
                for (unsigned c = 0; c < 4; ++c) {
                    const int expected = int(std::clamp(channels[c], 0.f, 1.f) * 255 + .5f);
                    require(std::abs(int(pixels[i * 4 + c]) - expected) <= 1,
                            "Direct-buffer presenter changed pixel value or orientation");
                }
            }
        }
        options.mode = v::VolumeViewMode::Voxel;
        options.explicitCamera = true;
        options.cameraEye = {2, 2, 10, 0};
        options.cameraForward = {0, 0, -2, 0};  // Non-unit bases are accepted and normalized.
        options.cameraUp = {0, 3, 0, 0};
        options.horizontalFov = 90;
        options.cameraNear = 1;
        options.cameraFar = 20;
        output = v::renderVolumeView(runtime, scene, options);
        runtime.read(output.depth, depth.data(), depth.size() * 4);
        expectNear(depth[center], 6, 1e-5f, "explicit camera center depth");
        const auto unadorned = colors(runtime, output);
        scene.lines = {
            {{0, 2, 2, 0}, {4, 2, 2, 0}, {1, 0, 0, 1}}
        };
        scene.points = {
            {{2, 3, 2, 0},    {0, 1, 0, 1}},
            {{2, 2, 9.5f, 0}, {1, 1, 0, 1}}, // Before near plane.
            {{2, 2, -20, 0},  {1, 0, 1, 1}}
        };  // Beyond far plane.
        image = colors(runtime, v::renderVolumeView(runtime, scene, options));
        for (unsigned y = 0; y < options.height; ++y)
            for (unsigned x = 0; x < options.width; ++x) {
                const auto& got = image[y * options.width + x];
                const g::Float4 expected = y == 6 && x >= 6 && x <= 10 ? g::Float4{1, 0, 0, 1}
                                           : y == 5 && x == 8          ? g::Float4{0, 1, 0, 1}
                                                                       : unadorned[y * options.width + x];
                expectNear(got.x, expected.x, 1e-6f, "world overlay horizontal projection");
                expectNear(got.y, expected.y, 1e-6f, "world point projection and clipping");
                expectNear(got.z, expected.z, 1e-6f, "world overlay preserves other pixels");
            }
        scene.lines.clear();
        scene.points.clear();
        options.cameraFar = 5;
        output = v::renderVolumeView(runtime, scene, options);
        runtime.read(output.depth, depth.data(), depth.size() * 4);
        require(std::isinf(depth[center]), "explicit camera far clip failed");
        options.cameraFar = 20;
        options.cameraEye = {-2, 2, 10, 0};
        options.cameraFar = 7;
        output = v::renderVolumeView(runtime, scene, options);
        runtime.read(output.depth, depth.data(), depth.size() * 4);
        // Pixel14 points .70588 units right per forward unit. It enters the
        // front Z face at view depth6 but ray distance7.345; far7 must retain it.
        const float slope = 12.f / 17;
        expectNear(depth[6 * options.width + 14], 6 * std::sqrt(1 + slope * slope), 1e-4f,
                   "off-axis view-Z far plane");
        options.cameraEye = {2, 2, 10, 0};
        options.cameraFar = 20;
        // Very large, finite projected endpoints must never reach float-to-int
        // conversion as infinity or NaN during tile binning.
        scene.lines = {
            {{-1e38f, 2, 2, 0}, {1e38f, 2, 2, 0}, {1, 0, 0, 1}}
        };
        (void)colors(runtime, v::renderVolumeView(runtime, scene, options));
        scene.lines.clear();
        options.mode = v::VolumeViewMode::Section2D;
        const auto sectionBase = colors(runtime, v::renderVolumeView(runtime, scene, options));
        scene.points = {
            {{2, 123, 2, 0}, {0, 1, 0, 1}}
        };
        image = colors(runtime, v::renderVolumeView(runtime, scene, options));
        for (size_t i = 0; i < image.size(); ++i) {
            const auto expected = i == center ? g::Float4{0, 1, 0, 1} : sectionBase[i];
            expectNear(image[i].x, expected.x, 1e-6f, "orthographic section overlay X");
            expectNear(image[i].y, expected.y, 1e-6f, "orthographic section overlay Z");
        }
        scene.points.clear();
        options.cameraUp = {0, 0, 1, 0};
        bool rejected = false;
        try {
            (void)v::renderVolumeView(runtime, scene, options);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, "Parallel explicit camera basis was accepted");
        std::puts(
            "Volume view: section interpolation/composition, voxel DDA, odd inset, topology, surface and "
            "graphics presentation PASS");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "volume-view: %s\n", e.what());
        return 1;
    }
}
