// Render a VBX with the original custom diffuse/reflected-sky surface look on every API.
// SPDX-License-Identifier: Apache-2.0
#include "../../tests/PortableFixtures.h"
#include "ExampleDevice.h"
#include "RenderCustom.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <donut/core/math/math.h>
#include "../sample-utils/OwnedVolumeIO.h"
#include <gvdb/slang/GVDBPortableVbx.h>
#include <stb_image_write.h>
#include <vector>

namespace
{
gvdb::slang::Float4 vector4(dm::float3 v, float w = 0)
{
    return {v.x, v.y, v.z, w};
}
void analyticTest(ethereal::slang::ComputeRuntime &runtime,const std::filesystem::path& remap)
{
    namespace g = gvdb::slang;
    namespace f = g::fixtures;
    auto fixture = f::twoBrickVolume();
    for (uint32_t z = 0; z < 4; ++z)
        for (uint32_t y = 0; y < 4; ++y)
            for (uint32_t x = 0; x < 8; ++x)
                fixture.atlas[(z * 4 + y) * 8 + x] = (float(x % 4) - .5f + float(x / 4) * 2.f) * .25f;
    g::PackedVolume packed;
    std::string error;
    if (!g::packVolume(f::volumeSource(fixture), packed, error))
        throw std::runtime_error(error);
    g::VbxRawVolume file;file.packed=std::move(packed);
    g::VbxChannel scalar;scalar.format=g::AtlasFormat::R32_FLOAT;scalar.bytes.resize(fixture.atlas.size()*sizeof(float));
    std::memcpy(scalar.bytes.data(),fixture.atlas.data(),scalar.bytes.size());file.channels.push_back(std::move(scalar));
    std::vector<uint8_t> encoded;if(!g::saveRawVBX(file,encoded,error))throw std::runtime_error(error);
    auto volume=gvdb::sample::loadOwnedVBX(runtime.device(),remap,encoded.data(),encoded.size());
    auto p = f::raycastParameters(fixture);
    const auto test = [&](g::RaycastParameters settings, g::Float4 expected, float expectedDepth) {
        auto output = renderCustom(runtime, volume, settings);
        g::Float4 result{};
        float depth = 0;
        runtime.read(output.color, &result, sizeof(result));
        runtime.read(output.depth, &depth, sizeof(depth));
        const float actual[] = {result.x, result.y, result.z, result.w},
                    reference[] = {expected.x, expected.y, expected.z, expected.w};
        for (unsigned i = 0; i < 4; ++i)
            if (!std::isfinite(actual[i]) || std::abs(actual[i] - reference[i]) > .002f)
            {
                std::fprintf(stderr, "channel %u: expected %f actual %f\n", i, reference[i], actual[i]);
                throw std::runtime_error("Custom diffuse/reflection color differs from analytic reference");
            }
        if (std::isinf(expectedDepth) ? !(std::isinf(depth) && depth > 0) :
            (!std::isfinite(depth) || std::abs(depth - expectedDepth) > .002f))
            throw std::runtime_error("Custom surface depth differs from analytic reference");
    };
    test(p, {.6f, .6f, .6f, 1}, 3);
    p.rayOrigin = {-1, 4, 1, 0};
    p.direction00 = {1, -1, 0, 0};
    p.march.x = std::sqrt(2.f) * .25f;
    p.valueRange.z = .49f; // Keep the analytic sample away from floating-point threshold ties.
    const float sky = 1 / std::sqrt(2.f);
    test(p, {.6f, .6f + .3f * sky, .6f + .7f * sky, 1}, 3 * std::sqrt(2.f));
    p = f::raycastParameters(fixture);
    p.rayOrigin.y = 3;
    test(p, {0, 0, .1f, 1}, INFINITY);
    std::puts("PASS custom threshold surface, diffuse/reflected sky and miss color analytic references");
}
} // namespace
int main(int argc, const char **argv)
{
    try
    {
        int width = 1024, height = 768;
        float step = 0.25f, threshold = 0.1f;
        bool selfTest = false;
        std::filesystem::path input = std::filesystem::path(GVDB_SAMPLES_ASSETS_DIR) / "explosion.vbx",
                              output = "out_rendkernel.png";
        for (int i = 1; i < argc; ++i)
        {
            const auto argument = [&]() -> const char * {
                if (++i >= argc)
                    throw std::invalid_argument("Missing option value");
                return argv[i];
            };
            if (!std::strcmp(argv[i], "--help"))
            {
                std::puts("render-kernel [-dx11|-dx12|-vk|-cuda] [--adapter N] [--validation]\n"
                          "  [--input volume.vbx] [--output image.png] [--width N] [--height N] [--step N] "
                          "[--threshold N] [--self-test]");
                return 0;
            }
            if (!std::strcmp(argv[i], "--input"))
                input = argument();
            else if (!std::strcmp(argv[i], "--output"))
                output = argument();
            else if (!std::strcmp(argv[i], "--width"))
                width = std::stoi(argument());
            else if (!std::strcmp(argv[i], "--height"))
                height = std::stoi(argument());
            else if (!std::strcmp(argv[i], "--step"))
                step = std::stof(argument());
            else if (!std::strcmp(argv[i], "--threshold"))
                threshold = std::stof(argument());
            else if (!std::strcmp(argv[i], "--self-test"))
                selfTest = true;
        }
        if (width < 1 || height < 1 || uint64_t(width) * height > 16 * 1024 * 1024 || !std::isfinite(step) ||
            step <= 0 || !std::isfinite(threshold))
            throw std::invalid_argument("Invalid image dimensions or ray step");
        ethereal::slang::ExampleDevice device(argc, argv);
        const auto shaderPath =
            device.shaderPath().parent_path().parent_path() / "render_kernel" / device.shaderPath().filename();
        const auto remap=device.shaderPath().parent_path().parent_path()/"gvdb_owned"/
            device.shaderPath().filename()/"GVDBAtlasRepack_relocateAtlas.bin";
        ethereal::slang::ComputeRuntime runtime(device.get(), shaderPath);
        if (selfTest)
        {
            analyticTest(runtime,remap);
            return 0;
        }
        auto volume=gvdb::sample::loadOwnedVBX(device.get(),remap,input);
        const auto snapshot=volume->getDesc();gvdb::portable::ChannelSnapshotDesc scalar;gvdb::portable::LevelInfo leafLevel;
        if(NVRHI_FAILED(volume->getChannel(0,scalar))||snapshot.channelMask!=1||
            scalar.desc.format!=gvdb::portable::AtlasFormat::R32_FLOAT||NVRHI_FAILED(volume->getLevel(0,leafLevel)))
            throw std::runtime_error("render-kernel requires exactly one R32_FLOAT channel");
        const auto& info=snapshot.volume;
        const dm::float3 lower(info.boundsMin.x, info.boundsMin.y, info.boundsMin.z);
        const dm::float3 upper(info.boundsMax.x, info.boundsMax.y, info.boundsMax.z);
        const dm::float3 center = (lower + upper) * 0.5f;
        // Frame index-space bounds, so differently transformed VBX assets remain visible.
        const float radius = std::max(1.f, dm::length(upper - lower) * 0.5f);
        const dm::float3 backwards = dm::normalize(dm::float3(0.296198f, 0.5f, 0.813798f));
        const dm::float3 forward = -backwards, right = dm::normalize(dm::cross(forward, dm::float3(0, 1, 0)));
        const dm::float3 up = dm::cross(right, forward);
        const float horizontal = std::tan(dm::radians(25.f)), vertical = horizontal * float(height) / width;
        gvdb::slang::RaycastParameters p{};
        const double maximumSteps = std::ceil(double(radius) * 2 / step) + 8;
        if (maximumSteps > UINT32_MAX)
            throw std::invalid_argument("Ray step requires too many samples");
        p.image = {uint32_t(width), uint32_t(height), 1, uint32_t(maximumSteps)};
        p.rayOrigin = vector4(center + backwards * radius * 2.8f);
        p.direction00 = vector4(forward - right * horizontal + up * vertical);
        p.directionDx = vector4(right * (2 * horizontal / width));
        p.directionDy = vector4(up * (-2 * vertical / height));
        p.march = {step, 0, radius * 6, 1.5f};
        p.valueRange = {0, 1, threshold, 0.995f};
        p.background = {0, 0, 0.1f, 1};
        p.light = vector4(center + dm::float3(-radius, radius * 2, -radius), 0.1f);
        runtime.flush(); // Exclude asset upload from GPU render timing.
        const auto start = std::chrono::steady_clock::now();
        auto rendered = renderCustom(runtime, volume, p);
        std::vector<gvdb::slang::Float4> color(size_t(width) * height);
        runtime.read(rendered.color, color.data(), color.size() * sizeof(color[0]));
        const double milliseconds =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::vector<uint8_t> rgba(color.size() * 4);
        size_t changed = 0;
        for (size_t i = 0; i < color.size(); ++i)
        {
            const auto &c = color[i];
            if (!std::isfinite(c.x) || !std::isfinite(c.y) || !std::isfinite(c.z) || !std::isfinite(c.w))
                throw std::runtime_error("Renderer produced nonfinite pixels");
            if (std::abs(c.x - p.background.x) + std::abs(c.y - p.background.y) + std::abs(c.z - p.background.z) >
                0.03f)
                ++changed;
            const float channels[4] = {c.x, c.y, c.z, c.w};
            for (unsigned j = 0; j < 4; ++j)
                rgba[i * 4 + j] = uint8_t(std::clamp(channels[j], 0.f, 1.f) * 255 + 0.5f);
        }
        if (!changed)
            throw std::runtime_error("No volume samples contributed to the image");
        if (!stbi_write_png(output.string().c_str(), width, height, 4, rgba.data(), width * 4))
            throw std::runtime_error("Cannot write output image");
        std::printf("%u leaves, %zu voxels, %zu non-background pixels; render/readback %.3f ms\n",
                    leafLevel.nodes.z, size_t(scalar.storage.voxelCount), changed, milliseconds);
        std::printf("Wrote %s\n", output.string().c_str());
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "render-kernel: %s\n", error.what());
        return 1;
    }
}
