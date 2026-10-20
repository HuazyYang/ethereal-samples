// SPDX-License-Identifier: Apache-2.0
#include "OwnedVolumeSections.h"
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
p::SnapshotHandle fixture(es::ComputeRuntime& runtime, const std::filesystem::path& remap) {
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
void sections(es::ComputeRuntime& runtime, const std::filesystem::path& remap) {
    auto snapshot = fixture(runtime, remap);
    g::RaycastParameters camera{};
    camera.image = {17, 9, 0, 256};
    camera.rayOrigin = {-10, -4, -4, 0};
    camera.direction00 = {1, 0, 0, 0};
    camera.march = {.25f, 0, 32, 1};
    camera.valueRange = {0, 1, .3f, 1};
    camera.background = {.1f, .2f, .3f, 1};
    camera.light = {-12, -4, -4, 0};
    std::vector<g::Float4> transfer(16384);
    for (size_t i = 0; i < transfer.size(); ++i)
        transfer[i] = {float(i) / 16300, .4f, .2f, .5f};
    s::SectionOptions settings;
    settings.mode = s::SectionMode::Slice2D;
    settings.point = {-4, -4, -4, 0};
    settings.normal = {4, 0, 4, 0};
    auto image = s::renderOwnedSection(runtime, snapshot, 0, UINT32_MAX, camera, transfer, settings);
    std::vector<g::Float4> pixels(17 * 9);
    std::vector<float> depths(pixels.size());
    runtime.read(image.color, pixels.data(), pixels.size() * 16);
    runtime.read(image.depth, depths.data(), depths.size() * 4);
    for (unsigned y = 0; y < 9; ++y)
        for (unsigned x = 0; x < 17; ++x) {
            const float coordinate = float(x) * 8 / 17;
            const uint32_t tf = uint32_t(std::clamp(.1f * coordinate - .3f, 0.f, 1.f) * 16300);
            const auto actual = pixels[y * 17 + x];
            expectByte(actual.x, byte(transfer[tf].x * .5f),
                       "2D legacy integer pixel mapping and transfer threshold");
            expectByte(actual.y, byte(.2f), "2D alpha over black");
            require(actual.w == 1 && depths[y * 17 + x] == 0, "2D opaque alpha/depth");
        }
    // Missing leaves stay black, even with a nonblack scene background.
    settings.point.y = 2;
    image = s::renderOwnedSection(runtime, snapshot, 0, UINT32_MAX, camera, transfer, settings);
    runtime.read(image.color, pixels.data(), pixels.size() * 16);
    for (auto v : pixels)
        require(v.x == 0 && v.y == 0 && v.z == 0 && v.w == 1, "2D sparse miss black");
    transfer.assign(16384, {.8f, .4f, .2f, .5f});
    camera.image = {1, 1, 0, 256};
    settings.mode = s::SectionMode::Slice3D;
    settings.point = {-3, -4, -4, 0};
    settings.normal = {1, 0, 0, 0};
    auto render = [&](uint32_t color = UINT32_MAX) {
        auto result = s::renderOwnedSection(runtime, snapshot, 0, color, camera, transfer, settings);
        g::Float4 pixel;
        float depth;
        runtime.read(result.color, &pixel, 16);
        runtime.read(result.depth, &depth, 4);
        return std::pair{pixel, depth};
    };
    auto result = render();
    expectByte(result.first.x, byte(.9f), "3D unshaded interior section over white surface");
    expectByte(result.first.y, byte(.7f), "3D section blend");
    require(std::isfinite(result.second), "3D surface depth");
    result = render(1);
    expectByte(result.first.x, byte(.9f * 32 / 255), "3D color modulation");
    expectByte(result.first.y, byte(.7f * 64 / 255), "3D color modulation green");
    settings.point.x = -7;
    result = render();
    expectByte(result.first.x, byte(.8f), "3D diffuse surface behind section");
    expectByte(result.first.y, byte(.6f), "3D diffuse blend");
    camera.valueRange.z = 2;
    result = render();
    expectByte(result.first.x, byte(.45f), "3D section over background without surface");
    expectByte(result.first.y, byte(.3f), "3D background blend");
    camera.valueRange.z = .3f;
    settings.point.x = -12;
    result = render();
    expectByte(result.first.x, byte(.8f), "Behind-camera plane repairs legacy positive NOHIT sentinel");
    require(result.second < 32, "Behind-camera trace starts at original camera");
    settings.normal = {0, 1, 0, 0};
    settings.point = {-3, -3, -4, 0};
    result = render();
    expectByte(result.first.x, byte(.8f), "Parallel plane still traces surface");
    settings.normal = {1, 0, 0, 0};
    settings.point = {2, -4, -4, 0};
    result = render();
    expectByte(result.first.x, byte(.1f), "Plane past volume background");
    require(std::isinf(result.second), "Plane past volume miss depth");
    settings.point = {-7, -4, -4, 0};
    settings.maxLeafSteps = 1;
    result = render();
    require(result.second == -1, "Section exhaustion is explicit");
    auto reject = [&](s::SectionOptions bad, const std::vector<g::Float4>& tf) {
        bool failed = false;
        try {
            s::renderOwnedSection(runtime, snapshot, 0, UINT32_MAX, camera, tf, bad);
        } catch (const std::invalid_argument&) {
            failed = true;
        }
        require(failed, "Invalid section input rejected");
    };
    reject(settings, {});
    settings.maxLeafSteps = 0;
    reject(settings, transfer);
    settings.maxLeafSteps = 256;
    settings.normal.x = NAN;
    reject(settings, transfer);
    // Empty publications have no atlas resources but still obey opaque section output.
    runtime.flush();
    std::ifstream binary(remap, std::ios::binary);
    std::vector<char> code((std::istreambuf_iterator<char>(binary)), std::istreambuf_iterator<char>());
    p::VolumeCreateDesc create;
    create.device = runtime.device();
    create.configuration.levelCount = 1;
    create.configuration.logDimension = {3};
    create.remapArtifact = code.data();
    create.remapArtifactBytes = code.size();
    p::VolumeHandle emptyOwner;
    require(NVRHI_SUCCEEDED(p::createVolume(create, &emptyOwner)), "Empty section owner");
    require(NVRHI_SUCCEEDED(emptyOwner->finishTopology()) &&
                NVRHI_SUCCEEDED(emptyOwner->setChannel(0, {p::AtlasFormat::R32_FLOAT, {}})),
            "Empty section topology");
    p::GenerationTicketHandle ticket;
    require(NVRHI_SUCCEEDED(emptyOwner->prepareGeneration({}, &ticket)), "Empty section preparation");
    nvrhi::CommandListHandle commands;
    require(NVRHI_SUCCEEDED(runtime.device()->createCommandList({}, &commands)), "Empty section commands");
    commands->open();
    require(NVRHI_SUCCEEDED(emptyOwner->recordAndPublish({runtime.device(), commands}, ticket)),
            "Empty section publication");
    commands->close();
    runtime.device()->executeCommandList(commands);
    snapshot = nullptr;
    require(NVRHI_SUCCEEDED(emptyOwner->getSnapshot(&snapshot)), "Empty section snapshot");
    emptyOwner = nullptr;
    ticket = nullptr;
    settings = {};
    result = render();
    expectByte(result.first.x, byte(camera.background.x), "Empty 3D section background");
    require(result.first.w == 1 && std::isinf(result.second), "Empty 3D section alpha/depth");
    settings.mode = s::SectionMode::Slice2D;
    result = render();
    require(result.first.x == 0 && result.first.y == 0 && result.first.z == 0 && result.first.w == 1,
            "Empty 2D section black");
    std::puts("PASS section 2D/3D equations, transfer, color, sparse misses, behind/parallel planes, limits");
}
void compositing(es::ComputeRuntime& runtime, int argc, const char** argv) {
    // Every byte source/destination/alpha combination, distributed over RGB.
    const uint32_t count = 256 * 256 * 256;
    std::vector<uint32_t> src(count + 2), dst(count + 2), actual(count);
    src[0] = src.back() = 0x12345678;
    dst[0] = dst.back() = 0x87654321;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t s = i & 255, d = (i >> 8) & 255, a = i >> 16;
        src[i + 1] = s | ((255 - s) << 8) | (s << 16) | (a << 24);
        dst[i + 1] = d | (d << 8) | ((255 - d) << 16) | (d << 24);
    }
    auto source = runtime.buffer(src.size() * 4, 4, src.data()),
         destination = runtime.buffer(dst.size() * 4, 4, dst.data());
    auto output = s::compositeOverRGBA8(runtime, {source, 4, uint64_t(count) * 4},
                                        {destination, 4, uint64_t(count) * 4}, count);
    runtime.read(output, actual.data(), actual.size() * 4);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t expected = 0;
        for (unsigned c = 0; c < 4; ++c) {
            const unsigned s = (src[i + 1] >> (c * 8)) & 255, d = (dst[i + 1] >> (c * 8)) & 255;
            const double result = std::min(255., s + d * (255. - (src[i + 1] >> 24)) / 255.);
            // Integer numerator is the independent exact rational oracle at integer boundaries.
            const uint32_t exact = std::min(255u, (s * 255 + d * (255 - (src[i + 1] >> 24))) / 255);
            require(std::abs(double(exact) - result) < 1.0000001, "Composite rational bound");
            expected |= exact << (c * 8);
        }
        require(actual[i] == expected, "Composite exhaustive byte equation");
    }
    // CUDA-reference capture is optional; default correctness remains hardware-independent.
    for (int i = 1; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--dump-composite")) {
            std::ofstream out(argv[++i], std::ios::binary);
            out.write(reinterpret_cast<const char*>(actual.data()), std::streamsize(actual.size() * 4));
            require(bool(out), "write composite capture");
        }
    uint32_t guard[2];
    runtime.read(source, guard, 4);
    require(guard[0] == src.front(), "Composite source guard");
    runtime.read(destination, guard, 4);
    require(guard[0] == dst.front(), "Composite destination guard");
    auto aliased = s::compositeOverRGBA8(runtime, {source, 4, 12}, {source, 8, 12}, 3);
    runtime.read(aliased, guard, 4);
    require(guard[0] == 0x0001ff01, "Composite overlapping read-only input ranges");
    require(!s::compositeOverRGBA8(runtime, {}, {}, 0), "Composite empty no allocation");
    bool failed = false;
    try {
        s::compositeOverRGBA8(runtime, {source, 1, 4}, {destination, 0, 4}, 1);
    } catch (const std::invalid_argument&) {
        failed = true;
    }
    require(failed, "Composite unaligned ranges rejected");
    std::puts("PASS composite all 16,777,216 byte triples, offsets, alias inputs, guards, empty/rejection");
}
}  // namespace
int main(int argc, const char** argv) {
    try {
        es::ExampleDevice device(argc, argv);
        const auto path = donut::app::GetDirectoryWithExecutable() / "shaders/gvdb_owned" /
                          donut::app::GetShaderTypeName(device.get()->getGraphicsAPI());
        es::ComputeRuntime runtime(device.get(), path);
        sections(runtime, path / "GVDBAtlasRepack_relocateAtlas.bin");
        compositing(runtime, argc, argv);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
