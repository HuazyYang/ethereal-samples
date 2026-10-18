// Independent small-volume ray and deposition references.
// SPDX-License-Identifier: Apache-2.0
#include "SpraySimulation.h"
#include "SprayRender.h"
#include "ExampleDevice.h"
#include <gvdb/slang/GVDBTopologyBuilder.h>
#include <sample-utils/SceneAssets.h>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace gvdb::slang;
using namespace gvdb::samples::spray;
namespace {
void require(bool yes, const char* what) {
    if (!yes)
        throw std::runtime_error(what);
}
void expectNear(float a, float b, float tolerance, const char* what) {
    if (!std::isfinite(a) || std::abs(a - b) > tolerance) {
        std::cerr << what << ": " << a << " expected " << b << '\n';
        throw std::runtime_error(what);
    }
}
PackedVolume oneLeaf(Int4 origin = {-8, -8, -8, 0}) {
    PackedVolume p;
    std::string error;
    TopologyBuildOptions options;
    // Deliberately small analytic fixture. Production retains legacy leaf32.
    options.logDimensions = {3, 3, 3};
    require(buildTopology({origin}, options, p, error), error.c_str());
    return p;
}
size_t at(int x, int y, int z) { return size_t((z * 10 + y) * 10 + x); }
void queryTests(ethereal::slang::ComputeRuntime& runtime) {
    const auto packed = oneLeaf();
    std::vector<float> scalar(1000, 0);
    for (int z = 0; z < 8; ++z)
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                scalar[at(x + 1, y + 1, z + 1)] = (x + .5f) / 8;
    SlangVolume volume(runtime, packed, scalar.data(), scalar.size());
    volume.updateApron();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    std::vector<Float4> origins{
        {-10, -4, -4, 0},
        {2,   -4, -4, 0},
        {-10, 2,  -4, 0},
        {-10, -4, -4, 0},
        {nan, -4, -4, 0},
        {-8,  -4, -4, 0}
    };
    std::vector<Float4> directions{
        {1,  0, 0, 0},
        {-1, 0, 0, 0},
        {1,  0, 0, 0},
        {0,  0, 0, 0},
        {1,  0, 0, 0},
        {1,  0, 0, 0}
    };
    auto ob = runtime.buffer(origins.size() * 16, 16, origins.data()),
         db = runtime.buffer(directions.size() * 16, 16, directions.data());
    auto query = queryVolumeRays(runtime, volume, ob, db, uint32_t(origins.size()));
    std::vector<Float4> hits(origins.size()), normals(hits.size());
    runtime.read(query.hits, hits.data(), hits.size() * 16);
    runtime.read(query.normals, normals.data(), normals.size() * 16);
    require(hits[0].w == 1 && hits[1].w == 1 && hits[5].w == 1, "Forward/backward/on-face rays hit");
    expectNear(hits[0].x, -5.4999f, 2e-5f, "Absolute wavefront and negative bias");
    expectNear(hits[1].x, -.2501f, 2e-5f, "Negative direction hit bias");
    expectNear(normals[0].x, -1, 1e-5f, "Negative scalar gradient normal");
    for (int i : {2, 3, 4})
        require(hits[i].w == 0 && hits[i].z == 1e10f, "Miss/zero/nonfinite ray sentinel");
    RayQueryOptions limit;
    limit.maxLeafSteps = 1;
    query = queryVolumeRays(runtime, volume, ob, db, 1, limit);
    runtime.read(query.hits, hits.data(), 16);
    require(hits[0].w == -1, "Iteration budget is not reported as a miss");
    limit.maxLeafSteps = 256;
    limit.maxLeafVisits = 1;
    limit.threshold = 2;
    query = queryVolumeRays(runtime, volume, ob, db, 1, limit);
    runtime.read(query.hits, hits.data(), 16);
    require(hits[0].w == 0, "Exactly completed final leaf is a miss");
    // Sampling bias is independent of cell selection. A large direction makes
    // the entire brick shorter than the old fixed probe step in ray parameters.
    const Float4 fastOrigin{-10, -4, -4, 0}, fastDirection{1e7f, 0, 0, 0};
    auto fastOrigins = runtime.buffer(16, 16, &fastOrigin),
         fastDirections = runtime.buffer(16, 16, &fastDirection);
    RayQueryOptions fast;
    fast.epsilon = 0;
    fast.bias = 0;
    fast.step = 2.5e-8f;
    fast.maxLeafVisits = 1;
    query = queryVolumeRays(runtime, volume, fastOrigins, fastDirections, 1, fast);
    runtime.read(query.hits, hits.data(), 16);
    require(hits[0].w == 1, "Unnormalized large-direction ray must visit its geometric cell");
    expectNear(hits[0].x, -5.5f, 3e-5f, "Large direction preserves parameter-space wavefront");
    fast.epsilon = .01f;
    query = queryVolumeRays(runtime, volume, fastOrigins, fastDirections, 1, fast);
    runtime.read(query.hits, hits.data(), 16);
    require(hits[0].w == 0, "Bias beyond completed interval gives miss without extra samples");
    const Float4 reverseOrigin{0, -4, -4, 0}, reverseDirection{-1, 0, 0, 0};
    auto reverseOrigins = runtime.buffer(16, 16, &reverseOrigin),
         reverseDirections = runtime.buffer(16, 16, &reverseDirection);
    fast.epsilon = 0;
    fast.step = .25f;
    query = queryVolumeRays(runtime, volume, reverseOrigins, reverseDirections, 1, fast);
    runtime.read(query.hits, hits.data(), 16);
    require(hits[0].w == 1, "Zero-epsilon negative boundary selects entered cell");
    expectNear(hits[0].x, 0, 1e-6f, "Zero-epsilon boundary sample");
    // Oblique entry recomputation may round one face just outside its box.
    // All rays aim through the center of a uniform brick and must hit with a
    // budget of one nonempty cell, regardless of any initial zero-length cell.
    std::vector<float> solid(1000, 1.f);
    SlangVolume solidVolume(runtime, packed, solid.data(), solid.size());
    std::vector<Float4> obliqueOrigins, obliqueDirections;
    for (unsigned i = 0; i < 257; ++i) {
        const float a = float(i) * .731f, b = float(i) * .317f;
        dm::float3 origin(-4 + 1000 * std::cos(a) * std::cos(b), -4 + 1000 * std::sin(a) * std::cos(b),
                          -4 + 1000 * std::sin(b));
        const auto direction = dm::normalize(dm::float3(-4) - origin);
        obliqueOrigins.push_back({origin.x, origin.y, origin.z, 0});
        obliqueDirections.push_back({direction.x, direction.y, direction.z, 0});
    }
    auto oo = runtime.buffer(obliqueOrigins.size() * 16, 16, obliqueOrigins.data()),
         od = runtime.buffer(obliqueDirections.size() * 16, 16, obliqueDirections.data());
    RayQueryOptions obliqueOptions;
    obliqueOptions.maxLeafVisits = 1;
    auto oblique =
        queryVolumeRays(runtime, solidVolume, oo, od, uint32_t(obliqueOrigins.size()), obliqueOptions);
    std::vector<Float4> obliqueHits(obliqueOrigins.size());
    runtime.read(oblique.hits, obliqueHits.data(), obliqueHits.size() * 16);
    for (const auto& hit : obliqueHits)
        require(hit.w == 1, "Oblique near-face rays must not fail on zero-length entry cell");
    // Two distant leaves force a root at level2. Legacy traversal adds epsilon
    // at the box, root->level1 descent, and level1->leaf descent: 2.23+.03
    // crosses2.25. A flat leaf DDA would incorrectly sample at2.25.
    PackedVolume hierarchy;
    TopologyBuildOptions hierarchyOptions;
    hierarchyOptions.logDimensions = {3, 3, 3};
    std::string hierarchyError;
    require(buildTopology(
                {
                    {-8, -8, -8, 0},
                    {56, -8, -8, 0}
    },
                hierarchyOptions, hierarchy, hierarchyError),
            hierarchyError.c_str());
    require(hierarchy.volume.hierarchy.x == 2, "Wavefront reference requires a three-level hierarchy");
    const auto atlas = hierarchy.volume.atlasResolutionAndApron;
    std::vector<float> uniformScalar(size_t(atlas.x) * atlas.y * atlas.z, 1.f);
    SlangVolume uniform(runtime, hierarchy, uniformScalar.data(), uniformScalar.size());
    const Float4 boundaryOrigin{-10.23f, -4, -4, 0}, boundaryDirection{1, 0, 0, 0};
    auto boundaryOrigins = runtime.buffer(16, 16, &boundaryOrigin),
         boundaryDirections = runtime.buffer(16, 16, &boundaryDirection);
    query = queryVolumeRays(runtime, uniform, boundaryOrigins, boundaryDirections, 1);
    runtime.read(query.hits, hits.data(), 16);
    require(hits[0].w == 1, "Hierarchy-entry wavefront fixture must hit");
    expectNear(hits[0].x, -7.7299f, 2e-5f, "Legacy hierarchy epsilon crosses absolute wavefront");
    std::cout << "Arbitrary-ray wavefront/bias/normal/signed/miss/limit fixtures PASS\n";
}
void depositTests(ethereal::slang::ComputeRuntime& runtime) {
    auto packed = oneLeaf();
    std::vector<float> scalar(1000, .2f);
    SprayVolume spray(runtime, packed, scalar, 0x80402010u);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const std::vector<Float4> points{
        {-4,    -4,    -4,    1},
        {-4.2f, -4,    -4,    1},
        {1e10f, 1e10f, 1e10f, 0},
        {-3,    -3,    -3,    1},
        {-100,  -100,  -100,  0},
        {nan,   0,     0,     0}
    };
    const std::vector<uint32_t> colors{0xff2020e0u, 0x8040e020u, 0xffffffffu,
                                       0x20e04020u, 0xffffffffu, 0xffffffffu};
    auto p = runtime.buffer(points.size() * 16, 16, points.data()),
         c = runtime.buffer(colors.size() * 4, 4, colors.data());
    std::vector<uint32_t> expectedColor(1000, 0x80402010u);
    const auto originalNodes = packed.nodes;
    for (uint32_t frame = 0; frame < 3; ++frame) {
        spray.depositPoints(p, c, uint32_t(points.size()));
        for (int z = 0; z < 8; ++z)
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x) {
                    const int world[3]{x - 8, y - 8, z - 8},
                        cell[3]{int(std::floor(float(x - 8) / 4)), int(std::floor(float(y - 8) / 4)),
                                int(std::floor(float(z - 8) / 4))};
                    const size_t i = at(x + 1, y + 1, z + 1);
                    float channel[4];
                    for (int k = 0; k < 4; ++k)
                        channel[k] = float((expectedColor[i] >> (k * 8)) & 255) / 255;
                    uint32_t contributors = 1;
                    for (size_t point = 0; point < points.size(); ++point) {
                        const float v[3]{points[point].x, points[point].y, points[point].z};
                        bool present = true;
                        for (int k = 0; k < 3; ++k)
                            present = present && std::isfinite(v[k]) &&
                                      float(cell[k]) >= std::floor((v[k] - 1) / 4) &&
                                      float(cell[k]) <= std::floor((v[k] + 2) / 4);
                        if (!present)
                            continue;
                        float distance = 0;
                        for (int k = 0; k < 3; ++k)
                            distance += (v[k] - world[k]) * (v[k] - world[k]);
                        scalar[i] = std::max(scalar[i], 1 - std::sqrt(distance));
                        for (int k = 0; k < 4; ++k)
                            channel[k] += float((colors[point] >> (k * 8)) & 255) / 255;
                        ++contributors;
                    }
                    expectedColor[i] = 0;
                    for (int k = 0; k < 4; ++k)
                        expectedColor[i] |= (uint32_t(channel[k] / contributors * 255) & 255) << (k * 8);
                }
        for (int z = 0; z < 10; ++z)
            for (int y = 0; y < 10; ++y)
                for (int x = 0; x < 10; ++x)
                    if (!x || !y || !z || x == 9 || y == 9 || z == 9) {
                        scalar[at(x, y, z)] = 0;
                        expectedColor[at(x, y, z)] = 0;
                    }
        const auto actual = spray.volume().readAtlas();
        const auto raw = spray.color().read();
        for (size_t i = 0; i < scalar.size(); ++i) {
            expectNear(actual[i], scalar[i], 1e-5f, "Full deposition scalar reference");
            uint32_t color;
            std::memcpy(&color, raw.data() + i * 4, 4);
            for (int k = 0; k < 4; ++k)
                require(std::abs(int((color >> (k * 8)) & 255) - int((expectedColor[i] >> (k * 8)) & 255)) <=
                            int(frame + 1),
                        "Full deposition color reference");
        }
        require(spray.packed().nodes == originalNodes, "Deposition must not allocate or change topology");
    }
    // A zero-hit bundle must leave density unchanged. Color can differ by one
    // conversion code after the original unpack/divide/repack operation.
    auto miss = runtime.buffer(16, 16, &points[2]);
    auto mc = runtime.buffer(4, 4, &colors[2]);
    spray.depositPoints(miss, mc, 1);
    const auto empty = spray.volume().readAtlas();
    for (size_t i = 0; i < scalar.size(); ++i)
        expectNear(empty[i], scalar[i], 1e-5f, "Miss bundle preserves density");
    std::cout << "Repeated fixed-topology scalar/RGBA/apron/miss deposition fixtures PASS\n";
}
void renderTests(ethereal::slang::ComputeRuntime& runtime) {
    std::vector<float> scalar(1000, 0);
    for (int z = 0; z < 8; ++z)
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                scalar[at(x + 1, y + 1, z + 1)] = (x + .5f) / 8;
    SprayVolume volume(runtime, oneLeaf(), scalar, 0xff204080u);
    RaycastParameters camera{};
    camera.image = {1, 1, 0, 65536};
    camera.rayOrigin = {-10, -4, -4, 0};
    camera.direction00 = {1, 0, 0, 0};
    camera.march = {.25f, 0, 1000, 1};
    camera.valueRange = {0, 1, .3f, .995f};
    camera.light = {-10, -4, -4, 0};
    camera.background = {.1f, .2f, .4f, 1};
    const Float4 raw{128.f / 255, 64.f / 255, 32.f / 255, 1};
    const auto check = [&](SprayVolume& source, Shading mode, const std::vector<Float4>& transfer,
                           Float4 expected, float expectedDepth) {
        const auto image = renderSpray(runtime, source, camera, mode, transfer);
        Float4 pixel{};
        float depth = 0;
        runtime.read(image.color, &pixel, sizeof(pixel));
        runtime.read(image.depth, &depth, sizeof(depth));
        const float got[] = {pixel.x, pixel.y, pixel.z, pixel.w},
                    want[] = {expected.x, expected.y, expected.z, expected.w};
        for (int k = 0; k < 4; ++k) {
            // The retained renderer writes uchar4. One code allows only the
            // final normalized-byte conversion boundary, not shading drift.
            const float quantized = float(uint32_t(std::clamp(want[k], 0.f, 1.f) * 255)) / 255;
            expectNear(got[k], quantized, 1.001f / 255, "Legacy spray mode RGBA equation");
        }
        if (std::isinf(expectedDepth))
            require(std::isinf(depth), "Off or missed voxel has no depth");
        else
            expectNear(depth, expectedDepth, 2e-5f, "Legacy spray mode depth");
    };
    const std::vector<Float4> constant(16384, Float4{.25f, .5f, 1, .2f});
    check(volume, Shading::Off, constant, {}, std::numeric_limits<float>::infinity());
    check(volume, Shading::Voxel, constant, raw, 4);
    check(volume, Shading::Surface, constant, raw, 4.5f);
    // Samples at t=2.25,...,9.75: 31 identical Beer-Lambert steps.
    const float transmittance = std::exp(-.2f * .25f * 31), opacity = 1 - transmittance;
    const Float4 deep{camera.background.x * transmittance + .25f * 1.2f * raw.x * opacity * opacity,
                      camera.background.y * transmittance + .5f * 1.2f * raw.y * opacity * opacity,
                      camera.background.z * transmittance + 1.2f * raw.z * opacity * opacity, opacity};
    check(volume, Shading::Volume, constant, deep, 2.25f);
    SprayVolume section(runtime, oneLeaf({48, -8, -8, 0}), scalar, 0xff204080u);
    camera.rayOrigin.x = 46;
    camera.light.x = 46;
    // The fixed X=50 section sees scalar .25 and blends its TF color over
    // the subsequent surface lit with .8 diffuse (no .1 ambient in this mode).
    const std::vector<Float4> sectionTable(16384, Float4{1, .5f, 0, .5f});
    check(section, Shading::Section, sectionTable, {raw.x * .9f, raw.y * .65f, raw.z * .4f, 1}, 4.5f);
    // Entering this brick leaves only .005 units of the first Y voxel.
    // Legacy hierarchy epsilon .01 in normalized ray distance crosses that
    // voxel before PrepareLeaf floors the position. A fixed .001 probe would
    // incorrectly render the isolated first voxel instead of background.
    std::vector<float> thinScalar(1000, 0);
    thinScalar[at(1, 1, 5)] = 1;
    SprayVolume thin(runtime, oneLeaf(), thinScalar, 0xff204080u);
    camera.rayOrigin = {-10, -9.005f, -3.5f, 0};
    camera.direction00 = {std::sqrt(.5f), std::sqrt(.5f), 0, 0};
    camera.light = camera.rayOrigin;
    check(thin, Shading::Voxel, constant, camera.background, std::numeric_limits<float>::infinity());
    thinScalar[at(1, 2, 5)] = 1;
    SprayVolume nextVoxel(runtime, oneLeaf(), thinScalar, 0xff204080u);
    const float lit = .9f * std::sqrt(.5f) + .1f;
    check(nextVoxel, Shading::Voxel, constant, {raw.x * lit, raw.y * lit, raw.z * lit, 1},
          2.005f * std::sqrt(2.f));
    std::vector<float> reverseScalar(1000, 0);
    reverseScalar[at(4, 4, 5)] = 1;
    SprayVolume reverse(runtime, oneLeaf(), reverseScalar, 0xff204080u);
    camera.rayOrigin = {-3.99f, -4.5f, -3.5f, 0};
    camera.direction00 = {-1, 0, 0, 0};
    camera.light = {2, -4.5f, -3.5f, 0};
    check(reverse, Shading::Voxel, constant, raw, .01f);
    std::cout << "Five legacy spray rendering mode equations/depths and thin-cell selection PASS\n";
}
void smoothingCadenceTests(ethereal::slang::ComputeRuntime& runtime) {
    const auto packed = oneLeaf({192, 392, 192, 0});
    std::vector<float> scalar(1000, 0);
    for (int z = 1; z <= 8; ++z)
        for (int y = 1; y <= 8; ++y)
            for (int x = 1; x <= 8; ++x)
                scalar[at(x, y, z)] = .8f;
    SprayVolume actual(runtime, packed, scalar), unfiltered(runtime, packed, scalar);
    SprayWand wand;
    uint32_t totalHits = 0;
    for (uint32_t frame = 1; frame <= 20; ++frame) {
        const auto rays = wand.next(WandStyle::Rotate);
        auto origins = runtime.buffer(rays.origins.size() * 16, 16, rays.origins.data());
        auto directions = runtime.buffer(rays.directions.size() * 16, 16, rays.directions.data());
        auto colors = runtime.buffer(rays.colors.size() * 4, 4, rays.colors.data());
        auto hits =
            queryVolumeRays(runtime, unfiltered.volume(), origins, directions, uint32_t(rays.origins.size()));
        unfiltered.depositPoints(hits.hits, colors, uint32_t(rays.origins.size()));
        actual.step(WandStyle::Rotate);
        totalHits += actual.hitCount();
        const auto before = unfiltered.volume().readAtlas();
        const auto result = actual.volume().readAtlas();
        for (int z = 0; z < 10; ++z)
            for (int y = 0; y < 10; ++y)
                for (int x = 0; x < 10; ++x) {
                    const size_t i = at(x, y, z);
                    float expected = before[i];
                    if (frame == 20 && x > 0 && x < 9 && y > 0 && y < 9 && z > 0 && z < 9)
                        expected = (4 * before[i] + before[at(x - 1, y, z)] + before[at(x + 1, y, z)] +
                                    before[at(x, y - 1, z)] + before[at(x, y + 1, z)] +
                                    before[at(x, y, z - 1)] + before[at(x, y, z + 1)]) /
                                   10;
                    expectNear(result[i], expected, 2e-5f, "Step20-only smoothing CPU stencil/cadence");
                }
    }
    require(totalHits > 0 && actual.time() == 20, "Smoothing cadence fixture deposits real hits for20steps");
    std::cout << "Twenty-frame smoothing cadence and independent six-neighbor stencil PASS\n";
}
void wandTests() {
    for (auto style : {WandStyle::Rotate, WandStyle::Sweep, WandStyle::Wave}) {
        SprayWand a, b;
        auto x = a.next(style), y = b.next(style);
        require(x.origins.size() == 1000 && a.time() == 1, "Original wand size/time");
        require(std::memcmp(x.origins.data(), y.origins.data(), x.origins.size() * 16) == 0 &&
                    std::memcmp(x.directions.data(), y.directions.data(), x.directions.size() * 16) == 0,
                "Wand deterministic seed");
        for (const auto& d : x.directions)
            expectNear(d.x * d.x + d.y * d.y + d.z * d.z, 1, 1e-5f, "Wand normalized directions");
        expectNear(x.origins[0].y, 400, 0, "Original wand height");
        require(x.colors[0] == 0x33808183u, "Original time-one packed color");
        a.next(style);
        require(a.time() == 2, "Wand advances exactly one per simulation step");
    }
    std::mt19937 reference(6572);
    std::uniform_real_distribution<float> draw(-1, 1);
    const float x = std::clamp(-.5f + draw(reference) * .1f, -.5f, .5f);
    const float y = std::clamp(-.5f + draw(reference) * .1f, -.5f, .5f);
    const float jitter = draw(reference) * .1f;
    SprayWand sweep;
    const auto first = sweep.next(WandStyle::Sweep);
    expectNear(first.origins[0].x, 200 + 25 * x, 1e-5f, "Independent sweep origin X");
    expectNear(first.origins[0].z, 200 + .5f * y - 24.5f, 1e-5f, "Independent sweep origin Z");
    const float length = std::sqrt((x + jitter) * (x + jitter) + .25f + y * y * .0016f);
    expectNear(first.directions[0].x, (x + jitter) / length, 1e-6f, "Independent random draw order/direction");
    expectNear(first.directions[0].z, y * .04f / length, 1e-6f, "Independent sweep angular spread");
    std::cout << "Seeded rotate/sweep/wave wand fixtures PASS\n";
}
}  // namespace
int main(int argc, const char** argv) {
    try {
        ethereal::slang::ExampleDevice device(argc, argv);
        ethereal::slang::ComputeRuntime runtime(device.get(), device.shaderPath());
        wandTests();
        queryTests(runtime);
        depositTests(runtime);
        renderTests(runtime);
        smoothingCadenceTests(runtime);
        for (int i = 1; i + 1 < argc; ++i)
            if (std::strcmp(argv[i], "--asset") == 0) {
                auto mesh = gvdb::sample::loadSceneObj(argv[++i]);
                auto volume = voxelizeSprayPart(runtime, mesh);
                require(volume->packed().levels[0].nodeExtent.x == 32 &&
                            volume->packed().volume.atlasBrickCountAndWidth.w == 34,
                        "Legacy spray fromBranching(...,5) requires leaf32/apron34");
                const auto nodes = volume->packed().nodes;
                const auto before = volume->volume().interiorSum();
                for (int frame = 0; frame < 3; ++frame)
                    volume->step(WandStyle::Rotate);
                require(volume->hitCount() > 0, "Real spray bundle must hit the metal part");
                require(volume->packed().nodes == nodes, "Real spray fixed topology");
                require(volume->volume().interiorSum() > before, "Real spray must deposit density");
                std::cout << "Metal asset bricks " << volume->packed().levels[0].nodes.z << " hits "
                          << volume->hitCount() << " density " << before << " -> "
                          << volume->volume().interiorSum() << " PASS\n";
            }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "spray tests: " << e.what() << '\n';
        return 1;
    }
}
