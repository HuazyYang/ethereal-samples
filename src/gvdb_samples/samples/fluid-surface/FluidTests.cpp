// Independent FLUIDS-v3 equations, sparse gather, and application state regressions.
// SPDX-License-Identifier: Apache-2.0
#include <donut/app/ApplicationBase.h>
#include <gvdb/slang/GVDBTopologyBuilder.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

#include "ComputeRuntime.h"
#include "FluidScene.h"
namespace gvdb::samples::fluid {
using namespace slang;
namespace {
void require(bool v, const char* what) {
    if (!v) throw std::runtime_error(what);
}
void expectNear(float a, float b, float tolerance, const char* what) {
    if (!std::isfinite(a) || std::abs(a - b) > tolerance) {
        std::fprintf(stderr, "%s: %.9g vs %.9g\n", what, a, b);
        throw std::runtime_error(what);
    }
}
void sphTests(ethereal::slang::ComputeRuntime& runtime) {
    SphParameters p{};
    p.gridMinimumAndCellWidth = {0, 0, 0, 1};
    p.gridResolutionAndCount = {10, 10, 10, 0};
    p.physical = {1, 1, 1, 1};
    p.boundaryMinimum = {2, 2, 2, 0};
    p.boundaryMaximum = {8, 8, 8, 0};
    p.boundary = {.5f, 10, 0, 0};
    p.integration = {0, .1f, 1000, 1000};
    p.wall = {1, 2, 4, 1};
    std::vector<Float4> positions = {{2.75f, 5, 5, 42}, {6.25f, 5, 5, 43}, {2.75f, 5, 6, 44}};
    SlangSphSimulation waves(runtime, p, positions);
    waves.step();
    const auto velocity = waves.readVelocities(), location = waves.readPositions();
    expectNear(velocity[0].x, 2.25f, 1e-5f, "FLUIDS-v3 left wall independent equation");
    expectNear(velocity[1].x, -3.75f, 1e-5f, "FLUIDS-v3 right wall independent equation");
    expectNear(velocity[2].x, velocity[0].x, 1e-6f, "FLUIDS-v3 wave has no z phase");
    expectNear(location[0].x, 2.975f, 1e-5f, "Leapfrog uses half-step velocity");
    require(location[0].w == 42, "Caller-owned particle word retained");
    p.wall.w = 0;
    SlangSphSimulation pool(runtime, p, {positions[0]});
    pool.step();
    const auto poolVelocity = pool.readVelocities();
    const float wave = (std::sin(5.f / 400) * .5f + .5f) * 2;
    const float expected = (.5f - (2.75f - (2 + wave))) * (wave * 10) * .1f;
    expectNear(poolVelocity[0].x, expected, 1e-5f, "Existing pool phase and stiffness unchanged");
    p.boundary = {0, 0, 0, 0};
    p.wall = {0, 0, 0, 1};
    positions = {{.5f, 5, 5, 0}, {1.5f, 5, 5, 0}, {7.5f, 5, 5, 0}, {8.5f, 5, 5, 0}};
    SlangSphSimulation inside(runtime, p, positions);
    inside.step();
    const auto accepted = inside.readPositions();
    require(accepted[0].x == -1000 && accepted[3].x == -1000, "FLUIDS-v3 guard cells reject outer particles");
    require(accepted[1].x == 1.5f && accepted[2].x == 7.5f, "FLUIDS-v3 inclusive interior insertion limits");
    p.wall.w = 0;
    SlangSphSimulation fullGrid(runtime, p, positions);
    fullGrid.step();
    const auto all = fullGrid.readPositions();
    require(all[0].x == .5f && all[3].x == 8.5f, "Default pool retains boundary grid cells");
    p.wall.w = 1;
    p.integration.w = 2;
    SlangSphSimulation clamp(runtime, p, {{5, 5, 5, 0}}, {{6, 8, 0, 0}});
    clamp.step();
    const auto limited = clamp.readVelocities();
    expectNear(limited[0].x, 1.2f, 1e-6f, "Corrected velocity clamp X");
    expectNear(limited[0].y, 1.6f, 1e-6f, "Corrected velocity clamp Y");
    bool rejected = false;
    p.wall.w = 2;
    try {
        SlangSphSimulation invalid(runtime, p, positions);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "Unknown SPH compatibility mode rejected");
}
void gatherTests(ethereal::slang::ComputeRuntime& runtime) {
    const std::vector<Float4> points = {{-31.75f, 16.25f, 16.25f, 0}, {31.75f, 16.25f, 16.25f, 0}};
    // Nonbinary bytes catch normalized-float/uchar texture reinterpretation regressions.
    const uint32_t colors[] = {0xffcc3399, 0xff996633};
    auto gpu = runtime.buffer(points.size() * 16, 16, points.data()),
         color = runtime.buffer(sizeof(colors), 4, colors);
    FluidSurface surface(runtime);
    surface.reconstruct(gpu, uint32_t(points.size()), color);
    const auto atlas = surface.volume().readAtlas();
    std::vector<uint32_t> actualColor(atlas.size());
    runtime.read(surface.color(), actualColor.data(), actualColor.size() * 4);
    const auto& packed = surface.packed();
    const auto& level = packed.levels[0];
    const auto r = packed.volume.atlasResolutionAndApron;
    require(packed.levels.size() == 5 && level.nodes.w == 5 && level.nodeExtent.x == 32 &&
                level.nodeExtent.y == 32 && level.nodeExtent.z == 32 && packed.levels[1].nodes.w == 3,
            "Legacy reversed-argument configuration has width-32 fluid leaves");
    require(level.nodes.z == 4 && packed.volume.boundsMin.x == -64 && packed.volume.boundsMax.x == 64,
            "Independent signed support crosses both 32-voxel boundaries");
    constexpr int width = 32;
    using Key = std::tuple<int, int, int>;
    std::map<Key, float> expected;
    for (uint32_t n = 0; n < level.nodes.z; ++n) {
        const auto* node = packed.nodes.data() + (level.nodes.x + size_t(n) * level.nodes.y) / 4;
        for (int z = 0; z < width; ++z)
            for (int y = 0; y < width; ++y)
                for (int x = 0; x < width; ++x) {
                    const int world[] = {int32_t(node[1]) + x, int32_t(node[2]) + y, int32_t(node[3]) + z};
                    float value = 3;
                    uint32_t rgb = 0, contributors = 0;
                    for (size_t i = 0; i < points.size(); ++i) {
                        const float p[] = {points[i].x, points[i].y, points[i].z};
                        bool member = true;
                        float distance = 0;
                        for (int a = 0; a < 3; ++a) {
                            const int cell = int(std::floor(double(world[a]) / 4));
                            member &= cell >= int(std::floor((p[a] - 2) / 4)) &&
                                      cell <= int(std::floor((p[a] + 3) / 4));
                            distance += (p[a] - world[a]) * (p[a] - world[a]);
                        }
                        if (member) {
                            value = std::min(value, std::sqrt(distance) - 1);
                            rgb = colors[i] & 0xffffff;
                            ++contributors;
                        }
                    }
                    require(contributors <= 1, "Independent colored fixtures have distinct support bins");
                    expected[{world[0], world[1], world[2]}] = value;
                    const size_t address = (size_t(node[6] + z) * r.y + node[5] + y) * r.x + node[4] + x;
                    expectNear(atlas.at(address), value, 2e-5f, "Independent radius-one scalar gather");
                    require((actualColor.at(address) & 0xffffff) == rgb,
                            "RGBA8 atlas shares exact scalar slots");
                }
    }
    require(packed.volume.boundsMin.x < 0 && packed.volume.boundsMax.x > 0, "Signed sparse fluid bounds");
    expectNear(expected.at({-32, 16, 16}), std::sqrt(3.f * .25f * .25f) - 1, 1e-6f,
               "Negative boundary sphere sample");
    for (uint32_t n = 0; n < level.nodes.z; ++n) {
        const auto* node = packed.nodes.data() + (level.nodes.x + size_t(n) * level.nodes.y) / 4;
        for (int z = -1; z <= width; ++z)
            for (int y = -1; y <= width; ++y)
                for (int x = -1; x <= width; ++x) {
                    auto it =
                        expected.find({int32_t(node[1]) + x, int32_t(node[2]) + y, int32_t(node[3]) + z});
                    const float wanted = it == expected.end() ? 3 : it->second;
                    const size_t address =
                        (size_t(int(node[6]) + z) * r.y + uint32_t(int(node[5]) + y)) * r.x +
                        uint32_t(int(node[4]) + x);
                    expectNear(atlas.at(address), wanted, 2e-5f,
                               "Scalar apron preserves sparse background three");
                }
    }
}
}  // namespace
void runFluidTopologyTests() {
    const auto initial = makeInitialFluid(1500000);
    require(initial.positions.size() == 1500000, "Full legacy particle count");
    // Independent golden from the retained full preset: GVDB fromBranching accepts
    // (r4,r3,r2,r1,r0), and the initial legacy run reports 7 * 4 * 7 leaves.
    const auto topology = buildFluidTopology(initial.positions);
    require(topology.levels.size() == 5, "All five legacy level descriptors retained");
    const uint32_t widths[] = {32, 256, 2048, 16384, 131072};
    for (size_t level = 0; level < 5; ++level) {
        require(topology.levels[level].nodes.w == (level == 0 ? 5u : 3u) &&
                    topology.levels[level].nodeExtent.x == widths[level],
                "Independent leaf-first legacy branching and cumulative widths");
    }
    require(topology.levels[0].nodes.z == 196, "Legacy initial preset has exactly 196 leaves");
    const auto minimum = topology.volume.boundsMin, maximum = topology.volume.boundsMax;
    require(minimum.x == 96 && minimum.y == 64 && minimum.z == 96 && maximum.x == 320 && maximum.y == 192 &&
                maximum.z == 320,
            "Legacy initial topology bounds are (96,64,96)..(320,192,320)");
    expectNear(initial.parameters.physical.x, 3, 1e-6f, "World smoothing support");
    require(initial.parameters.wall.w == 1 && initial.parameters.wall.y == 100,
            "Legacy wave preset selected");
    require(initial.colors.front() == 0xff333333, "Legacy first particle color");
    const auto reset = makeInitialFluid(32);
    require(std::memcmp(initial.positions.data(), reset.positions.data(), 32 * sizeof(Float4)) == 0,
            "Portable seeded reset is deterministic");
    const auto support = buildFluidTopology({{-31.75f, 16.25f, 16.25f, 0}, {31.75f, 16.25f, 16.25f, 0}});
    std::set<std::tuple<int, int, int>> origins;
    const auto level = support.levels.front();
    for (uint32_t n = 0; n < level.nodes.z; ++n) {
        const auto* node = support.nodes.data() + (level.nodes.x + size_t(n) * level.nodes.y) / 4;
        origins.emplace(int32_t(node[1]), int32_t(node[2]), int32_t(node[3]));
    }
    require(origins == std::set<std::tuple<int, int, int>>{{-64, 0, 0}, {-32, 0, 0}, {0, 0, 0}, {32, 0, 0}},
            "Independent signed full-rebuild support activates exact boundary leaves");
    bool rejected = false;
    try {
        buildFluidTopology({{std::numeric_limits<float>::quiet_NaN(), 16, 16, 0}});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "Nonfinite fluid topology positions rejected");
    std::puts("Fluid CPU legacy topology: 196 leaves, bounds (96,64,96)..(320,192,320), signed support PASS");
}
void runFluidTests(ethereal::slang::ComputeRuntime& runtime, const std::filesystem::path& assets, bool rt) {
    runFluidTopologyTests();
    sphTests(runtime);
    gatherTests(runtime);
    AppOptions options;
    options.assets = assets;
    options.compute = !rt;
    options.compact = true;
    options.particles = 4096;
    options.captureSteps = 2;
    FluidScene scene(runtime, options);
    sample::VolumeViewOptions view;
    view.width = 80;
    view.height = 60;
    const auto& s = scene.sceneDescription();
    require(s.meshes.size() == 1 && s.volumes.size() == 1 && s.instances.size() == 2,
            "Ground and fluid mixed scene");
    require(s.volumes[0].threshold.y == 3 && s.volumes[0].threshold.z == -1 &&
                s.volumes[0].transferLastIndex == 16300,
            "Signed legacy transfer range and inclusive endpoint");
    expectNear(s.instances[1].transform[3], -10 * std::sin(dm::radians(5.f)), 1e-6f,
               "Ground pretranslation rotates with mesh");
    expectNear(s.materials[0].refrIor, 1.1f, 1e-6f, "Legacy refraction material");
    scene.animate(0, view);
    require(scene.frame() == 1, "One SPH step per simulation frame");
    auto output = sample::renderVolumeView(runtime, scene, view);
    scene.animate(0, view);
    output = sample::renderVolumeView(runtime, scene, view);
    scene.animate(0, view);
    output = sample::renderVolumeView(runtime, scene, view);
    require(scene.frame() == 2 && scene.sampleCount() == 2,
            "Capture freezes solver then accumulates final-state samples");
    if (rt) {
        view.width = 93;
        view.height = 67;
        output = sample::renderVolumeView(runtime, scene, view);
        require(output.width == 93 && output.height == 67 && scene.sampleCount() == 1,
                "Paused resize resets actual RT accumulation and dimensions");
    }
    std::vector<Float4> image(size_t(output.width) * output.height);
    std::vector<float> depth(image.size());
    runtime.read(output.color, image.data(), image.size() * 16);
    runtime.read(output.depth, depth.data(), depth.size() * 4);
    uint32_t hits = 0;
    for (size_t i = 0; i < image.size(); ++i) {
        auto c = image[i];
        require(std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z) && std::isfinite(c.w) &&
                    !std::isnan(depth[i]),
                "Finite fluid frame");
        hits += depth[i] >= 0 && depth[i] < 20000;
    }
    require(hits > 0, "Fluid frame contains scene hits");
    if (rt) {
        const auto sampleCount = scene.sampleCount();
        require(scene.verifyVolumeContribution() > 0, "RT fluid, not only ground, is visible");
        std::vector<Float4> after(image.size());
        runtime.read(output.color, after.data(), after.size() * 16);
        require(std::memcmp(image.data(), after.data(), image.size() * 16) == 0 &&
                    scene.sampleCount() == sampleCount,
                "Contribution verification preserves capture pixels and accumulation state");
        scene.key('3', view);
        require(scene.shading() == 2, "Trilinear mode selected");
        scene.key('3', view);
        require(scene.shading() == 3, "Deep mode selected");
        output = sample::renderVolumeView(runtime, scene, view);
        runtime.read(output.color, image.data(), image.size() * 16);
        for (const auto c : image)
            require(std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z),
                    "Legacy negative-range deep mode finite");
#if ETHEREAL_FLUID_RT
        TopologyBuildOptions build;
        build.logDimensions = {1, 1};
        PackedVolume packed;
        std::string error;
        require(buildTopology({{0, 0, 0, 0}}, build, packed, error), "Signed-range analytic topology");
        auto res = packed.volume.atlasResolutionAndApron;
        std::vector<float> scalar(size_t(res.x) * res.y * res.z, 2);
        SlangVolume gpu(runtime, packed, scalar.data(), scalar.size());
        auto v = sample::SceneVolume::from(gpu);
        v.mode = sample::SceneVolumeMode::Deep;
        v.threshold = {4, 3, -1, 0};
        v.extinct = {-1, 1, 0, 0};
        v.cutoff = {0, .0001f, 0, 0};
        v.transfer = {{1, 0, 0, .1f}, {0, 1, 0, .5f}, {0, 0, 1, .9f}};
        sample::SceneDescription analytic;
        analytic.volumes = {v};
        analytic.materials.emplace_back();
        sample::SceneInstance instance;
        instance.kind = sample::SceneInstance::Kind::Volume;
        analytic.instances = {instance};
        const char* target = runtime.device()->getGraphicsAPI() == nvrhi::GraphicsAPI::CUDA
                                 ? "optix"
                                 : donut::app::GetShaderTypeName(runtime.device()->getGraphicsAPI());
        sample::SlangSceneRT probe(runtime, donut::app::GetDirectoryWithExecutable() / "shaders/gvdb_slang" /
                                                target / "SlangSceneRT.bin");
        probe.setScene(analytic);
        RaycastParameters camera{};
        camera.volume = packed.volume;
        camera.image = {1, 1, 3, 128};
        camera.rayOrigin = {-1, 1, 1, 0};
        camera.direction00 = {1, 0, 0, 0};
        camera.march = {.25f, 0, 10, 1};
        camera.valueRange = {0, 4, 0, .995f};
        camera.background = {.2f, .4f, .6f, 1};
        camera.light = {-10, 10, 1, 0};
        auto sample = probe.render(camera, 0);
        Float4 actual{};
        runtime.read(sample.color, &actual, sizeof(actual));
        // Seven .25 samples in one width-two leaf. Signed (2-4)/( -1-3 )=.5
        // selects green, then the retained pipeline packs opacity and applies it twice.
        const float opacity = 1 - std::exp(-.5f * 7 * .25f), a = std::floor(opacity * 255 + .5f) / 255;
        expectNear(actual.x, .2f * (1 - a), .002f, "Negative transfer range selects green, not red");
        expectNear(actual.y, .4f * (1 - a) + a * a, .002f,
                   "Signed-range independent Beer and packed-alpha reference");
        expectNear(actual.z, .6f * (1 - a), .002f, "Negative transfer range does not select blue endpoint");
#endif
    }
#if ETHEREAL_FLUID_RT
    if (rt) {
        auto offOptions = options;
        offOptions.particles = 128;
        offOptions.shading = 0;
        offOptions.captureSteps = 0;
        FluidScene off(runtime, offOptions);
        sample::VolumeViewOptions offView;
        offView.width = 80;
        offView.height = 60;
        off.animate(0, offView);
        auto a = sample::renderVolumeView(runtime, off, offView);
        std::vector<Float4> first(size_t(a.width) * a.height), second(first.size());
        runtime.read(a.color, first.data(), first.size() * 16);
        off.animate(0, offView);
        auto b = sample::renderVolumeView(runtime, off, offView);
        runtime.read(b.color, second.data(), second.size() * 16);
        require(off.frame() == 2 && off.sampleCount() == 1,
                "Off mode advances SPH and resets per-frame accumulation");
        require(std::memcmp(first.data(), second.data(), first.size() * 16) != 0,
                "Off ground-only frame advances stochastic dataset seed");
    }
#endif
    const auto eye = view.cameraEye;
    require(scene.mouseDrag(0, 8, 3, 0, view) && view.cameraEye.x != eye.x, "Original orbit controls");
    const auto light = view.lightDirection;
    require(scene.mouseDrag(1, 0, 10, 1, view) && view.lightDirection.x != light.x,
            "Shift distance controls light");
    require(scene.key('1', view) && view.topology, "Topology toggle");
    require(scene.key('2', view) && scene.overlayLines().size() == 512,
            "Every eighth particle velocity line");
    for (const auto& line : scene.overlayLines())
        require(std::isfinite(line.start.w) && std::isfinite(line.color.x),
                "Packed color bits never become overlay coordinates");
    require(scene.key('4', view) && !scene.colorAtlas(), "Color channel toggle");
    require(scene.key('`', view) && !view.showUI && scene.overlayLines().empty(),
            "GUI toggle suppresses debug geometry");
    scene.key('R', view);
    require(scene.frame() == 0 && scene.sampleCount() == 0, "Reset simulation and accumulation");
    scene.key(' ', view);
    scene.animate(0, view);
    require(scene.frame() == 0, "Space pauses solver");
    std::puts("Fluid SPH equations, insertion, clamp, signed gather, color, and application fixtures PASS");
}
}  // namespace gvdb::samples::fluid
