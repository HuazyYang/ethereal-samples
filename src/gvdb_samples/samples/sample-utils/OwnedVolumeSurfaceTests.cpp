// Independent sparse/color equations, retained typed renderer oracle, and snapshot-only viewer.
// SPDX-License-Identifier: Apache-2.0
#include "OwnedVolumeSurface.h"
#include "OwnedVolumeIO.h"
#include "ExampleDevice.h"
#include "VolumeSurface.h"
#include "VolumeViewer.h"
#include <gvdb/slang/GVDBPortableVbx.h>
#include <gvdb/slang/GVDBTopologyBuilder.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
namespace g = gvdb::slang;
namespace p = gvdb::portable;
namespace s = gvdb::sample;
namespace es = ethereal::slang;
namespace
{
void require(bool v, const char *m)
{
    if (!v)
        throw std::runtime_error(m);
}
void check(nvrhi::FRESULT v, const char *m)
{
    require(NVRHI_SUCCEEDED(v), m);
}
void nearValue(float a, float b, const char *m)
{
    if (std::isinf(b))
    {
        require(std::isinf(a) && a > 0, m);
        return;
    }
    if (!std::isfinite(a) || std::abs(a - b) > 2e-4f)
    {
        std::fprintf(stderr, "%s actual %.9g expected %.9g\n", m, a, b);
        throw std::runtime_error(m);
    }
}
const std::vector<g::Float4> transfer{{.2f, .4f, .6f, 1}, {.2f, .4f, .6f, 1}};
struct Image
{
    std::vector<g::Float4> color;
    std::vector<float> depth;
};
Image read(es::ComputeRuntime &runtime, const g::RaycastOutput &output)
{
    Image result;
    result.color.resize(size_t(output.width) * output.height);
    result.depth.resize(result.color.size());
    runtime.read(output.color, result.color.data(), result.color.size() * 16);
    runtime.read(output.depth, result.depth.data(), result.depth.size() * 4);
    for (const auto &c : result.color)
        require(std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z) && std::isfinite(c.w),
                "nonfinite surface pixel");
    for (float z : result.depth)
        require(std::isfinite(z) || (std::isinf(z) && z > 0), "invalid surface depth");
    return result;
}
void equal(const Image &a, const Image &b)
{
    require(a.color.size() == b.color.size(), "oracle image size");
    // Both shaders execute the same retained equations; only the atlas load type changes.
    require(!std::memcmp(a.color.data(), b.color.data(), a.color.size() * 16) &&
                !std::memcmp(a.depth.data(), b.depth.data(), a.depth.size() * 4),
            "raw/typed surface result differs");
}
void color(const Image &image, float r, float green, float blue)
{
    nearValue(image.color[0].x, r, "surface red");
    nearValue(image.color[0].y, green, "surface green");
    nearValue(image.color[0].z, blue, "surface blue");
    nearValue(image.color[0].w, 1, "surface alpha");
}
void miss(const Image &image, const g::RaycastParameters &q)
{
    nearValue(image.depth[0], INFINITY, "sparse miss depth");
    nearValue(image.color[0].x, q.background.x, "miss red");
    nearValue(image.color[0].y, q.background.y, "miss green");
    nearValue(image.color[0].z, q.background.z, "miss blue");
    nearValue(image.color[0].w, q.background.w, "miss alpha");
}
struct Fixture
{
    g::VbxRawVolume input;
    p::SnapshotHandle snapshot;
    std::unique_ptr<g::SlangVolume> typed;
    nvrhi::BufferHandle typedColor;
    g::Int4 shift;
    Fixture(es::ComputeRuntime &runtime, nvrhi::IDevice *device, const std::filesystem::path &remap, g::Int4 offset,
            int mode)
        : shift(offset)
    {
        g::TopologyBuildOptions o;
        o.logDimensions = {2, 2};
        o.atlasGrid = {3, 1, 1, 0};
        std::string error;
        require(g::buildTopology(
                    {{-8 + shift.x, shift.y, shift.z, 0}, {shift.x + (mode == 3 ? -4 : 0), shift.y, shift.z, 0}}, o,
                    input.packed, error),
                error.c_str());
        const auto res = input.packed.volume.atlasResolutionAndApron;
        // Scalar aprons are deliberately stale zero, including borders adjoining holes.
        // The third unused slot has conspicuous color and must never be sampled.
        std::vector<float> scalar(size_t(res.x) * res.y * res.z, 0);
        std::vector<uint32_t> rgba(scalar.size(), 0xffff00ff);
        const auto &level = input.packed.levels[0];
        for (uint32_t i = 0; i < level.nodes.z; ++i)
        {
            const auto *node = input.packed.nodes.data() + (level.nodes.x + size_t(i) * level.nodes.y) / 4;
            for (uint32_t z = 0; z < 4; ++z)
                for (uint32_t y = 0; y < 4; ++y)
                    for (uint32_t x = 0; x < 4; ++x)
                    {
                        const auto address = ((size_t(node[6]) + z) * res.y + node[5] + y) * res.x + node[4] + x;
                        scalar[address] = mode == 3   ? float(int32_t(node[1]) - shift.x) + float(x) + 4.5f
                                          : mode == 2 ? 0.f
                                          : mode == 1 ? 2.f
                                          : int32_t(node[1]) == shift.x - 8 ? float(x) - 1.75f
                                                                            : 3.f;
                        rgba[address] = (32u + 32u * x) | (64u << 8) | (192u << 16) | (255u << 24);
                    }
        }
        input.channels.resize(2);
        input.channels[0].format = g::AtlasFormat::R32_FLOAT;
        input.channels[1].format = g::AtlasFormat::RGBA8_UINT;
        input.channels[0].bytes.resize(scalar.size() * 4);
        input.channels[1].bytes.resize(rgba.size() * 4);
        std::memcpy(input.channels[0].bytes.data(), scalar.data(), scalar.size() * 4);
        std::memcpy(input.channels[1].bytes.data(), rgba.data(), rgba.size() * 4);
        std::vector<uint8_t> encoded;
        require(g::saveRawVBX(input, encoded, error), error.c_str());
        runtime.flush();
        snapshot = s::loadOwnedVBX(device, remap, encoded.data(), encoded.size());
        typed = std::make_unique<g::SlangVolume>(runtime, input.packed, scalar.data(), scalar.size());
        typedColor = runtime.buffer(rgba.size() * 4, 0, rgba.data(), true);
    }
    g::RaycastParameters parameters() const
    {
        g::RaycastParameters q{};
        q.image = {1, 1, 0, 128};
        q.march = {.25f, 0, 64, 1};
        q.valueRange = {-2, 3, 0, 1};
        q.rayOrigin = {float(shift.x) - 7, float(shift.y) + 1.5f, float(shift.z) + 1.5f, 0};
        q.direction00 = {1, 0, 0, 0};
        q.background = {.03f, .07f, .11f, .4f};
        q.light = {float(shift.x) + 4, float(shift.y) + 1.5f, float(shift.z) + 1.5f, 0};
        return q;
    }
};
Image compare(es::ComputeRuntime &runtime, s::OwnedVolumeSurface &renderer, Fixture &fixture, g::RaycastParameters q,
              bool useColor = true, float background = 3)
{
    auto oracle = read(runtime, s::renderVolumeSurface(runtime, *fixture.typed, q, transfer,
                                                       useColor ? fixture.typedColor.Get() : nullptr, background));
    auto result =
        read(runtime, renderer.render(fixture.snapshot, 0, useColor ? 1 : UINT32_MAX, q, transfer, background));
    equal(result, oracle);
    return result;
}
void plane(es::ComputeRuntime &runtime, nvrhi::IDevice *device, s::OwnedVolumeSurface &renderer,
           const std::filesystem::path &remap, g::Int4 shift)
{
    Fixture f(runtime, device, remap, shift, 0);
    auto q = f.parameters();
    auto result = compare(runtime, renderer, f, q);
    nearValue(result.depth[0], 1.25f, "inside level-set crossing");
    color(result, 96.f / 255, 64.f / 255, 192.f / 255);
    q.rayOrigin.x = float(shift.x) - 4.75f;
    q.direction00 = {-1, 0, 0, 0};
    result = compare(runtime, renderer, f, q);
    nearValue(result.depth[0], 1, "reverse crossing");
    q.rayOrigin.x = float(shift.x) - 5.75f;
    result = compare(runtime, renderer, f, q);
    nearValue(result.depth[0], 0, "exact start crossing");
    q = f.parameters();
    q.light = {float(shift.x) - 10, float(shift.y) + 1.5f, float(shift.z) + 1.5f, .25f};
    color(compare(runtime, renderer, f, q), 24.f / 255, 16.f / 255, 48.f / 255);
    q = f.parameters();
    color(compare(runtime, renderer, f, q, false), .2f, .4f, .6f);
    q.direction00 = {0, 0, 0, 0};
    miss(compare(runtime, renderer, f, q), q);
    q = f.parameters();
    q.image.x = 3;
    q.rayOrigin.y = float(shift.y) + .5f;
    q.direction00 = {1, 2, 0, 1};
    q.directionDx = {0, -4.f / 3, 0, 0};
    q.march.y = .1f;
    q.march.z = 1.5f;
    q.light.w = 1;
    result = compare(runtime, renderer, f, q);
    nearValue(result.depth[0], 25.f / 12, "off-axis view-Z far clipping");
    q.march.y = 1.3f;
    miss(compare(runtime, renderer, f, q), q);
    q.march.y = .1f;
    q.direction00.w = 0;
    miss(compare(runtime, renderer, f, q), q);
    const auto reject = [&](auto call, const char *message) {
        bool rejected = false;
        try
        {
            call();
        }
        catch (const std::exception &)
        {
            rejected = true;
        }
        require(rejected, message);
    };
    q = f.parameters();
    reject([&] { renderer.render(f.snapshot, 0, 1, q, transfer, NAN); }, "NaN background accepted");
    reject([&] { renderer.render(f.snapshot, 1, UINT32_MAX, q, transfer, 3); }, "RGBA scalar accepted");
    reject([&] { renderer.render(f.snapshot, 0, 0, q, transfer, 3); }, "scalar as color accepted");
    reject([&] { renderer.render(f.snapshot, 0, 31, q, transfer, 3); }, "missing color accepted");
    reject([&] { renderer.render({}, 0, 1, q, transfer, 3); }, "null snapshot accepted");
    for (uint32_t axis = 0; axis < 2; ++axis)
    {
        auto invalid = q;
        if (axis)
            invalid.image.y = 65535u * 8u + 1;
        else
            invalid.image.x = 65535u * 8u + 1;
        bool rejected = false;
        try
        {
            renderer.render(f.snapshot, 0, 1, invalid, transfer, 3);
        }
        catch (const std::invalid_argument &e)
        {
            rejected = std::strcmp(e.what(), "Owned surface dispatch dimensions") == 0;
        }
        require(rejected, "dispatch axis did not reject before allocation");
    }
}
std::vector<char> bytes(const std::filesystem::path &path)
{
    std::ifstream f(path, std::ios::binary);
    require(bool(f), "open remap artifact");
    return {(std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()};
}
p::SnapshotHandle empty(nvrhi::IDevice *device, const std::filesystem::path &remap)
{
    auto artifact = bytes(remap);
    p::VolumeCreateDesc desc;
    desc.device = device;
    desc.configuration.levelCount = 2;
    desc.configuration.logDimension[0] = 2;
    desc.configuration.logDimension[1] = 2;
    desc.remapArtifact = artifact.data();
    desc.remapArtifactBytes = artifact.size();
    p::VolumeHandle owner;
    check(p::createVolume(desc, &owner), "empty owner");
    p::ChannelDesc channel;
    check(owner->setChannel(0, channel), "empty scalar");
    channel.format = p::AtlasFormat::RGBA8_UINT;
    check(owner->setChannel(1, channel), "empty color");
    check(owner->finishTopology(), "empty topology");
    p::GenerationTicketHandle ticket;
    check(owner->prepareGeneration({}, &ticket), "empty prepare");
    nvrhi::CommandListHandle list;
    check(device->createCommandList({}, &list), "empty list");
    list->open();
    check(owner->recordAndPublish({device, list}, ticket), "empty publication");
    list->close();
    device->executeCommandList(list);
    p::SnapshotHandle snapshot;
    check(owner->getSnapshot(&snapshot), "empty snapshot");
    return snapshot;
}
class SnapshotScene : public s::VolumeScene
{
    es::ComputeRuntime &m_runtime;
    s::OwnedVolumeSurface &m_renderer;
    p::SnapshotHandle m_snapshot;

  public:
    bool lines = false, points = false, topology = false;
    SnapshotScene(es::ComputeRuntime &runtime, s::OwnedVolumeSurface &renderer, p::SnapshotHandle snapshot)
        : m_runtime(runtime), m_renderer(renderer), m_snapshot(std::move(snapshot))
    {
    }
    g::VolumeInfo volumeInfo() override
    {
        return m_snapshot->getDesc().volume;
    }
    std::vector<g::Float4> transfer() const override
    {
        return ::transfer;
    }
    g::RaycastOutput renderCustom(const g::RaycastParameters &q, const s::VolumeViewOptions &) override
    {
        m_runtime.flush();
        return m_renderer.render(m_snapshot, 0, 1, q, ::transfer, 3);
    }
    bool topologyOverlay(std::vector<s::VolumeOverlayLine> &output) const override
    {
        if (topology)
            output = {{{-5.75f, .5f, 1.5f, 0}, {-5.75f, 2.5f, 1.5f, 0}, {1, 0, 1, 1}}};
        return true;
    }
    std::vector<s::VolumeOverlayLine> overlayLines() const override
    {
        return lines
                   ? std::vector<s::VolumeOverlayLine>{{{-5.75f, 1.5f, .5f, 0}, {-5.75f, 1.5f, 2.5f, 0}, {1, 0, 0, 1}}}
                   : std::vector<s::VolumeOverlayLine>{};
    }
    std::vector<s::VolumeOverlayPoint> overlayPoints() const override
    {
        return points ? std::vector<s::VolumeOverlayPoint>{{{-5.75f, 1.5f, 1.5f, 0}, {0, 1, 0, 1}}}
                      : std::vector<s::VolumeOverlayPoint>{};
    }
};
void viewer(es::ComputeRuntime &runtime, s::OwnedVolumeSurface &renderer, p::SnapshotHandle snapshot)
{
    SnapshotScene scene(runtime, renderer, std::move(snapshot));
    s::VolumeViewOptions options;
    options.mode = s::VolumeViewMode::Surface;
    options.width = 9;
    options.height = 9;
    options.explicitCamera = true;
    options.cameraEye = {-7, 1.5f, 1.5f, 0};
    options.cameraForward = {1, 0, 0, 0};
    options.cameraUp = {0, 1, 0, 0};
    options.cameraNear = .01f;
    options.cameraFar = 32;
    options.step = .25f;
    options.isovalue = 0;
    options.valueMin = -2;
    options.valueMax = 3;
    options.scalarBackground = 3;
    auto baseline = read(runtime, s::renderVolumeView(runtime, scene, options));
    options.topology = true;
    equal(baseline,
          read(runtime, s::renderVolumeView(runtime, scene, options))); // True/empty hook must not ask for packed().
    scene.topology = true;
    auto top = read(runtime, s::renderVolumeView(runtime, scene, options));
    size_t changed = 0;
    for (size_t i = 0; i < top.color.size(); ++i)
    {
        nearValue(top.depth[i], baseline.depth[i], "topology altered depth");
        if (std::memcmp(&top.color[i], &baseline.color[i], 16))
        {
            ++changed;
            color({{top.color[i]}, {top.depth[i]}}, 1, 0, 1);
        }
    }
    require(changed > 0, "snapshot topology overlay not rendered");
    scene.lines = true;
    scene.points = true;
    const auto overlaid = read(runtime, s::renderVolumeView(runtime, scene, options));
    bool green = false, red = false;
    for (size_t i = 0; i < overlaid.color.size(); ++i)
    {
        nearValue(overlaid.depth[i], baseline.depth[i], "scene overlay altered depth");
        const auto c = overlaid.color[i];
        green |= c.x == 0 && c.y == 1 && c.z == 0;
        red |= c.x == 1 && c.y == 0 && c.z == 0;
    }
    require(green && red, "custom surface line/point overlay composition missing");
    options.sectionInset = true;
    bool rejected = false;
    try
    {
        s::renderVolumeView(runtime, scene, options);
    }
    catch (const std::logic_error &)
    {
        rejected = true;
    }
    require(rejected, "unsupported snapshot legacy inset silently accepted");
    runtime.flush();
}
} // namespace
int main(int argc, const char **argv)
{
    try
    {
        es::ExampleDevice device(argc, argv);
        es::ComputeRuntime runtime(device.get(), device.shaderPath());
        auto path = donut::app::GetDirectoryWithExecutable() / "shaders/gvdb_owned" /
                    donut::app::GetShaderTypeName(device.get()->getGraphicsAPI());
        for (int i = 1; i + 1 < argc; ++i)
            if (!std::strcmp(argv[i], "--artifacts"))
                path = std::filesystem::path(argv[++i]) / donut::app::GetShaderTypeName(device.get()->getGraphicsAPI());
        const auto production = donut::app::GetDirectoryWithExecutable() / "shaders/gvdb_owned" /
                                donut::app::GetShaderTypeName(device.get()->getGraphicsAPI());
        const auto remap = production / "GVDBAtlasRepack_relocateAtlas.bin",
                   artifact = path / "OwnedVolumeSurface_renderSurface.bin";
        bool rejected = false;
        try
        {
            s::OwnedVolumeSurface invalid(device.get(), device.shaderPath() / "VolumeSurface_renderSurface.bin");
        }
        catch (const std::invalid_argument &)
        {
            rejected = true;
        }
        require(rejected, "typed artifact accepted as raw surface ABI");
        auto renderer = std::make_unique<s::OwnedVolumeSurface>(device.get(), artifact);
        plane(runtime, device.get(), *renderer, remap, {0, 0, 0, 0});
        plane(runtime, device.get(), *renderer, remap, {12, -8, 4, 0});
        Fixture positive(runtime, device.get(), remap, {0, 0, 0, 0}, 1);
        auto q = positive.parameters();
        q.rayOrigin.x = -10;
        miss(compare(runtime, *renderer, positive, q), q);
        q.rayOrigin = {-2, 1.5f, -1, 0};
        q.direction00 = {0, 0, 1, 0};
        miss(compare(runtime, *renderer, positive, q), q);
        miss(compare(runtime, *renderer, positive, q, true, 0), q);
        Fixture adjacent(runtime, device.get(), remap, {0, 0, 0, 0}, 3);
        q = adjacent.parameters();
        q.rayOrigin.x = -5;
        q.light.w = 1;
        auto result = compare(runtime, *renderer, adjacent, q);
        nearValue(result.depth[0], 1, "adjacent leaf boundary");
        color(result, 32.f / 255, 64.f / 255, 192.f / 255);
        Fixture flat(runtime, device.get(), remap, {0, 0, 0, 0}, 2);
        q = flat.parameters();
        q.light = q.rayOrigin;
        q.light.w = .25f;
        result = compare(runtime, *renderer, flat, q);
        nearValue(result.depth[0], 0, "flat zero gradient depth");
        Fixture fixture(runtime, device.get(), remap, {0, 0, 0, 0}, 0);
        runtime.flush();
        viewer(runtime, *renderer, fixture.snapshot);
        auto noLeaves = empty(device.get(), remap);
        q = fixture.parameters();
        miss(read(runtime, renderer->render(noLeaves, 0, 1, q, transfer, 3)), q);
        viewer(runtime, *renderer, noLeaves);
        // Drop the snapshot, owner (already gone after load), and renderer before
        // execution completion/readback. The submitted binding set retains all inputs.
        q = fixture.parameters();
        auto output = renderer->render(fixture.snapshot, 0, 1, q, transfer, 3);
        fixture.snapshot = nullptr;
        renderer.reset();
        result = read(runtime, output);
        nearValue(result.depth[0], 1.25f, "released owner/renderer lifetime");
        color(result, 96.f / 255, 64.f / 255, 192.f / 255);
        std::puts(
            "Owned sparse surface raw/typed, analytic color/hole/clip, snapshot overlays and release lifetime PASS");
        return 0;
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
