// Analytic acceptance for mixed mesh/volume NVRHI ray tracing.
// SPDX-License-Identifier: Apache-2.0
#include "../../tests/PortableFixtures.h"
#include "ComputeRuntime.h"
#include "SlangSceneRT.h"
#include <donut/app/ApplicationBase.h>
#include <donut/app/DeviceManager.h>
#include <donut/core/log.h>
#include <nvrhi/core/foundation.h>
#if ETHEREAL_TEST_CUDA
#include <nvrhi/cuda.h>
#endif
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

using namespace nvrhi;
namespace g = gvdb::slang;
namespace f = gvdb::slang::fixtures;
namespace s = gvdb::sample;
using ethereal::slang::ComputeRuntime;
namespace {
std::atomic_uint validationProblems{0};
struct Messages : ObjectImpl<IMessageCallback> {
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Messages)
    NVRHI_IMPLEMENTS_INTERFACE(IMessageCallback)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    void message(MessageSeverity severity, const char *text) noexcept override {
        if (severity >= MessageSeverity::Warning)
            ++validationProblems;
        std::fprintf(stderr, "NVRHI: %s\n", text);
    }
};
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
void expectNear(float actual, float expected, const char *message, float tolerance = 2e-4f) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) {
        std::fprintf(stderr, "%s: actual %.9g expected %.9g\n", message, actual, expected);
        throw std::runtime_error(message);
    }
}
void expectColor(g::Float4 actual, g::Float4 expected, const char *message) {
    expectNear(actual.x, expected.x, message);
    expectNear(actual.y, expected.y, message);
    expectNear(actual.z, expected.z, message);
    expectNear(actual.w, expected.w, message);
}
template <class Operation>
void reject(Operation operation, const char *message) {
    bool rejected = false;
    try {
        operation();
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    require(rejected, message);
}
struct Pixel {
    g::Float4 color;
    float depth;
};
Pixel read(ComputeRuntime &runtime, const g::RaycastOutput &image) {
    Pixel result{};
    runtime.read(image.color, &result.color, sizeof(result.color));
    runtime.read(image.depth, &result.depth, sizeof(result.depth));
    return result;
}
g::RaycastParameters camera(g::Float4 origin = {-1, 1, 1, 0}) {
    g::RaycastParameters p{};
    p.image = {1, 1, 0, 128};
    p.rayOrigin = origin;
    p.direction00 = {1, 0, 0, 0};
    p.march = {.25f, 0, 100, 1};
    p.valueRange = {0, 1, .1f, .995f};
    p.background = {.125f, .25f, .5f, 1};
    return p;
}
s::SceneMaterial ambient(g::Float4 color) {
    s::SceneMaterial material;
    material.ambColor = {color.x, color.y, color.z};
    material.diffColor = material.specColor = material.reflColor = material.refrColor =
        donut::math::float3::zero();
    material.reflWidth = material.refrWidth = 0;
    material.shadowBias = .001f;
    return material;
}
s::SceneMesh triangle(float extent = 1) {
    // A triangle in the YZ plane, facing -X. Empty normals exercise geometric fallback.
    s::SceneMesh mesh;
    mesh.positions = {
        {0, -extent, -extent, 1},
        {0, 0,       extent,  1},
        {0, extent,  -extent, 1}
    };
    mesh.indices = {0, 1, 2};
    return mesh;
}
s::SceneInstance instance(s::SceneInstance::Kind kind, uint32_t geometry, uint32_t material, float x, float y,
                          float z) {
    s::SceneInstance result;
    result.kind = kind;
    result.geometry = geometry;
    result.material = material;
    result.transform[3] = x;
    result.transform[7] = y;
    result.transform[11] = z;
    return result;
}
s::SceneDescription meshScene(g::Float4 color = {.2f, .4f, .6f, 1}, float x = 3) {
    s::SceneDescription scene;
    scene.meshes.push_back(triangle());
    scene.materials.push_back(ambient(color));
    scene.instances.push_back(instance(s::SceneInstance::Kind::Mesh, 0, 0, x, 1, 1));
    return scene;
}
s::SceneVolume volume(ComputeRuntime &runtime, float density, int shift = 0, bool missingSecondLeaf = false) {
    auto fixture = f::twoBrickVolume(density);
    if (missingSecondLeaf)
        fixture.children[2] = fixture.children[3] = UINT32_MAX;
    fixture.volume.boundsMin.x += float(shift);
    fixture.volume.boundsMax.x += float(shift);
    for (size_t n = 0; n < fixture.nodes.size(); n += 16)
        fixture.nodes[n + 1] += uint32_t(shift);
    for (auto &node : fixture.atlasMap)
        node.x += shift;
    g::PackedVolume packed;
    std::string error;
    require(g::packVolume(f::volumeSource(fixture), packed, error), error.c_str());
    g::SlangVolume uploaded(runtime, packed, fixture.atlas.data(), fixture.atlas.size());
    auto result = s::SceneVolume::from(uploaded);
    result.steps = {.25f, 16, .25f, .001f};
    result.extinct = {-1, 1, 0, 0};
    result.cutoff = {.005f, .00001f, 0, 0};
    result.threshold = {0, 0, 1, 0};
    result.transfer = {
        {.6f, .2f, .1f, .2f},
        {.6f, .2f, .1f, .2f}
    };
    return result;
}
float packedChannel(float value) { return std::floor(std::clamp(value, 0.f, 1.f) * 255.f + .5f) / 255.f; }
g::Float4 deepExpected(g::Float4 transfer, g::Float4 behind, g::Float4 tint = {1, 1, 1, 1},
                       uint32_t samples = 14) {
    // Independent retained-legacy equation: two occupied width-2 leaves, step .25.
    // Each leaf starts after epsilon on its global wavefront: 7 samples per leaf.
    // rayDeepBrick integrates 14 * .25 units; OptiX packs rounded RGBA8 attributes,
    // then closest-hit lerps the already premultiplied RGB by opacity a second time.
    const float opacity = 1.f - std::exp(-transfer.w * float(samples) * .25f);
    const float a = packedChannel(opacity);
    return {behind.x * (1 - a) + packedChannel(transfer.x * opacity * tint.x) * a,
            behind.y * (1 - a) + packedChannel(transfer.y * opacity * tint.y) * a,
            behind.z * (1 - a) + packedChannel(transfer.z * opacity * tint.z) * a, 1};
}
void meshMaterials(ComputeRuntime &runtime, s::SlangSceneRT &renderer) {
    const g::Float4 red{.75f, .1f, .05f, 1}, blue{.05f, .2f, .8f, 1};
    auto scene = meshScene(red);
    scene.materials.push_back(ambient(blue));
    scene.instances[0].transform[7] = -3;
    scene.instances.push_back(instance(s::SceneInstance::Kind::Mesh, 0, 1, 3, 5, 1));
    renderer.setScene(scene);
    renderer.setPointLight({-10, 100, 1, 1});
    const auto first = read(runtime, renderer.render(camera({-1, -3, 1, 0})));
    const auto second = read(runtime, renderer.render(camera({-1, 5, 1, 0})));
    expectColor(first.color, red, "first instance material");
    expectColor(second.color, blue, "second instance material");
    expectNear(first.depth, 4, "triangle distance", .002f);
    expectNear(second.depth, 4, "second instance distance", .002f);
    std::puts("PASS shared mesh BLAS, distinct instance materials and geometric normals");
}
void deepComposition(ComputeRuntime &runtime, s::SlangSceneRT &renderer) {
    const g::Float4 behind{.2f, .4f, .6f, 1};
    auto scene = meshScene(behind, 6);
    scene.volumes.push_back(volume(runtime, 1));
    scene.instances.push_back(instance(s::SceneInstance::Kind::Volume, 0, 0, 0, 0, 0));
    renderer.setScene(scene);
    // The triangle's shadow rays head upward outside the volume instead of through it.
    renderer.setPointLight({5, 100, 1, 1});
    auto pixel = read(runtime, renderer.render(camera()));
    expectColor(pixel.color, deepExpected(scene.volumes[0].transfer[0], behind), "deep volume over mesh");
    expectNear(pixel.depth, 1.25f, "first allocated deep sample");
    // Packed color-channel tint must affect integration without changing opacity.
    std::vector<uint32_t> tint(128, 0xff4080ffu);
    auto untintedPending = renderer.render(camera());
    auto tinted = scene.volumes[0];
    tinted.color = runtime.buffer(tint.size() * 4, 0, tint.data(), true);
    renderer.updateVolume(0, tinted);
    pixel = read(runtime, renderer.render(camera()));
    expectColor(read(runtime, untintedPending).color, deepExpected(scene.volumes[0].transfer[0], behind),
                "pending volume generation survives atlas replacement");
    expectColor(pixel.color, deepExpected(tinted.transfer[0], behind, {1, 128.f / 255, 64.f / 255, 1}),
                "deep packed-color atlas");
    std::puts(
        "PASS analytic deep attenuation, leaf wavefront, packed attributes, color atlas and mesh continuation");
}
void deepCameraClipping(ComputeRuntime &runtime, s::SlangSceneRT &renderer) {
    const g::Float4 behind{.2f, .4f, .6f, 1};
    auto scene = meshScene(behind, 6);
    scene.volumes = {volume(runtime, 1)};
    scene.instances.push_back(instance(s::SceneInstance::Kind::Volume, 0, 0, 0, 0, 0));
    renderer.setScene(scene);
    renderer.setEnvironment({});
    renderer.setPointLight({5, 100, 1, 1});
    auto p = camera();
    // The original fixture occupies x=[0,4]. From x=-1, the first leaf
    // contributes at ray distances 1.25,1.5,1.75,2 before the far plane 2.1.
    // The mesh at distance7 must also be excluded by deep continuation.
    p.march.z = 2.1f;
    auto pixel = read(runtime, renderer.render(p));
    expectColor(pixel.color, deepExpected(scene.volumes[0].transfer[0], p.background, {1, 1, 1, 1}, 4),
                "deep far plane limits integration and continuation");
    expectNear(pixel.depth, 1.25f, "deep clipped first sample");
    p.march.y = 1.6f;
    pixel = read(runtime, renderer.render(p));
    expectColor(pixel.color, deepExpected(scene.volumes[0].transfer[0], p.background, {1, 1, 1, 1}, 2),
                "deep near plane removes preceding samples");
    expectNear(pixel.depth, 1.75f, "deep near clipped sample");
    p.march.y = 0;
    p.march.z = 4.1f;
    expectColor(read(runtime, renderer.render(p)).color,
                deepExpected(scene.volumes[0].transfer[0], p.background, {1, 1, 1, 1}, 11),
                "deep far plane within second leaf");
    // A nearer triangle must not truncate integration, regardless of BVH order.
    // Send its shadow rays beyond the fixture to isolate the composition equation.
    scene.instances[0].transform[3] = 2;
    scene.materials[0].shadowBias = 100;
    renderer.setScene(scene);
    expectColor(read(runtime, renderer.render(p)).color,
                deepExpected(scene.volumes[0].transfer[0], behind, {1, 1, 1, 1}, 11),
                "deep camera clipping remains independent of nearest triangle");
    std::puts("PASS deep near/far integration, clipped continuation and traversal independence");
}
void volumeModes(ComputeRuntime &runtime, s::SlangSceneRT &renderer) {
    const g::Float4 color{.25f, .5f, .75f, 1};
    s::SceneDescription scene;
    scene.materials = {ambient(color)};
    scene.volumes = {volume(runtime, 1)};
    scene.instances = {instance(s::SceneInstance::Kind::Volume, 0, 0, 0, 0, 0)};
    scene.volumes[0].mode = s::SceneVolumeMode::Surface;
    scene.volumes[0].threshold.x = .5f;
    renderer.setScene(scene);
    renderer.setPointLight({-10, 1, 1, 1});
    auto pixel = read(runtime, renderer.render(camera()));
    expectColor(pixel.color, color, "surface ambient material");
    // Legacy surface shading displaces the first sample two index units along
    // the outward normal; its reported distance is measured from that position.
    expectNear(pixel.depth, .75f, "retained surface offset distance");
    scene.volumes[0] = volume(runtime, 0);
    scene.volumes[0].mode = s::SceneVolumeMode::LevelSet;
    scene.volumes[0].threshold.x = .1f;
    renderer.updateVolume(0, scene.volumes[0]);
    pixel = read(runtime, renderer.render(camera()));
    expectColor(pixel.color, color, "level-set ambient material");
    expectNear(pixel.depth, 1.25f, "level-set first threshold sample");
    scene.volumes[0].mode = s::SceneVolumeMode::EmptySkip;
    renderer.updateVolume(0, scene.volumes[0]);
    pixel = read(runtime, renderer.render(camera()));
    expectColor(pixel.color, color, "empty-skip ambient material");
    expectNear(pixel.depth, 1.001f, "empty-skip leaf entry epsilon");
    std::puts("PASS surface, level-set and empty-skip shader groups with retained legacy distances");
}
void volumeArrayEndpoints(ComputeRuntime &runtime, s::SlangSceneRT &renderer) {
    s::SceneDescription scene;
    scene.materials.push_back(ambient({0, 0, 0, 1}));
    for (uint32_t n = 0; n < s::SlangSceneRT::MaxVolumes; ++n) {
        auto item = volume(runtime, n == 7 ? 2.f : .25f, int(n * 100));
        item.transfer = {
            {.8f,  .1f, .05f, .25f},
            {.05f, .1f, .8f,  .5f }
        };
        scene.volumes.push_back(item);
    }
    scene.instances = {instance(s::SceneInstance::Kind::Volume, 0, 0, 0, 0, 0),
                       instance(s::SceneInstance::Kind::Volume, 7, 0, -700, 4, 0)};
    renderer.setScene(scene);
    auto p = camera();
    expectColor(read(runtime, renderer.render(p)).color,
                deepExpected(scene.volumes[0].transfer[0], p.background), "volume resource array element zero");
    p.rayOrigin.y = 5;
    const auto pixel = read(runtime, renderer.render(p));
    expectColor(pixel.color, deepExpected(scene.volumes[7].transfer[1], p.background),
                "volume resource array element seven");
    expectNear(pixel.depth, 1.25f, "translated resource-array volume distance");
    // Reverse traversal reaches the exact lower boundary of an empty cell before
    // entering an occupied leaf. Negative index coordinates exercise root origin.
    scene.volumes = {volume(runtime, 1, -100, true)};
    scene.instances = {instance(s::SceneInstance::Kind::Volume, 0, 0, 0, 0, 0)};
    renderer.setScene(scene);
    p = camera({-95, 1, 1, 0});
    p.direction00 = {-1, 0, 0, 0};
    const auto reverse = read(runtime, renderer.render(p));
    expectColor(reverse.color, deepExpected(scene.volumes[0].transfer[0], p.background, {1, 1, 1, 1}, 7),
                "reverse sparse traversal attenuation");
    expectNear(reverse.depth, 3.25f, "reverse ray crosses empty leaf boundary");
    std::puts(
        "PASS resource-array endpoints, translated topology and reverse sparse traversal at negative "
        "coordinates");
}
void transformAndReplacement(ComputeRuntime &runtime, s::SlangSceneRT &renderer,
                             const std::filesystem::path &library) {
    const g::Float4 red{.7f, .1f, .2f, 1}, green{.1f, .7f, .2f, 1};
    auto scene = meshScene(red);
    renderer.setScene(scene);
    renderer.setPointLight({-10, 100, 1, 1});
    auto p = camera();
    auto old = renderer.render(p);  // Deliberately keep the GPU result pending across replacement.
    rt::AffineTransform moved = {1, 0, 0, 3, 0, 1, 0, 20, 0, 0, 1, 1};
    renderer.updateTransform(0, moved);
    auto miss = renderer.render(p);
    expectColor(read(runtime, old).color, red, "old output survives TLAS update");
    expectColor(read(runtime, miss).color, p.background, "transform update produces miss");
    expectNear(read(runtime, miss).depth, 1e10f, "miss sentinel", 1.f);
    renderer.setScene(scene);
    old = renderer.render(p);
    auto changed = triangle();
    for (auto &position : changed.positions)
        position.x += 2;
    renderer.updateMesh(0, changed);
    auto replacement = renderer.render(p);
    expectNear(read(runtime, old).depth, 4, "old BLAS output survives replacement", .002f);
    expectNear(read(runtime, replacement).depth, 6, "replacement mesh distance", .002f);
    old = renderer.render(p);
    renderer.setScene(meshScene(green));
    replacement = renderer.render(p);
    expectColor(read(runtime, old).color, red, "old output survives entire scene replacement");
    expectColor(read(runtime, replacement).color, green, "new scene material");
    g::RaycastOutput afterDestruction;
    {
        s::SlangSceneRT temporary(runtime, library);
        temporary.setScene(meshScene(green));
        temporary.setPointLight({-10, 100, 1, 1});
        afterDestruction = temporary.render(p);
    }
    expectColor(read(runtime, afterDestruction).color, green, "submitted output survives renderer destruction");
    std::puts("PASS TLAS updates, pending-output geometry/scene replacement and renderer destruction");
}
void environmentAndAccumulation(ComputeRuntime &runtime, s::SlangSceneRT &renderer) {
    auto scene = meshScene();
    scene.instances[0].transform[7] = 20;
    renderer.setScene(scene);
    renderer.setEnvironment({
        1, 1, {64, 128, 192, 255}
    });
    auto p = camera();
    const g::Float4 expected{p.background.x * 64.f / 255, p.background.y * 128.f / 255,
                             p.background.z * 192.f / 255, 1};
    auto first = renderer.render(p, 0), second = renderer.render(p, 1);
    require(renderer.sampleCount() == 2, "progressive sample count");
    reject([&] { renderer.render(p, 99); }, "nonsequential explicit sample rejected");
    expectColor(read(runtime, first).color, expected, "constant environment miss");
    expectColor(read(runtime, second).color, expected, "progressive average of constant scene");
    renderer.render(p, 0);
    require(renderer.sampleCount() == 1, "explicit sample zero resets");
    p.rayOrigin.y += .25f;
    renderer.render(p);
    require(renderer.sampleCount() == 1, "camera change resets");
    p.image.x = 2;
    auto resized = renderer.render(p);
    require(renderer.sampleCount() == 1 && resized.width == 2, "resolution change resets");
    g::Float4 both[2];
    runtime.read(resized.color, both, sizeof(both));
    expectColor(both[0], expected, "resized first pixel");
    expectColor(both[1], expected, "resized second pixel");
    renderer.setMaterial(0, ambient({.4f, .3f, .2f, 1}));
    require(renderer.sampleCount() == 0, "material change resets");
    renderer.render(p);
    renderer.setRandomSeed(7654, 12);
    require(renderer.sampleCount() == 0, "seed/data-frame change resets");
    renderer.render(p);
    renderer.setPointLight({1, 2, 3, 1});
    require(renderer.sampleCount() == 0, "light change resets");
    renderer.render(p);
    renderer.resetAccumulation();
    require(renderer.sampleCount() == 0, "explicit accumulation reset");
    renderer.setEnvironment({});
    expectColor(read(runtime, renderer.render(p)).color, p.background, "default upper-hemisphere environment");
    std::puts("PASS constant environment, progressive averaging, resize and scene/camera/light/seed resets");
}
void shadowAttenuation(ComputeRuntime &runtime, s::SlangSceneRT &renderer) {
    const g::Float4 color{.8f, .4f, .2f, 1};
    auto scene = meshScene(color, 0);
    scene.materials[0].shadowBias = 0;
    scene.volumes = {volume(runtime, 1)};
    scene.instances.push_back(instance(s::SceneInstance::Kind::Volume, 0, 0, -1, 2, 0));
    renderer.setScene(scene);
    renderer.setEnvironment({});
    // The shaded position has the retained mesh normal offset of -.0001 on X.
    // An exactly +Y light ray enters one width-2 leaf after one world unit.
    renderer.setPointLight({-.0001f, 10, 1, 1});
    const auto shaded = read(runtime, renderer.render(camera()));
    const float opacity = packedChannel(1 - std::exp(-.2f * 7 * .25f));
    const float transmission = (1 - opacity) * (1 - opacity);
    expectColor(shaded.color, {color.x * transmission, color.y * transmission, color.z * transmission, 1},
                "two deep shadow rays multiply packed transmittance");
    // A separate opaque blocker intersects the shadow ray but not the primary.
    scene.volumes.clear();
    scene.instances.resize(1);
    s::SceneMesh blocker;
    blocker.positions = {
        {-1, 0, -1, 1},
        {0,  0, 1,  1},
        {1,  0, -1, 1}
    };
    blocker.indices = {0, 1, 2};
    scene.meshes.push_back(blocker);
    scene.instances.push_back(instance(s::SceneInstance::Kind::Mesh, 1, 0, 0, 2.5f, 1));
    renderer.setScene(scene);
    expectColor(read(runtime, renderer.render(camera())).color, {0, 0, 0, 1}, "opaque mesh shadow");
    std::puts("PASS deep shadow attenuation and opaque occlusion without primary-ray obstruction");
}
void reflectionAndRefraction(ComputeRuntime &runtime, s::SlangSceneRT &renderer) {
    const g::Float4 target{.2f, .6f, .8f, 1};
    auto scene = meshScene({0, 0, 0, 1});
    scene.materials.push_back(ambient(target));
    scene.instances.push_back(instance(s::SceneInstance::Kind::Mesh, 0, 1, -3, 1, 1));
    auto &mirror = scene.materials[0];
    // Positive width enables the legacy ray branch; this tiny spread remains
    // inside the constant-color target and does not perturb its analytic color.
    mirror.reflWidth = 1e-8f;
    mirror.reflColor = {.5f, .25f, .75f};
    mirror.reflBias = .001f;
    renderer.setScene(scene);
    renderer.setEnvironment({});
    renderer.setPointLight({-10, 100, 1, 1});
    expectColor(read(runtime, renderer.render(camera())).color,
                {target.x * .5f, target.y * .25f, target.z * .75f, 1}, "reflection reaches ambient target");
    scene.materials[0] = ambient({0, 0, 0, 1});
    auto &glass = scene.materials[0];
    glass.refrWidth = 1e-8f;
    glass.refrIor = 1;
    glass.refrBias = .001f;
    glass.refrAmount = .5f;
    glass.refrOffset = 1e20f;
    scene.instances[1].transform[3] = 6;
    renderer.setScene(scene);
    expectColor(read(runtime, renderer.render(camera())).color,
                {target.x * .5f, target.y * .5f, target.z * .5f, 1}, "refraction reaches ambient target");
    // A ratio below one with a 45-degree incident shading normal yields k<0
    // under Snell's law. The retained shader emits no transmitted contribution.
    const float diagonal = std::sqrt(.5f);
    scene.meshes[0].normals.assign(3, {-diagonal, diagonal, 0, 0});
    scene.materials[0].refrIor = .5f;
    renderer.setScene(scene);
    expectColor(read(runtime, renderer.render(camera())).color, {0, 0, 0, 1},
                "total internal reflection suppresses transmission");
    std::puts("PASS reflected/refracted scene targets and total internal reflection");
}
void transformedNormals(ComputeRuntime &runtime, s::SlangSceneRT &renderer) {
    const g::Float4 diffuse{.2f, .4f, .6f, 1};
    auto scene = meshScene({0, 0, 0, 1});
    scene.meshes[0].normals.assign(3, {-1, 1, 0, 0});
    scene.instances[0].transform[0] = 2;
    scene.materials[0].diffColor = {diffuse.x, diffuse.y, diffuse.z};
    renderer.setScene(scene);
    renderer.setEnvironment({});
    // inverse(transpose(diag(2,1,1))) transforms (-1,1,0) to (-.5,1,0).
    // Align the light with that unit vector: Lambert's factor is exactly one.
    const float inverseLength = 1 / std::sqrt(5.f);
    renderer.setPointLight({3 - 10 * inverseLength, 1 + 20 * inverseLength, 1, 1});
    expectColor(read(runtime, renderer.render(camera())).color, diffuse,
                "inverse-transpose normal under nonuniform scale");
    std::puts("PASS inverse-transpose shading normals under nonuniform instance scale");
}
g::Float4 environmentDirection(float u, float v) {
    const float y = 1 - v, radius = std::sqrt(std::max(0.f, 1 - y * y));
    const float angle = u * 3.14159265358979323846f;
    return {std::sin(angle) * radius, y, std::cos(angle) * radius, 0};
}
s::SceneEnvironment checkerEnvironment() {
    return {
        2, 2, {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255}
    };
}
void filteredEnvironment(ComputeRuntime &runtime, s::SlangSceneRT &renderer) {
    auto scene = meshScene();
    scene.instances[0].transform[7] = 100;
    renderer.setScene(scene);
    renderer.setEnvironment(checkerEnvironment());
    auto p = camera();
    p.background = {1, 1, 1, 1};
    const auto sample = [&](float u, float v, g::Float4 expected, const char *message) {
        p.direction00 = environmentDirection(u, v);
        expectColor(read(runtime, renderer.render(p)).color, expected, message);
    };
    sample(.5f, .5f, {.5f, .5f, .5f, 1}, "bilinear environment center");
    sample(.375f, .375f, {.625f, .25f, .25f, 1}, "bilinear environment interior weights");
    sample(0, .5f, {.5f, .5f, .5f, 1}, "environment U zero wraps both edge columns");
    sample(1, .5f, {.5f, .5f, .5f, 1}, "environment U one wraps both edge columns");
    sample(-.25f, .25f, {0, 1, 0, 1}, "negative environment U wraps");
    sample(.75f, .25f, {0, 1, 0, 1}, "positive equivalent wrapped environment coordinate");
    sample(.5f, .01f, {.5f, .5f, 0, 1}, "environment V clamps first row");
    p.direction00 = {1, -.1f, 0, 0};
    expectColor(read(runtime, renderer.render(p)).color, {.1f, .1f, .1f, 1},
                "legacy lower hemisphere environment");
    std::puts("PASS bilinear environment filtering, horizontal wrap and vertical clamp");
}
std::vector<g::Float4> readImage(ComputeRuntime &runtime, const g::RaycastOutput &output) {
    std::vector<g::Float4> result(size_t(output.width) * output.height);
    runtime.read(output.color, result.data(), result.size() * sizeof(result[0]));
    return result;
}
void expectImage(const std::vector<g::Float4> &actual, const std::vector<g::Float4> &expected,
                 const char *message) {
    require(actual.size() == expected.size(), "image dimensions differ");
    for (size_t i = 0; i < actual.size(); ++i)
        expectColor(actual[i], expected[i], message);
}
bool differentImage(const std::vector<g::Float4> &a, const std::vector<g::Float4> &b) {
    for (size_t i = 0; i < a.size(); ++i)
        if (std::abs(a[i].x - b[i].x) > 1e-4f || std::abs(a[i].y - b[i].y) > 1e-4f ||
            std::abs(a[i].z - b[i].z) > 1e-4f)
            return true;
    return false;
}
void randomAccumulation(ComputeRuntime &runtime, s::SlangSceneRT &renderer) {
    auto scene = meshScene();
    scene.instances[0].transform[7] = 100;
    renderer.setScene(scene);
    renderer.setEnvironment(checkerEnvironment());
    auto p = camera();
    p.image = {4, 3, 0, 128};
    p.background = {1, 1, 1, 1};
    p.direction00 = {.7f, .5f, .3f, 0};
    p.directionDx = {.15f, 0, 0, 0};
    p.directionDy = {0, .1f, 0, 0};
    renderer.setRandomSeed(1234, 7);
    const auto first = readImage(runtime, renderer.render(p, 0));
    const auto average = readImage(runtime, renderer.render(p, 1));
    require(differentImage(first, average), "successive random samples must contribute distinct values");
    renderer.resetAccumulation();
    expectImage(readImage(runtime, renderer.render(p, 0)), first, "seed reset reproduces sample zero");
    expectImage(readImage(runtime, renderer.render(p, 1)), average,
                "seed reset reproduces accumulated sample one");
    // The documented implementation advances the per-sample seed by seventeen.
    // Render that next seed independently to verify the arithmetic averaging,
    // rather than deriving the expected image from a second accumulation run.
    renderer.setRandomSeed(1234 + 17, 7);
    auto second = readImage(runtime, renderer.render(p, 0));
    for (size_t i = 0; i < second.size(); ++i)
        second[i] = {(first[i].x + second[i].x) * .5f, (first[i].y + second[i].y) * .5f,
                     (first[i].z + second[i].z) * .5f, 1};
    expectImage(average, second, "progressive image is arithmetic mean of independent samples");
    renderer.setRandomSeed(1235, 7);
    require(differentImage(readImage(runtime, renderer.render(p, 0)), first), "seed changes random image");
    renderer.setRandomSeed(1234, 8);
    require(differentImage(readImage(runtime, renderer.render(p, 0)), first),
            "data frame changes random image");
    renderer.setRandomSeed(1234, 7);
    expectImage(readImage(runtime, renderer.render(p, 0)), first,
                "restored seed and data frame reproduce image");
    std::puts("PASS deterministic jitter, seed/frame changes and independent progressive mean");
}
void invalidInputs(ComputeRuntime &runtime, s::SlangSceneRT &renderer) {
    auto valid = meshScene();
    renderer.setScene(valid);
    auto bad = valid;
    bad.instances[0].geometry = 1;
    reject([&] { renderer.setScene(bad); }, "missing geometry rejected");
    bad = valid;
    bad.instances[0].material = 1;
    reject([&] { renderer.setScene(bad); }, "missing material rejected");
    bad = valid;
    bad.meshes[0].indices[0] = 99;
    reject([&] { renderer.setScene(bad); }, "invalid triangle index rejected");
    bad = valid;
    bad.meshes[0].positions[0].x = std::numeric_limits<float>::quiet_NaN();
    reject([&] { renderer.setScene(bad); }, "nonfinite position rejected");
    rt::AffineTransform singular{};
    reject([&] { renderer.updateTransform(0, singular); }, "singular transform rejected");
    auto material = ambient({1, 1, 1, 1});
    material.refrIor = 0;
    reject([&] { renderer.setMaterial(0, material); }, "invalid refractive index rejected");
    material = ambient({1, 1, 1, 1});
    material.ambColor.x = std::numeric_limits<float>::infinity();
    reject([&] { renderer.setMaterial(0, material); }, "nonfinite material rejected");
    reject(
        [&] {
            renderer.setEnvironment({
                2, 1, {1, 2, 3, 4}
            });
        },
        "environment size mismatch rejected");
    auto withVolume = valid;
    withVolume.volumes.push_back(volume(runtime, 1));
    withVolume.instances.push_back(instance(s::SceneInstance::Kind::Volume, 0, 0, 0, 0, 0));
    bad = withVolume;
    bad.volumes[0].scalar = nullptr;
    reject([&] { renderer.setScene(bad); }, "missing volume resource rejected");
    bad = withVolume;
    bad.volumes[0].scalar = runtime.buffer(4, 4);
    reject([&] { renderer.setScene(bad); }, "undersized scalar atlas rejected");
    bad = withVolume;
    bad.volumes[0].nodes = runtime.buffer(60, 0, nullptr, true);
    reject([&] { renderer.setScene(bad); }, "incomplete node record rejected");
    bad = withVolume;
    bad.volumes[0].children = runtime.buffer(4, 0, nullptr, true);
    reject([&] { renderer.setScene(bad); }, "incomplete child reference rejected");
    bad = withVolume;
    bad.volumes[0].transfer[0].w = -1;
    reject([&] { renderer.setScene(bad); }, "negative extinction transfer rejected");
    bad = withVolume;
    bad.volumes[0].transferLastIndex = uint32_t(bad.volumes[0].transfer.size());
    reject([&] { renderer.setScene(bad); }, "out-of-range transfer endpoint rejected");
    bad = withVolume;
    bad.volumes[0].steps.x = 0;
    reject([&] { renderer.setScene(bad); }, "zero integration step rejected");
    auto p = camera();
    p.march.y = p.march.z;
    reject([&] { renderer.render(p); }, "empty camera clip interval rejected");
    p = camera();
    p.march.y = -1;
    reject([&] { renderer.render(p); }, "negative camera near plane rejected");
    p = camera();
    p.direction00.w = 2;
    reject([&] { renderer.render(p); }, "unknown clip convention rejected");
    p = camera();
    p.direction00.x = std::numeric_limits<float>::quiet_NaN();
    reject([&] { renderer.render(p); }, "nonfinite camera rejected");
    p = camera();
    p.image.x = 0;
    reject([&] { renderer.render(p); }, "zero image width rejected");
    runtime.flush();
    std::puts("PASS invalid geometry, resources, material, transform, environment and camera clipping");
}
int run(IDevice *device, const std::filesystem::path &library) {
    if (!device->queryFeatureSupport(Feature::RayTracingPipeline)) {
        std::puts("SKIP ray tracing unavailable");
        return 77;
    }
    ComputeRuntime runtime(device, library.parent_path());
    s::SlangSceneRT renderer(runtime, library);
    meshMaterials(runtime, renderer);
    deepComposition(runtime, renderer);
    deepCameraClipping(runtime, renderer);
    volumeModes(runtime, renderer);
    volumeArrayEndpoints(runtime, renderer);
    transformAndReplacement(runtime, renderer, library);
    environmentAndAccumulation(runtime, renderer);
    shadowAttenuation(runtime, renderer);
    reflectionAndRefraction(runtime, renderer);
    transformedNormals(runtime, renderer);
    filteredEnvironment(runtime, renderer);
    randomAccumulation(runtime, renderer);
    invalidInputs(runtime, renderer);
    return 0;
}
}  // namespace
int main(int argc, const char **argv) {
    donut::log::ConsoleApplicationMode();
    donut::log::SetCallback([](donut::log::Severity severity, const char *text) {
        if (severity >= donut::log::Severity::Warning)
            ++validationProblems;
        std::fprintf(stderr, "%s\n", text);
    });
    try {
        std::filesystem::path library;
        bool cudaRequested = false, validation = false;
        for (int i = 1; i < argc; ++i) {
            if (!std::strcmp(argv[i], "--library") && i + 1 < argc)
                library = argv[++i];
            else if (!std::strcmp(argv[i], "-cuda"))
                cudaRequested = true;
            else if (!std::strcmp(argv[i], "--validation"))
                validation = true;
        }
        int result = 77;
        if (cudaRequested) {
#if ETHEREAL_TEST_CUDA
            auto messages = MAKE_RC_OBJ_PTR(Messages);
            cuda::DeviceDesc desc;
            desc.errorCB = messages;
            desc.enableOptiX = true;
            auto device = cuda::createDevice(desc);
            if (!device) {
                std::puts("SKIP CUDA unavailable");
                return 77;
            }
            if (library.empty())
                library =
                    donut::app::GetDirectoryWithExecutable() / "shaders/gvdb_slang/optix/SlangSceneRT.bin";
            result = run(device, library);
            device->waitForIdle();
            device = nullptr;
#else
            std::puts("SKIP CUDA backend not built");
            return 77;
#endif
        } else {
            const auto api = donut::app::GetGraphicsAPIFromCommandLine(argc, argv);
            auto manager = TakeOver(donut::app::DeviceManager::Create(api));
            require(bool(manager), "Requested graphics backend unavailable");
            donut::app::DeviceCreationParameters params;
            params.enableNvrhiValidationLayer = true;
            params.enableDebugRuntime = validation;
            params.enableRayTracingExtensions = true;
            require(manager->CreateHeadlessDevice(params), "Graphics device creation failed");
            if (library.empty())
                library = donut::app::GetDirectoryWithExecutable() / "shaders/gvdb_slang" /
                          donut::app::GetShaderTypeName(api) / "SlangSceneRT.bin";
            result = run(manager->GetDevice(), library);
            manager->Shutdown();
            manager = nullptr;
        }
        require(validationProblems == 0, "Native/NVRHI validation reported warnings or errors");
        return result;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
