// SPDX-License-Identifier: Apache-2.0
#include "ResampleTests.h"
#include "ComputeRuntime.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
namespace gvdb::samples {
using namespace slang;
namespace {
struct PrefixVolume {
    UInt4 resolution;
    std::vector<double> sums;
    PrefixVolume(const std::vector<float>& values, UInt4 r)
        : resolution{r.x + 1, r.y + 1, r.z + 1, 0}, sums(size_t(r.x + 1) * (r.y + 1) * (r.z + 1), 0) {
        for (uint32_t z = 1; z <= r.z; ++z)
            for (uint32_t y = 1; y <= r.y; ++y)
                for (uint32_t x = 1; x <= r.x; ++x)
                    at(x, y, z) = values[(size_t(z - 1) * r.y + y - 1) * r.x + x - 1] + at(x - 1, y, z) +
                                  at(x, y - 1, z) + at(x, y, z - 1) - at(x - 1, y - 1, z) -
                                  at(x - 1, y, z - 1) - at(x, y - 1, z - 1) + at(x - 1, y - 1, z - 1);
    }
    double& at(uint32_t x, uint32_t y, uint32_t z) {
        return sums[(size_t(z) * resolution.y + y) * resolution.x + x];
    }
    double at(uint32_t x, uint32_t y, uint32_t z) const {
        return sums[(size_t(z) * resolution.y + y) * resolution.x + x];
    }
    double box(const uint32_t low[3], const uint32_t high[3]) const {
        double value = 0;
        for (uint32_t mask = 0; mask < 8; ++mask) {
            const uint32_t x = (mask & 1) ? low[0] : high[0] + 1, y = (mask & 2) ? low[1] : high[1] + 1,
                           z = (mask & 4) ? low[2] : high[2] + 1;
            const bool odd = ((mask & 1) != 0) ^ ((mask & 2) != 0) ^ ((mask & 4) != 0);
            value += (odd ? -1 : 1) * at(x, y, z);
        }
        return value;
    }
};
Float4 transformed(Float4 p, const std::array<Float4, 3>& rows) {
    float value[3];
    for (size_t i = 0; i < 3; ++i)
        value[i] = rows[i].x * p.x + rows[i].y * p.y + rows[i].z * p.z + rows[i].w;
    return {value[0], value[1], value[2], 0};
}
std::vector<float> coarseReference(const std::vector<float>& values, const ResampleConfiguration& c,
                                   UInt4 resolution) {
    PrefixVolume prefix(values, c.sourceResolution);
    const auto extent = c.destinationResolution, source = c.sourceResolution;
    std::vector<float> output(size_t(resolution.x) * resolution.y * resolution.z);
    for (uint32_t z = 0; z < resolution.z; ++z)
        for (uint32_t y = 0; y < resolution.y; ++y)
            for (uint32_t x = 0; x < resolution.x; ++x) {
                const Float4 lower = transformed(
                    {float(x) * extent.x / (resolution.x + 1), float(y) * extent.y / (resolution.y + 1),
                     float(z) * extent.z / (resolution.z + 1), 0},
                    c.transform);
                const Float4 upper = transformed({float(x + 1) * extent.x / (resolution.x + 1) - 1,
                                                  float(y + 1) * extent.y / (resolution.y + 1) - 1,
                                                  float(z + 1) * extent.z / (resolution.z + 1) - 1, 0},
                                                 c.transform);
                const uint32_t lo[] = {uint32_t(std::clamp(int(lower.x), 0, int(source.x) - 1)),
                                       uint32_t(std::clamp(int(lower.y), 0, int(source.y) - 1)),
                                       uint32_t(std::clamp(int(lower.z), 0, int(source.z) - 1))};
                const uint32_t hi[] = {uint32_t(std::clamp(int(upper.x), int(lo[0]), int(source.x) - 1)),
                                       uint32_t(std::clamp(int(upper.y), int(lo[1]), int(source.y) - 1)),
                                       uint32_t(std::clamp(int(upper.z), int(lo[2]), int(source.z) - 1))};
                const uint64_t count = uint64_t(hi[0] - lo[0] + 1) * (hi[1] - lo[1] + 1) * (hi[2] - lo[2] + 1);
                output[(size_t(z) * resolution.y + y) * resolution.x + x] = float(prefix.box(lo, hi) / count);
            }
    return output;
}
std::vector<uint8_t> activationReference(const std::vector<float>& coarse, UInt4 r, float threshold,
                                         bool sparse, bool halo) {
    std::vector<uint8_t> result(coarse.size(), 0);
    for (uint32_t z = 0; z < r.z; ++z)
        for (uint32_t y = 0; y < r.y; ++y)
            for (uint32_t x = 0; x < r.x; ++x) {
                const size_t i = (size_t(z) * r.y + y) * r.x + x;
                bool active = !sparse || coarse[i] > threshold;
                if (sparse && halo && !active) {
                    const int delta[6][3] = {
                        {-1, 0,  0 },
                        {1,  0,  0 },
                        {0,  -1, 0 },
                        {0,  1,  0 },
                        {0,  0,  -1},
                        {0,  0,  1 }
                    };
                    for (const auto& d : delta) {
                        const int qx = int(x) + d[0], qy = int(y) + d[1], qz = int(z) + d[2];
                        if (qx >= 0 && qy >= 0 && qz >= 0 && qx < int(r.x) && qy < int(r.y) && qz < int(r.z) &&
                            coarse[(size_t(qz) * r.y + qy) * r.x + qx] > threshold)
                            active = true;
                    }
                }
                result[i] = active ? 1u : 0u;
            }
    return result;
}
void verifyAtlas(ResampleVolume& scene, const std::vector<float>& source) {
    const auto actual = scene.readAtlas();
    const auto atlasMap = scene.readAtlasMap();
    const auto volume = scene.snapshot()->getDesc().volume;
    const auto& config = scene.configuration();
    const auto resolution = volume.atlasResolutionAndApron, grid = volume.atlasBrickCountAndWidth,
               src = config.sourceResolution;
    size_t nonzero = 0;
    for (uint32_t z = 0; z < resolution.z; ++z)
        for (uint32_t y = 0; y < resolution.y; ++y)
            for (uint32_t x = 0; x < resolution.x; ++x) {
                const size_t index = (size_t(z) * resolution.y + y) * resolution.x + x;
                const auto& map = atlasMap[(size_t(z / grid.w) * grid.y + y / grid.w) * grid.x + x / grid.w];
                float expected = 0;
                if (map.leaf >= 0) {
                    const auto p = transformed({float(map.x) + float(x % grid.w) - resolution.w,
                                                float(map.y) + float(y % grid.w) - resolution.w,
                                                float(map.z) + float(z % grid.w) - resolution.w, 0},
                                               config.transform);
                    const int sx = int(p.x), sy = int(p.y), sz = int(p.z);
                    if (sx >= 0 && sy >= 0 && sz >= 0 && sx < int(src.x) && sy < int(src.y) && sz < int(src.z))
                        expected = source[(size_t(sz) * src.y + sy) * src.x + sx];
                }
                if (actual[index] != expected) {
                    std::fprintf(stderr, "resample atlas (%u,%u,%u): expected %.9g actual %.9g\n", x, y, z,
                                 double(expected), double(actual[index]));
                    throw std::runtime_error("Resample atlas differs from transformed RAW reference");
                }
                nonzero += actual[index] != 0;
            }
    if (!nonzero)
        throw std::runtime_error("Resample fixture unexpectedly produced an empty atlas");
}
}  // namespace
void verifyResampleModes(ResampleVolume& scene, const std::vector<float>& source, bool originalHeadAsset) {
    const auto coarse = coarseReference(source, scene.configuration(), scene.coarseResolution());
    const uint32_t golden[] = {4096, 2472, 2997};
    if (originalHeadAsset && (scene.configuration().logDimensions != std::vector<uint32_t>{4, 3, 3, 3, 3} ||
                              scene.coarseResolution().x != 16))
        throw std::runtime_error("Legacy resample configuration requires leaf16 and16cubed coarse cells");
    for (uint32_t mode = 0; mode < 3; ++mode) {
        const bool sparse = mode != 0, halo = mode == 2;
        scene.rebuild(sparse, halo);
        const auto active = activationReference(coarse, scene.coarseResolution(),
                                                scene.configuration().threshold, sparse, halo);
        if (active != scene.activeCells()) {
            for (size_t i = 0; i < active.size(); ++i)
                if (active[i] != scene.activeCells()[i]) {
                    std::fprintf(stderr,
                                 "activation index %zu CPU=%.9g GPU=%.9g threshold=%.9g (CPU=%u GPU=%u)\n", i,
                                 double(coarse[i]), double(scene.coarseValues()[i]),
                                 double(scene.configuration().threshold), unsigned(active[i]),
                                 unsigned(scene.activeCells()[i]));
                    break;
                }
            throw std::runtime_error("Resample dense/sparse/six-face-halo activation differs from CPU");
        }
        const uint32_t leaves = uint32_t(std::count(active.begin(), active.end(), uint8_t(1)));
        if (scene.snapshot()->getDesc().activeLeafCount != leaves ||
            (originalHeadAsset && leaves != golden[mode]))
            throw std::runtime_error("Resample topology leaf count differs from reference");
        if (sparse && scene.coarseValues().size() != coarse.size())
            throw std::runtime_error("Region downsample result size differs from reference");
        if (sparse)
            for (size_t i = 0; i < coarse.size(); ++i)
                if (!std::isfinite(scene.coarseValues()[i]) ||
                    std::abs(scene.coarseValues()[i] - coarse[i]) > 2e-6f) {
                    std::fprintf(stderr, "coarse index %zu: expected %.9g actual %.9g\n", i, double(coarse[i]),
                                 double(scene.coarseValues()[i]));
                    throw std::runtime_error("Legacy region downsample differs from prefix-sum reference");
                }
        verifyAtlas(scene, source);
        std::printf("PASS resample %s: %u leaves, coarse placement and every atlas/apron voxel verified\n",
                    mode == 0   ? "dense"
                    : mode == 1 ? "sparse"
                                : "sparse+halo",
                    leaves);
    }
}
void runResampleSelfTests(ethereal::slang::ComputeRuntime& runtime) {
    // Both exact sums divided by their sample counts round to the threshold.
    // A graphics-shader reciprocal used to round these one float ULP upward,
    // incorrectly activating sparse cells. Keep the strict comparison exact.
    const std::array<Float4, 3> identity = {
        Float4{1, 0, 0, 0},
        Float4{0, 1, 0, 0},
        Float4{0, 0, 1, 0}
    };
    for (const uint32_t count : {105u, 120u}) {
        std::vector<float> values(count, 0);
        std::fill_n(values.begin(), count / 5, .25f);
        const double sum = double(count / 5) * .25;
        const float expected = float(sum / double(count));
        const auto result = queryResampleRegions(runtime, values, {count, 1, 1, 0}, {1, 1, 1, 0},
                                                 {float(count * 2), 2, 2, 0}, identity);
        if (result.size() != 1 || result[0] != expected || result[0] > .05f || expected != .05f)
            throw std::runtime_error("Region downsample changed equality at the strict activation threshold");
    }
    std::puts("PASS exact activation-threshold averages (5.25/105 and 6/120)");
    if (ResampleConfiguration{}.logDimensions != std::vector<uint32_t>{4, 3, 3, 3, 3})
        throw std::runtime_error("Legacy fromBranching(...,4) order changed");
    ResampleConfiguration c;
    c.sourceResolution = {12, 24, 24, 0};
    c.destinationResolution = {24, 24, 24, 0};
    c.logDimensions = {2, 2, 2};
    c.transform[2].w = 20;
    std::vector<float> source(12 * 24 * 24, 0);
    for (uint32_t z = 5; z < 18; ++z)
        for (uint32_t y = 7; y < 17; ++y)
            for (uint32_t x = 3; x < 8; ++x)
                source[(z * 24 + y) * 12 + x] = .25f + .125f * float((x + y + z) % 4);
    ResampleVolume scene(runtime, source, c);
    verifyResampleModes(scene, source);
    c.transform = {
        Float4{1, 0, 0, 0},
        Float4{0, 1, 0, 0},
        Float4{0, 0, 1, 0}
    };
    c.destinationResolution = {12, 24, 24, 0};
    ResampleVolume unreflected(runtime, source, c);
    verifyResampleModes(unreflected, source);
    // The sample copies source ownership and snapshots outlive subsequent owners.
    const auto retainedBytes = [&](portable::IGVDBSnapshot* snapshot) {
        portable::ChannelSnapshotDesc channel;
        if (NVRHI_FAILED(snapshot->getChannel(0, channel)))
            throw std::runtime_error("Retained resample channel");
        std::vector<uint8_t> storage(size_t(channel.raw.byteOffset + channel.raw.byteSize));
        if (!storage.empty())
            runtime.read(channel.raw.buffer, storage.data(), storage.size());
        return std::vector<uint8_t>(storage.begin() + size_t(channel.raw.byteOffset), storage.end());
    };
    portable::SnapshotHandle retained;
    std::vector<uint8_t> original;
    {
        auto input = source;
        ResampleVolume copied(runtime, input, c);
        std::fill(input.begin(), input.end(), 0);
        copied.rebuild(false, false);
        verifyAtlas(copied, source);
        retained = copied.snapshot();
        original = retainedBytes(retained);
        copied.rebuild(true, false);
        if (retainedBytes(retained) != original)
            throw std::runtime_error("Resample rebuild mutated old snapshot");
        copied.rebuild(true, true);
        if (retainedBytes(retained) != original)
            throw std::runtime_error("Resample halo rebuild mutated old snapshot");
    }
    if (retainedBytes(retained) != original)
        throw std::runtime_error("Resample snapshot did not retain released owner bytes");
    std::vector<float> zero(source.size(), 0);
    ResampleVolume empty(runtime, zero, c);
    for (bool halo : {false, true}) {
        empty.rebuild(true, halo);
        if (empty.snapshot()->getDesc().activeLeafCount || !empty.readAtlas().empty() ||
            std::count(empty.activeCells().begin(), empty.activeCells().end(), uint8_t(1)))
            throw std::runtime_error("Empty sparse resample publication");
    }
    std::puts(
        "PASS copied CPU source, retained snapshots after rebuild/release, and empty sparse/halo publication");
    std::puts("PASS reflected/permuted and axis-aligned resample fixtures");
}
}  // namespace gvdb::samples
