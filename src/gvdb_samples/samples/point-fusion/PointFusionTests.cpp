// SPDX-License-Identifier: Apache-2.0
#include "PointFusionTests.h"
#include "PointFusionScanner.h"
#include "PointFusionVolume.h"
#include "ComputeRuntime.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
namespace gvdb::samples {
using namespace slang;
using ethereal::slang::ComputeRuntime;
namespace {
void require(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
void close(float actual, double expected, double tolerance, const char* message) {
    if (!std::isfinite(actual) || std::abs(double(actual) - expected) > tolerance)
        throw std::runtime_error(message);
}
float random(uint32_t& state) {
    state = state * 1664525u + 1013904223u;
    return float(state & 0x00ffffffu) / 16777216.f;
}
std::vector<uint32_t> seeds(uint32_t count) {
    std::vector<uint32_t> r(count);
    for (uint32_t i = 0; i < count; ++i)
        r[i] = 1043u ^ ((i + 1u) * 0x9e3779b9u);
    return r;
}
struct ReferenceScan {
    std::vector<Float4> positions;
    std::vector<uint32_t> colors, pixels;
};
ReferenceScan scanReference(const std::vector<ScanBox>& boxes, uint32_t width, uint32_t height,
                            const ScanParameters& p, std::vector<uint32_t>& state) {
    ReferenceScan result;
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x) {
            const uint32_t pixel = y * width + x;
            const float u = random(state[pixel]), v = random(state[pixel]);
            random(state[pixel]);
            if (p.originAndMaximum.x < p.cityMinimum.x || p.originAndMaximum.x >= p.cityMaximum.x ||
                p.originAndMaximum.z < p.cityMinimum.z || p.originAndMaximum.z >= p.cityMaximum.z)
                continue;
            const double px = (x + (p.options.x ? double(u) : .5)) / width,
                         py = (y + (p.options.x ? double(v) : .5)) / height;
            const double d[3] = {p.direction00.x + px * p.directionDx.x + py * p.directionDy.x,
                                 p.direction00.y + px * p.directionDx.y + py * p.directionDy.y,
                                 p.direction00.z + px * p.directionDx.z + py * p.directionDy.z};
            const double length = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (length == 0)
                continue;
            const double direction[3] = {d[0] / length, d[1] / length, d[2] / length};
            const double origin[3] = {p.originAndMaximum.x, p.originAndMaximum.y, p.originAndMaximum.z};
            double closest = p.originAndMaximum.w;
            int hit = -1;
            for (size_t i = 0; i < boxes.size(); ++i) {
                const auto& box = boxes[i];
                const double minimum[3] = {box.minimum.x, box.minimum.y, box.minimum.z},
                             maximum[3] = {box.maximum.x, box.maximum.y, box.maximum.z};
                double low = 0, high = std::numeric_limits<double>::infinity();
                bool valid = true;
                for (unsigned axis = 0; axis < 3; ++axis) {
                    if (direction[axis] == 0) {
                        if (origin[axis] < minimum[axis] || origin[axis] > maximum[axis])
                            valid = false;
                    } else {
                        double a = (minimum[axis] - origin[axis]) / direction[axis],
                               b = (maximum[axis] - origin[axis]) / direction[axis];
                        if (a > b)
                            std::swap(a, b);
                        low = std::max(low, a);
                        high = std::min(high, b);
                    }
                }
                if (valid && high >= low && low < closest) {
                    closest = low;
                    hit = int(i);
                }
            }
            if (hit < 0)
                continue;
            result.positions.push_back({float(origin[0] + closest * direction[0]),
                                        float(origin[1] + closest * direction[1]),
                                        float(origin[2] + closest * direction[2]), 1});
            result.colors.push_back(boxes[size_t(hit)].color.x);
            result.pixels.push_back(pixel);
        }
    return result;
}
void compareScan(ComputeRuntime& runtime, PointFusionScanner& scanner, const ScanFrame& frame,
                 const ReferenceScan& expected, const std::vector<uint32_t>& expectedSeeds) {
    require(frame.count == expected.positions.size(), "Scanner compact hit count differs from CPU slabs");
    std::vector<Float4> positions(frame.count);
    std::vector<uint32_t> colors(frame.count), pixels(frame.count);
    if (frame.count) {
        runtime.read(frame.positions, positions.data(), positions.size() * 16);
        runtime.read(frame.colors, colors.data(), colors.size() * 4);
        runtime.read(frame.pixels, pixels.data(), pixels.size() * 4);
    }
    require(colors == expected.colors && pixels == expected.pixels,
            "Scanner color or stable original pixel order differs");
    for (size_t i = 0; i < positions.size(); ++i) {
        close(positions[i].x, expected.positions[i].x, 3e-5, "Scanner position x differs");
        close(positions[i].y, expected.positions[i].y, 3e-5, "Scanner position y differs");
        close(positions[i].z, expected.positions[i].z, 3e-5, "Scanner position z differs");
        require(positions[i].w == 1, "Scanner compact output contains an invalid sentinel");
    }
    require(scanner.readSeeds() == expectedSeeds, "Per-pixel scanner LCG sequence differs");
}
void scannerTests(ComputeRuntime& runtime) {
    const std::vector<ScanBox> boxes = {
        {{-2, -2, 2, 0}, {2, 2, 3, 0}, {0xff204080u, 0, 0, 0}},
        {{-2, -2, 2, 0}, {2, 2, 4, 0}, {0xff00ffffu, 0, 0, 0}},
        {{3, -4, 4, 0},  {5, 4, 5, 0}, {0xffa05010u, 0, 0, 0}}
    };
    PointFusionScanner scanner(runtime, 13, 9, boxes);
    auto state = seeds(117);
    ScanParameters p{};
    p.originAndMaximum = {0, 0, -1, 30};
    p.direction00 = {-1, .75f, 1, 0};
    p.directionDx = {2, 0, 0, 0};
    p.directionDy = {0, -1.5f, 0, 0};
    p.cityMinimum = {-10, 0, -10, 0};
    p.cityMaximum = {10, 0, 10, 0};
    p.options.x = 1;
    for (unsigned iteration = 0; iteration < 2; ++iteration) {
        const auto expected = scanReference(boxes, 13, 9, p, state);
        require(!expected.positions.empty() && expected.positions.size() < 117,
                "Scanner fixture must include hits and misses");
        compareScan(runtime, scanner, scanner.scan(p), expected, state);
    }
    p.originAndMaximum.x = 10;
    auto expected = scanReference(boxes, 13, 9, p, state);
    require(expected.positions.empty(), "Outside-city scanner oracle failed");
    compareScan(runtime, scanner, scanner.scan(p), expected, state);
    PointFusionScanner empty(runtime, 1, 1, {});
    auto emptySeeds = seeds(1);
    p.originAndMaximum = {0, 0, 0, 20};
    p.options.x = 0;
    p.direction00 = {0, 0, 1, 0};
    p.directionDx = {};
    p.directionDy = {};
    compareScan(runtime, empty, empty.scan(p), scanReference({}, 1, 1, p, emptySeeds), emptySeeds);
    const std::vector<ScanBox> originBoxes = {
        {{0, -1, -1, 0}, {1, 1, 1, 0}, {0xff112233u, 0, 0, 0}},
        {{0, -1, -1, 0}, {1, 1, 1, 0}, {0xffffffffu, 0, 0, 0}}
    };
    PointFusionScanner atOrigin(runtime, 1, 1, originBoxes);
    auto originSeeds = seeds(1);
    p.direction00 = {1, 0, 0, 0};
    expected = scanReference(originBoxes, 1, 1, p, originSeeds);
    require(expected.positions.size() == 1 && expected.positions[0].x == 0 && expected.positions[0].y == 0 &&
                expected.positions[0].z == 0,
            "Origin scanner oracle failed");
    compareScan(runtime, atOrigin, atOrigin.scan(p), expected, originSeeds);
    PointFusionScanner clipped(runtime, 1, 1, boxes);
    auto clippedSeeds = seeds(1);
    p.originAndMaximum.w = 2;
    p.direction00 = {0, 0, 1, 0};
    expected = scanReference(boxes, 1, 1, p, clippedSeeds);
    require(expected.positions.empty(), "Strict far clipping scanner oracle failed");
    compareScan(runtime, clipped, clipped.scan(p), expected, clippedSeeds);
    std::puts(
        "PASS scanner: two jittered frames, stable compact pixels, LCG, origin hit, tie, empty/outside city "
        "and strict far clip");
}
using Coordinate = std::tuple<int32_t, int32_t, int32_t>;
struct Voxel {
    double distance = 3;
    uint32_t color = 0;
};
struct FusionReference {
    std::map<Coordinate, Voxel> voxels;
    std::set<Coordinate> leaves;
};
int floorMultiple(double value, int width) { return int(std::floor(value / width)) * width; }
void updateReference(FusionReference& ref, const std::vector<Float4>& points,
                     const std::vector<uint32_t>& colors) {
    for (const auto& p : points)
        for (int z = floorMultiple(p.z - 2, 8); z <= floorMultiple(p.z + 3, 8); z += 8)
            for (int y = floorMultiple(p.y - 2, 8); y <= floorMultiple(p.y + 3, 8); y += 8)
                for (int x = floorMultiple(p.x - 2, 8); x <= floorMultiple(p.x + 3, 8); x += 8)
                    ref.leaves.emplace(x, y, z);
    for (const auto& origin : ref.leaves)
        for (int z = 0; z < 8; ++z)
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x)
                    ref.voxels.emplace(
                        Coordinate{std::get<0>(origin) + x, std::get<1>(origin) + y, std::get<2>(origin) + z},
                        Voxel{});
    for (auto& entry : ref.voxels) {
        const int xyz[3] = {std::get<0>(entry.first), std::get<1>(entry.first), std::get<2>(entry.first)};
        const int cell[3] = {floorMultiple(xyz[0], 4), floorMultiple(xyz[1], 4), floorMultiple(xyz[2], 4)};
        auto& voxel = entry.second;
        float rgba[4];
        for (unsigned c = 0; c < 4; ++c)
            rgba[c] = float((voxel.color >> (c * 8)) & 255) / 255.f;
        uint32_t contributors = 0;
        for (size_t i = 0; i < points.size(); ++i) {
            const double p[3] = {points[i].x, points[i].y, points[i].z};
            bool contained = true;
            for (unsigned axis = 0; axis < 3; ++axis)
                if (cell[axis] < floorMultiple(p[axis] - 2, 4) || cell[axis] > floorMultiple(p[axis] + 3, 4))
                    contained = false;
            if (!contained)
                continue;
            ++contributors;
            const double dx = p[0] - xyz[0], dy = p[1] - xyz[1], dz = p[2] - xyz[2];
            voxel.distance = std::min(voxel.distance, std::sqrt(dx * dx + dy * dy + dz * dz) - 1);
            for (unsigned c = 0; c < 4; ++c)
                rgba[c] += float((colors[i] >> (c * 8)) & 255) / 255.f;
        }
        if (contributors) {
            voxel.color = 0;
            for (unsigned c = 0; c < 4; ++c)
                voxel.color |= (uint32_t((rgba[c] / float(contributors + 1)) * 255.f) & 255u) << (c * 8);
        }
    }
}
ScanFrame upload(ComputeRuntime& runtime, const std::vector<Float4>& positions,
                 const std::vector<uint32_t>& colors) {
    ScanFrame f;
    f.count = uint32_t(positions.size());
    f.totalPixels = f.count;
    if (!f.count)
        return f;
    f.positions = runtime.buffer(positions.size() * 16, 16, positions.data());
    f.colors = runtime.buffer(colors.size() * 4, 4, colors.data());
    return f;
}
void compareFusion(PointFusionVolume& fusion, const FusionReference& ref) {
    const auto& packed = fusion.packed();
    require(packed.levels[0].nodes.z == ref.leaves.size(),
            "Incremental fusion leaf union differs from reference");
    const auto values = fusion.volume().readAtlas();
    const auto bytes = fusion.colorAtlas() ? fusion.colorAtlas()->read() : std::vector<uint8_t>{};
    const auto grid = packed.volume.atlasBrickCountAndWidth, resolution = packed.volume.atlasResolutionAndApron;
    for (size_t slot = 0; slot < packed.atlasMap.size(); ++slot) {
        const auto& map = packed.atlasMap[slot];
        if (map.leaf < 0)
            continue;
        require(ref.leaves.count({map.x, map.y, map.z}) == 1, "Fusion atlas has an unexpected leaf origin");
        const uint32_t base[3] = {uint32_t(slot % grid.x) * grid.w, uint32_t(slot / grid.x % grid.y) * grid.w,
                                  uint32_t(slot / (uint64_t(grid.x) * grid.y)) * grid.w};
        for (uint32_t z = 0; z < grid.w; ++z)
            for (uint32_t y = 0; y < grid.w; ++y)
                for (uint32_t x = 0; x < grid.w; ++x) {
                    const Coordinate world{map.x + int(x) - 1, map.y + int(y) - 1, map.z + int(z) - 1};
                    const auto found = ref.voxels.find(world);
                    const double expected = found == ref.voxels.end() ? 0 : found->second.distance;
                    const uint32_t expectedColor = found == ref.voxels.end() ? 0 : found->second.color;
                    const size_t index =
                        ((size_t(base[2]) + z) * resolution.y + base[1] + y) * resolution.x + base[0] + x;
                    close(values[index], expected, 3e-5,
                          "Persistent level-set interior/apron differs from CPU recurrence");
                    if (!bytes.empty())
                        for (unsigned channel = 0; channel < 4; ++channel)
                            require(std::abs(int(bytes[index * 4 + channel]) -
                                             int((expectedColor >> (channel * 8)) & 255)) <= 1,
                                    "Persistent color interior/apron differs from stable CPU recurrence");
                }
    }
}
void fusionTests(ComputeRuntime& runtime) {
    PointFusionVolume fusion(runtime, {1, 2, true});
    FusionReference reference;
    const std::vector<std::vector<Float4>> batches = {
        {{2.25f, 2.5f, 2.75f, 1},    {2.75f, 2.5f, 2.25f, 1}  },
        {{-17.25f, 1.5f, -9.75f, 1}, {10.25f, 3.75f, 1.25f, 1}},
        {{0, 0, 0, 1},               {2.25f, 2.5f, 2.75f, 1}  }
    };
    const std::vector<std::vector<uint32_t>> colors = {
        {0xff204080u, 0xff80c020u},
        {0xff70a030u, 0xff102030u},
        {0xff305070u, 0xff204080u}
    };
    for (size_t i = 0; i < batches.size(); ++i) {
        fusion.fuse(upload(runtime, batches[i], colors[i]));
        updateReference(reference, batches[i], colors[i]);
        compareFusion(fusion, reference);
    }
    const auto before = fusion.volume().readAtlas();
    const auto beforeColor = fusion.colorAtlas()->read();
    const auto beforeNodes = fusion.packed().nodes;
    fusion.fuse({});
    require(fusion.volume().readAtlas() == before && fusion.colorAtlas()->read() == beforeColor &&
                fusion.packed().nodes == beforeNodes,
            "Empty scan changed persistent volume state");
    require(fusion.frames() == 4 && fusion.totalHits() == 6, "Fusion scan statistics are wrong");
    auto invalid = upload(runtime,
                          {
                              {std::numeric_limits<float>::quiet_NaN(), 0, 0, 1}
    },
                          {0});
    bool rejected = false;
    try {
        fusion.fuse(invalid);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected && fusion.frames() == 4 && fusion.packed().nodes == beforeNodes,
            "Invalid point changed persistent volume state");
    fusion.reset(false);
    require(!fusion.hasVolume() && !fusion.colorAtlas() && !fusion.totalHits() && !fusion.frames(),
            "Fusion reset retained previous channel or topology state");
    fusion.fuse(upload(runtime, batches[0], colors[0]));
    require(fusion.hasVolume() && !fusion.colorAtlas(),
            "Scalar-only fusion incorrectly creates a color channel");
    FusionReference scalarReference;
    updateReference(scalarReference, batches[0], colors[0]);
    compareFusion(fusion, scalarReference);
    std::puts(
        "PASS incremental fusion: negative/root/atlas relocation, origin hits, persistent level set/raw RGBA, "
        "aprons, empty/invalid scans and reset");
}
}  // namespace
void runPointFusionTests(ComputeRuntime& runtime) {
    scannerTests(runtime);
    fusionTests(runtime);
}
}  // namespace gvdb::samples
