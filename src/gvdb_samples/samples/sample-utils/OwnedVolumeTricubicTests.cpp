// SPDX-License-Identifier: Apache-2.0
#include "OwnedVolumeTricubic.h"
#include "OwnedVolumeIO.h"
#include "ExampleDevice.h"
#include <gvdb/slang/GVDBPortableVbx.h>
#include <gvdb/slang/GVDBTopologyBuilder.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <fstream>
namespace g = gvdb::slang;
namespace p = gvdb::portable;
namespace s = gvdb::sample;
namespace es = ethereal::slang;
namespace {
void require(bool v, const char* text) {
    if (!v)
        throw std::runtime_error(text);
}
void nearValue(float a, double b, const char* text) {
    if (!std::isfinite(a) || std::abs(double(a) - b) > 3e-4 * std::max(1., std::abs(b))) {
        std::fprintf(stderr, "%s: %.9g vs %.12g\n", text, a, b);
        throw std::runtime_error(text);
    }
}
struct Fixture {
    g::VbxRawVolume raw;
    p::SnapshotHandle snapshot;
    std::vector<float> scalar;
    g::Float4 offset{};
    g::Int4 origin{};
    Fixture(es::ComputeRuntime& runtime, const std::filesystem::path& remap, g::Int4 shift, unsigned mode,
            unsigned apron = 3)
        : origin(shift) {
        g::TopologyBuildOptions o;
        o.logDimensions = mode >= 3 ? std::vector<uint32_t>{3, 2} : std::vector<uint32_t>{3};
        o.apron = apron;
        o.atlasGrid = {2, 1, 1, 0};
        std::string error;
        std::vector<g::Int4> leaves{shift};
        if (mode >= 3)
            leaves.push_back({shift.x + 16, shift.y, shift.z, 0});
        require(g::buildTopology(leaves, o, raw.packed, error), error.c_str());
        const auto dims = raw.packed.volume.atlasResolutionAndApron;
        offset = {float(apron), float(apron), float(apron), 0};
        scalar.resize(size_t(dims.x) * dims.y * dims.z);
        for (uint32_t z = 0; z < dims.z; ++z)
            for (uint32_t y = 0; y < dims.y; ++y)
                for (uint32_t x = 0; x < dims.x; ++x) {
                    double a = double(x) - apron + .5, b = double(y) - apron + .5, c = double(z) - apron + .5;
                    scalar[(size_t(z) * dims.y + y) * dims.x + x] =
                        mode == 0                        ? float(.1 * a)
                        : mode == 1                      ? float(.03 * a * a + .013 * b * c + .001 * c * c * c)
                        : mode == 3 && x < 8 + 2 * apron ? float(.8 - .1 * a)
                                                         : 1.f;
                }
        raw.channels.resize(2);
        raw.channels[0].format = g::AtlasFormat::R32_FLOAT;
        raw.channels[0].bytes.resize(scalar.size() * 4);
        std::memcpy(raw.channels[0].bytes.data(), scalar.data(), scalar.size() * 4);
        raw.channels[1].format = g::AtlasFormat::RGBA8_UINT;
        raw.channels[1].bytes.resize(scalar.size() * 4);
        const uint32_t rgba = 0xffc08040;
        for (size_t i = 0; i < scalar.size(); ++i)
            std::memcpy(raw.channels[1].bytes.data() + i * 4, &rgba, 4);
        std::vector<uint8_t> encoded;
        require(g::saveRawVBX(raw, encoded, error), error.c_str());
        runtime.flush();
        snapshot = s::loadOwnedVBX(runtime.device(), remap, encoded.data(), encoded.size());
    }
    double reference(g::Float4 position, bool border = false) const {
        // Original 27 integer-position linear samples in double precision;
        // independent from the shader's combined 64-tap stencil.
        const double pos[] = {position.x, position.y, position.z}, off[] = {offset.x, offset.y, offset.z};
        double weights[3][3];
        int q[3];
        for (unsigned a = 0; a < 3; ++a) {
            double t = (pos[a] - std::floor(pos[a])) * .5 + .25;
            weights[a][0] = (1 - t) * (1 - t);
            weights[a][1] = 2 * t * (1 - t);
            weights[a][2] = t * t;
            q[a] = int(std::floor(pos[a] + off[a])) - 1;
        }
        const auto d = raw.packed.volume.atlasResolutionAndApron;
        double value = 0;
        for (int z = 0; z < 3; ++z)
            for (int y = 0; y < 3; ++y)
                for (int x = 0; x < 3; ++x) {
                    double linear = 0;
                    for (int corner = 0; corner < 8; ++corner) {
                        int ix = q[0] + x - 1 + (corner & 1), iy = q[1] + y - 1 + ((corner >> 1) & 1),
                            iz = q[2] + z - 1 + (corner >> 2);
                        if (border &&
                            (ix < 0 || iy < 0 || iz < 0 || ix >= int(d.x) || iy >= int(d.y) || iz >= int(d.z)))
                            continue;
                        ix = std::clamp(ix, 0, int(d.x) - 1);
                        iy = std::clamp(iy, 0, int(d.y) - 1);
                        iz = std::clamp(iz, 0, int(d.z) - 1);
                        linear += double(scalar[(size_t(iz) * d.y + iy) * d.x + ix]) * .125;
                    }
                    value += weights[0][x] * weights[1][y] * weights[2][z] * linear;
                }
        return value;
    }
    g::RaycastParameters camera() const {
        g::RaycastParameters q{};
        q.image = {1, 1, 0, 256};
        q.rayOrigin = {float(origin.x) - 2, float(origin.y) + 4, float(origin.z) + 4, 0};
        q.direction00 = {1, 0, 0, 0};
        q.march = {.25f, 0, 32, 1};
        q.valueRange = {0, 1, .3f, 1};
        q.background = {.03f, .07f, .11f, 1};
        q.light = {float(origin.x) - 10, float(origin.y) + 4, float(origin.z) + 4, 0};
        return q;
    }
};
struct Pixel {
    g::Float4 color;
    float depth;
};
Pixel render(es::ComputeRuntime& runtime, Fixture& f, g::RaycastParameters q, s::TricubicOptions options = {},
             bool color = false) {
    auto output = s::renderOwnedTricubic(runtime, f.snapshot, 0, color ? 1 : UINT32_MAX, q, options);
    Pixel value;
    runtime.read(output.color, &value.color, 16);
    runtime.read(output.depth, &value.depth, 4);
    return value;
}
void probes(es::ComputeRuntime& runtime, Fixture& f, bool border = false) {
    std::vector<g::Float4> points;
    for (unsigned i = 0; i < 257; ++i)
        points.push_back({float(int(i % 17) - 5) * .713f, float(int(i % 13) - 3) * .819f,
                          float(int(i % 11) - 2) * 1.127f, 0});
    struct Parameters {
        g::RaycastParameters camera;
        g::UInt4 counts;
        g::Float4 sampling, options;
    } q{};
    q.camera.image.x = uint32_t(points.size());
    q.camera.rayOrigin = f.offset;
    q.camera.volume = f.raw.packed.volume;
    q.options.w = border ? 1.f : 0.f;
    p::ChannelSnapshotDesc channel;
    require(NVRHI_SUCCEEDED(f.snapshot->getChannel(0, channel)), "probe scalar");
    auto input = runtime.buffer(points.size() * 16, 16, points.data()),
         output = runtime.buffer(points.size() * 16, 16);
    using B = nvrhi::BindingSetItem;
    runtime.dispatch("OwnedVolumeTricubicProbe", "probeTricubic", &q, sizeof(q),
                     {B::RawBuffer_SRV(0, channel.raw.buffer, {channel.raw.byteOffset, channel.raw.byteSize}),
                      B::StructuredBuffer_SRV(6, input), B::StructuredBuffer_UAV(2, output)},
                     (uint32_t(points.size()) + 63) / 64);
    std::vector<g::Float4> values(points.size());
    runtime.read(output, values.data(), values.size() * 16);
    bool nonlinear = false;
    for (size_t i = 0; i < points.size(); ++i) {
        nearValue(values[i].x, f.reference(points[i], border), "27-filtered-sample independent scalar");
        for (unsigned axis = 0; axis < 3; ++axis) {
            auto lo = points[i], hi = lo;
            if (axis == 0) {
                lo.x -= .5f;
                hi.x += .5f;
            } else if (axis == 1) {
                lo.y -= .5f;
                hi.y += .5f;
            } else {
                lo.z -= .5f;
                hi.z += .5f;
            }
            nearValue((axis == 0   ? values[i].y
                       : axis == 1 ? values[i].z
                                   : values[i].w),
                      f.reference(lo, border) - f.reference(hi, border), "independent tricubic gradient");
        }
        nonlinear |= std::abs(values[i].x - .1f * points[i].x) > 1e-3f;
    }
    require(nonlinear, "fixture distinguishes naive linear interpolation");
}
void empty(es::ComputeRuntime& runtime, const std::filesystem::path& remap) {
    std::ifstream file(remap, std::ios::binary);
    std::vector<char> code{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    p::VolumeCreateDesc d;
    d.device = runtime.device();
    d.configuration.levelCount = 1;
    d.configuration.logDimension = {1};
    d.remapArtifact = code.data();
    d.remapArtifactBytes = code.size();
    p::VolumeHandle owner;
    require(NVRHI_SUCCEEDED(p::createVolume(d, &owner)), "empty owner");
    require(NVRHI_SUCCEEDED(owner->setChannel(0, {p::AtlasFormat::R32_FLOAT, {}})), "empty scalar");
    require(NVRHI_SUCCEEDED(owner->finishTopology()), "empty topology");
    p::GenerationTicketHandle ticket;
    require(NVRHI_SUCCEEDED(owner->prepareGeneration({}, &ticket)), "empty generation");
    runtime.flush();
    nvrhi::CommandListHandle commands;
    require(NVRHI_SUCCEEDED(runtime.device()->createCommandList({}, &commands)), "empty command list");
    commands->open();
    require(NVRHI_SUCCEEDED(owner->recordAndPublish({runtime.device(), commands}, ticket)),
            "empty publication");
    commands->close();
    runtime.device()->executeCommandList(commands);
    require(runtime.device()->waitForIdle(), "empty completion");
    p::SnapshotHandle snapshot;
    require(NVRHI_SUCCEEDED(owner->getSnapshot(&snapshot)), "empty snapshot");
    owner = nullptr;
    ticket = nullptr;
    g::RaycastParameters q{};
    q.image = {1, 1, 0, 1};
    q.direction00 = {1, 0, 0, 0};
    q.march = {1, 0, 10, 1};
    q.valueRange = {0, 1, .3f, 1};
    q.background = {.1f, .2f, .3f, 1};
    auto result = s::renderOwnedTricubic(runtime, snapshot, 0, UINT32_MAX, q);
    snapshot = nullptr;
    Pixel pixel;
    runtime.read(result.color, &pixel.color, 16);
    runtime.read(result.depth, &pixel.depth, 4);
    require(std::isinf(pixel.depth) && pixel.depth > 0, "empty miss depth");
    nearValue(pixel.color.x, .1, "empty background");
}
void run(es::ComputeRuntime& runtime, const std::filesystem::path& remap) {
    for (auto shift : {
             g::Int4{0,   0, 0,  0},
             g::Int4{-16, 8, -8, 0}
    }) {
        Fixture plane(runtime, remap, shift, 0);
        auto q = plane.camera();
        s::TricubicOptions options;
        options.shadowAmount = 0;
        auto result = render(runtime, plane, q, options);
        nearValue(result.depth, 5.5, "refined linear density hit");
        nearValue(result.color.x, 1, "negative gradient diffuse");
        result = render(runtime, plane, q, options, true);
        nearValue(result.color.x, 64. / 255, "packed red");
        nearValue(result.color.y, 128. / 255, "packed green");
        nearValue(result.color.z, 192. / 255, "packed blue");
        q.light.x = float(shift.x) + 15;
        result = render(runtime, plane, q, options);
        nearValue(result.color.x, .1, "back-facing ambient");
        q = plane.camera();
        options.shadowAmount = .8f;
        result = render(runtime, plane, q, options);
        nearValue(result.color.x, 1, "unoccluded trilinear shadow");
        q.march.y = 5.6f;
        result = render(runtime, plane, q, options);
        require(result.depth >= 5.6f, "refinement respects near clip");
        q = plane.camera();
        q.march.z = 4;
        result = render(runtime, plane, q, options);
        require(std::isinf(result.depth) && result.depth > 0, "far clip miss");
        q = plane.camera();
        options.maxLeafSteps = 1;
        result = render(runtime, plane, q, options);
        require(result.depth < 0, "explicit sample exhaustion");
        options = {};
        options.shadowAmount = 0;
        q.direction00 = {0, 0, 0, 0};
        result = render(runtime, plane, q, options);
        require(std::isinf(result.depth), "zero ray miss");
        Fixture constant(runtime, remap, shift, 2);
        q = constant.camera();
        result = render(runtime, constant, q, options);
        nearValue(result.depth, 2.01, "zero-denominator unrefined hit");
        nearValue(result.color.x, 1, "constant field fallback normal");
        Fixture separated(runtime, remap, shift, 4);
        q = separated.camera();
        q.rayOrigin.x = float(shift.x) + 10;
        result = render(runtime, separated, q, options);
        nearValue(result.depth, 6.01, "gap traversal to second leaf");
        options.maxLeafVisits = 1;
        result = render(runtime, separated, q, options);
        require(result.depth < 0, "gap visit exhaustion");
        options = {};
        options.shadowAmount = 0;
        Fixture occluded(runtime, remap, shift, 3);
        q = occluded.camera();
        q.rayOrigin.x = float(shift.x) + 7;
        q.direction00 = {-1, 0, 0, 0};
        q.light.x = float(shift.x) + 30;
        result = render(runtime, occluded, q, options);
        nearValue(result.depth, 1.5, "reversed plane refinement");
        nearValue(result.color.x, 1, "unshadowed reverse plane");
        options.shadowAmount = .8f;
        result = render(runtime, occluded, q, options);
        nearValue(result.color.x, .28, "trilinear secondary-leaf shadow");
        options.shadowAmount = 0;
        Fixture curved(runtime, remap, shift, 1);
        probes(runtime, curved);
        Fixture clamped(runtime, remap, shift, 1, 0);
        probes(runtime, clamped);
        probes(runtime, clamped, true);
        q = plane.camera();
        auto reject = [&](s::TricubicOptions o, g::RaycastParameters camera, uint32_t channel, uint32_t color) {
            bool threw = false;
            try {
                s::renderOwnedTricubic(runtime, plane.snapshot, channel, color, camera, o);
            } catch (const std::invalid_argument&) {
                threw = true;
            }
            require(threw, "reject invalid renderer request");
        };
        for (unsigned i = 0; i < 8; ++i) {
            auto bad = options;
            switch (i) {
                case 0:
                    bad.fineStep = 0;
                    break;
                case 1:
                    bad.epsilon = -1;
                    break;
                case 2:
                    bad.shadowAmount = 2;
                    break;
                case 3:
                    bad.shadowBias = -1;
                    break;
                case 4:
                    bad.maxLeafSteps = 0;
                    break;
                case 5:
                    bad.maxLeafVisits = 0;
                    break;
                case 6:
                    bad.fineStep = NAN;
                    break;
                case 7:
                    bad.fineStep = 1e7f;
                    break;
            }
            reject(bad, q, 0, UINT32_MAX);
        }
        auto bad = q;
        bad.direction00.w = 1;
        reject(options, bad, 0, UINT32_MAX);
        reject(options, q, 1, UINT32_MAX);
        reject(options, q, 0, 0);
    }
    std::puts(
        "PASS tricubic retained 27-filtered-sample scalar/gradient references, signed roots, clamp/apron, "
        "plane refinement, "
        "color/shading, zero-denominator repair, clipping, exhaustion and invalid inputs");
}
}  // namespace
int main(int argc, const char** argv) {
    try {
        es::ExampleDevice device(argc, argv);
        auto path = donut::app::GetDirectoryWithExecutable() / "shaders/gvdb_owned" /
                    donut::app::GetShaderTypeName(device.get()->getGraphicsAPI());
        es::ComputeRuntime runtime(device.get(), path);
        run(runtime, path / "GVDBAtlasRepack_relocateAtlas.bin");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
