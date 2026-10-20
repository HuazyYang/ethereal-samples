// SPDX-License-Identifier: Apache-2.0
#include "OwnedVolumeModes.h"
#include "OwnedVolumeIO.h"
#include "ExampleDevice.h"
#include <gvdb/slang/GVDBTopologyBuilder.h>
#include <gvdb/slang/GVDBPortableVbx.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
namespace g = gvdb::slang;
namespace p = gvdb::portable;
namespace s = gvdb::sample;
namespace es = ethereal::slang;
namespace {
void require(bool ok, const char* text) {
    if (!ok)
        throw std::runtime_error(text);
}
void expectByte(float a, float b, const char* text) {
    if (!std::isfinite(a) || std::abs(a - b) > 1.01f / 255) {
        std::fprintf(stderr, "%s: %g expected %g\n", text, a, b);
        throw std::runtime_error(text);
    }
}
float byte(float v) { return float(uint32_t(std::clamp(v, 0.f, 1.f) * 255)) / 255; }
p::SnapshotHandle fixture(es::ComputeRuntime& runtime, const std::filesystem::path& remap,
    g::Int4 origin={-8,-8,-8,0}) {
    g::VbxRawVolume raw;
    g::TopologyBuildOptions options;
    options.logDimensions = {3};
    options.atlasGrid = {1, 1, 1, 0};
    std::string error;
    require(g::buildTopology({origin}, options, raw.packed, error),
            error.c_str());
    raw.channels.resize(2);
    raw.channels[0].format = g::AtlasFormat::R32_FLOAT;
    raw.channels[1].format = g::AtlasFormat::RGBA8_UINT;
    raw.channels[0].bytes.resize(4000);
    raw.channels[1].bytes.resize(4000);
    for (int z = 0; z < 10; ++z)
        for (int y = 0; y < 10; ++y)
            for (int x = 0; x < 10; ++x) {
                const float scalar = .1f * (x - .5f);
                const uint32_t color = 0xff804020;
                const size_t offset = ((z * 10 + y) * 10 + x) * 4;
                std::memcpy(raw.channels[0].bytes.data() + offset, &scalar, 4);
                std::memcpy(raw.channels[1].bytes.data() + offset, &color, 4);
            }
    std::vector<uint8_t> encoded;
    require(g::saveRawVBX(raw, encoded, error), error.c_str());
    runtime.flush();
    return s::loadOwnedVBX(runtime.device(), remap, encoded.data(), encoded.size());
}

void modes(es::ComputeRuntime& runtime, const std::filesystem::path& remap) {
    auto snapshot = fixture(runtime, remap);
    g::RaycastParameters camera{};
    camera.image = {1, 1, 0, 256};
    camera.rayOrigin = {-10, -4, -4, 0};
    camera.direction00 = {1, 0, 0, 0};
    camera.march = {.25f, 0, 32, 1};
    camera.valueRange = {0, 1, .32f, 1};
    camera.background = {.1f, .2f, .3f, 1};
    camera.light = {-12, -4, -4, 0};
    std::vector<g::Float4> transfer(16384, {.4f, .2f, .1f, .5f});
    s::VolumeModeOptions settings;
    auto render = [&](uint32_t color = UINT32_MAX) {
        auto image = s::renderOwnedMode(runtime, snapshot, 0, color, camera, transfer, settings);
        g::Float4 pixel;
        float depth;
        runtime.read(image.color, &pixel, 16);
        runtime.read(image.depth, &depth, 4);
        return std::pair{pixel, depth};
    };
    auto result = render();
    require(std::abs(result.second - 5.25f) < 1e-4f, "Trilinear threshold sample wavefront");
    expectByte(result.first.x, 1, "Trilinear outward phong");
    auto surface = result;
    settings.mode = s::VolumeMode::SurfaceDepth;
    result = render();
    require(std::memcmp(&surface.first, &result.first, 16) == 0 && result.second == surface.second,
            "Legacy depth entry aliases trilinear shading");
    settings.mode = s::VolumeMode::Voxel;
    result = render();
    require(std::abs(result.second - 5) < 1e-4f, "Voxel face hit depth");
    expectByte(result.first.x, 1, "Voxel face normal");
    result = render(1);
    expectByte(result.first.x, 32.f / 255, "Optional RGBA8 color");
    settings.maxLeafSteps = 1;
    result = render();
    require(result.second == -1, "Voxel exhaustion");
    settings.maxLeafSteps = 256;
    settings.mode = s::VolumeMode::EmptySkip;
    camera.valueRange.z = 10;
    result = render();
    require(std::abs(result.second - 2.01f) < 1e-4f, "Empty skip active leaf ignores scalar");
    require(result.first.x == 0 && result.first.y == 0 && result.first.z == 0 && result.first.w == 1,
            "Negative diagnostic coordinates saturate");
    snapshot=fixture(runtime,remap,{128,-8,-8,0});
    camera.rayOrigin.x=126;
    result=render();
    expectByte(result.first.x,70.f/255,"Positive empty-skip diagnostic low-byte wrap");
    snapshot=fixture(runtime,remap);
    settings.mode = s::VolumeMode::LevelSet;
    camera.valueRange.z = .32f;
    camera.rayOrigin = {2, -4, -4, 0};
    camera.direction00 = {-1, 0, 0, 0};
    camera.light = {12, -4, -4, 0};
    result = render();
    require(std::abs(result.second - 7.01f) < 1e-3f, "Level-set strict below and biased fixed step");
    expectByte(result.first.x, 1, "Level-set positive gradient");
    settings.maxLeafSteps = 1;
    result = render();
    require(result.second == -1, "Level-set exhaustion");
    settings.maxLeafSteps = 256;
    camera.rayOrigin = {-10, -4, -4, 0};
    camera.direction00 = {1, 0, 0, 0};
    camera.light = {-12, -4, -4, 0};
    settings.mode = s::VolumeMode::Trilinear;
    camera.march.x = 2;
    result = render();
    require(result.second == 6, "Configurable direct step");
    camera.march.x = .25f;
    settings.epsilon = 0;
    settings.shadowBias = 0;
    result = render();
    expectByte(result.first.x, byte(.28f), "Explicit self-shadow amount and bias");
    settings.shadowAmount = 0;
    result = render();
    expectByte(result.first.x, 1, "Disabled shadow");
    settings = {};
    settings.mode = s::VolumeMode::Deep;
    auto deepExpected = [&](unsigned samples, float scale) {
        const float alpha = 1 - std::exp(-settings.extinction * .5f * camera.march.x * samples);
        const float accumulated = std::min(1.f, .4f * settings.albedo * alpha * scale);
        return std::pair{byte(camera.background.x * (1 - alpha) + accumulated * alpha), byte(alpha)};
    };
    result = render();
    auto expected = deepExpected(31, 1);
    expectByte(result.first.x, expected.first, "Deep Beer-Lambert color integration");
    expectByte(result.first.w, expected.second, "Deep transparency output");
    require(result.second == 2.25f, "Deep first contributing depth");
    result = render(1);
    expected = deepExpected(31, 32.f / 255);
    expectByte(result.first.x, expected.first, "Deep color-channel modulation");
    camera.march.z = 4;
    result = render();
    expected = deepExpected(7, 1);
    expectByte(result.first.x, expected.first, "Deep maximum distance clipping");
    expectByte(result.first.w, expected.second, "Deep clipped alpha");
    camera.march.z = 32;
    settings.minimumValue = 1;
    result = render();
    expectByte(result.first.x, byte(.1f), "Deep minimum-value rejection");
    require(result.first.w == 0 && std::isinf(result.second), "Deep empty alpha/depth");
    settings.minimumValue = .005f;
    settings.extinction = 0;
    result = render();
    require(result.first.w == 0, "Zero extinction");
    settings.extinction = 1;
    settings.maxLeafSteps = 1;
    result = render();
    require(result.second == -1, "Deep step exhaustion is not a miss");
    settings.maxLeafSteps = 256;
    settings.alphaCutoff = 1;
    result = render();
    require(result.first.w == 0, "Alpha cutoff control");
    settings.alphaCutoff = .01f;
    camera.rayOrigin.y = 10;
    for (unsigned mode = 0; mode < 7; ++mode) {
        settings.mode = s::VolumeMode(mode);
        result = render();
        require(std::isinf(result.second), "Every mode sparse miss depth");
        expectByte(result.first.x, mode == 0 ? 0 : byte(.1f), "Sparse miss background");
        require(result.first.w == ((mode == 0 || mode == 4) ? 0 : 1), "Mode-specific miss alpha");
    }
    camera.rayOrigin.y = -4;
    camera.direction00 = {0, 0, 0, 0};
    result = render();
    require(std::isinf(result.second), "Zero ray direction miss");
    camera.direction00 = {1, 0, 0, 0};
    settings.mode = s::VolumeMode::Trilinear;
    settings.shadowAmount = NAN;
    bool rejected = false;
    try {
        render();
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "Nonfinite controls rejected");
    std::puts("PASS seven rendering modes, equations, controls, color, clipping, miss and exhaustion");
}
}  // namespace
int main(int argc, const char** argv) {
    try {
        es::ExampleDevice device(argc, argv);
        const auto path = donut::app::GetDirectoryWithExecutable() / "shaders/gvdb_owned" /
                          donut::app::GetShaderTypeName(device.get()->getGraphicsAPI());
        es::ComputeRuntime runtime(device.get(), path);
        modes(runtime, path / "GVDBAtlasRepack_relocateAtlas.bin");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
