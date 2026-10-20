// SPDX-License-Identifier: Apache-2.0
#include "OwnedVolumeLayers.h"
#include "OwnedVolumeIO.h"
#include "ExampleDevice.h"
#include <gvdb/slang/GVDBTopologyBuilder.h>
#include <gvdb/slang/GVDBPortableVbx.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
namespace g = gvdb::slang;
namespace p = gvdb::portable;
namespace s = gvdb::sample;
namespace es = ethereal::slang;
namespace {
void require(bool value, const char* text) {
    if (!value)
        throw std::runtime_error(text);
}
p::SnapshotHandle fixture(es::ComputeRuntime& runtime, const std::filesystem::path& remap, float density) {
    g::VbxRawVolume raw;
    g::TopologyBuildOptions options;
    options.logDimensions = {3};
    options.atlasGrid = {1, 1, 1, 0};
    std::string error;
    require(g::buildTopology(
                {
                    {-8, -8, -8, 0}
    },
                options, raw.packed, error),
            error.c_str());
    raw.channels.resize(1);
    raw.channels[0].format = g::AtlasFormat::R32_FLOAT;
    raw.channels[0].bytes.resize(4000);
    for (size_t i = 0; i < 1000; ++i)
        std::memcpy(raw.channels[0].bytes.data() + i * 4, &density, 4);
    std::vector<uint8_t> encoded;
    require(g::saveRawVBX(raw, encoded, error), error.c_str());
    runtime.flush();
    return s::loadOwnedVBX(runtime.device(), remap, encoded.data(), encoded.size());
}
uint32_t pack(g::Float4 value) {
    const float channels[] = {value.x, value.y, value.z, value.w};
    uint32_t result = 0;
    for (uint32_t c = 0; c < 4; ++c)
        result |= uint32_t(std::clamp(channels[c], 0.f, 1.f) * 255) << (c * 8);
    return result;
}
uint32_t over(uint32_t source, uint32_t destination) {
    uint32_t result = 0;
    for (uint32_t c = 0; c < 4; ++c)
        result |= std::min(255u, (((source >> (c * 8)) & 255) * 255 +
                                  ((destination >> (c * 8)) & 255) * (255 - (source >> 24))) /
                                     255)
                  << (c * 8);
    return result;
}
uint32_t deep(g::Float4 transfer, g::Float4 background) {
    // One width-eight brick, direct step .25, positive entry bias: 31 samples.
    const float transmission = std::exp(-transfer.w * .25f * 31), alpha = 1 - transmission;
    return pack({background.x * transmission + transfer.x * alpha * alpha,
                 background.y * transmission + transfer.y * alpha * alpha,
                 background.z * transmission + transfer.z * alpha * alpha, alpha});
}
void run(es::ComputeRuntime& runtime, const std::filesystem::path& remap) {
    const g::Float4 background{.1f, .2f, .3f, 1}, red{.8f, .1f, .2f, .25f}, green{.1f, .7f, .3f, .125f};
    auto check = [&](s::LayeredVolumeOutput output, uint32_t expected, uint32_t error = 0) {
        std::vector<uint32_t> values(size_t(output.width) * output.height);
        runtime.read(output.rgba8, values.data(), values.size() * 4);
        uint32_t diagnostics;
        runtime.read(output.diagnostics, &diagnostics, 4);
        require(diagnostics == error, "Layer diagnostics");
        for (auto value : values)
            if (value != expected) {
                std::fprintf(stderr, "Layer pixel %08x expected %08x\n", value, expected);
                throw std::runtime_error("Layer exact byte equation");
            }
    };
    auto render = [&](const std::vector<s::OwnedVolumeLayer>& layers) {
        return s::renderOwnedLayers(runtime, 17, 9, background, layers);
    };
    check(render({}), pack(background));
    s::OwnedVolumeLayer off;
    off.shading = s::LayerShading::Off;
    check(render({off}), 0);
    check(render(std::vector<s::OwnedVolumeLayer>(64, off)), 0);
    s::OwnedVolumeLayer a;
    a.snapshot = fixture(runtime, remap, .8f);
    a.transfer.assign(16384, red);
    a.camera.image = {1, 1, 0, 256};
    a.camera.rayOrigin = {-10, -4, -4, 0};
    a.camera.direction00 = {1, 0, 0, 0};
    a.camera.march = {.25f, 0, 32, 1};
    a.camera.valueRange = {0, 1, .1f, 1};
    a.camera.light = {-12, -4, -4, 0};
    a.modes.albedo = 1;
    a.modes.extinction = 1;
    a.modes.alphaCutoff = .001f;
    auto b = a;
    b.snapshot = fixture(runtime, remap, .4f);
    b.transfer.assign(16384, green);
    const auto first = deep(red, background), second = deep(green, {});
    check(render({a}), first);
    check(render({a, b}), over(second, first));
    check(render({b, a}), over(deep(red, {}), deep(green, background)));
    check(render({off, b}), second);
    check(render({a, off}), first);
    b.camera.rayOrigin.y = 4;
    check(render({a, b}), first);  // Transparent deep miss preserves the first layer.
    b.shading = s::LayerShading::Section2D;
    b.section.point = {0, 4, 0, 0};
    check(render({a, b}), 0xff000000u);  // Retained section miss is opaque black.
    b = a;
    b.modes.maxLeafSteps = 1;
    check(render({b}), pack({background.x, background.y, background.z, 0}), 4);
    auto retained = render({a});
    a.snapshot = nullptr;
    b.snapshot = nullptr;
    check(retained, first);  // Queue ownership survives input/result temporaries.
    auto reject = [&](auto operation) {
        bool rejected = false;
        try {
            operation();
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, "Invalid layered render accepted");
    };
    reject([&] { render({a}); });
    reject([&] { render(std::vector<s::OwnedVolumeLayer>(65, off)); });
    reject([&] { s::renderOwnedLayers(runtime, UINT32_MAX, UINT32_MAX, background, {}); });
    reject([&] { s::renderOwnedLayers(runtime, 0, 1, background, {}); });
    reject([&] { s::renderOwnedLayers(runtime, 1, 1, {NAN, 0, 0, 0}, {}); });
    off.shading = s::LayerShading(999);
    reject([&] { render({off}); });
    std::puts(
        "PASS ordered layers, independent deep equation, exact bytes, background/Off/miss alpha, tail, "
        "lifetime, exhaustion and validation");
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
