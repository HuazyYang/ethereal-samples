// Render a VBX scalar volume using the same offline Slang kernels on every API.
// SPDX-License-Identifier: Apache-2.0
#include "ExampleDevice.h"
#include "../sample-utils/OwnedVolumeRaycast.h"
#include "../sample-utils/OwnedVolumeIO.h"
#include "../sample-utils/OwnedVolumeTricubic.h"
#include "../sample-utils/OwnedVolumeModes.h"
#include "../sample-utils/OwnedVolumeSections.h"
#include "../sample-utils/OwnedVolumeLayers.h"
#include <gvdb/slang/GVDBPortableVbx.h>
#include <donut/core/math/math.h>
#include <stb_image_write.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
void check(nvrhi::FRESULT status, const char* operation) {
    if (NVRHI_FAILED(status))
        throw std::runtime_error(std::string(operation) + ": " + std::to_string(status));
}
gvdb::slang::Float4 vector4(dm::float3 v, float w = 0) { return {v.x, v.y, v.z, w}; }
std::vector<gvdb::slang::Float4> transferFunction(bool retained = false) {
    using F = gvdb::slang::Float4;
    const F starts[4] = {
        {0,    0,    0,    0   },
        {1,    1,    0,    0.4f},
        {1,    0,    0,    0.3f},
        {0.2f, 0.2f, 0.2f, 0.1f}
    };
    const F ends[4] = {
        {1,    1,    0,    0.1f},
        {1,    0,    0,    0.3f},
        {0.2f, 0.2f, 0.2f, 0.1f},
        {0,    0,    0,    0   }
    };
    const unsigned count = retained ? 16384 : 256;
    std::vector<F> result(count);
    for (unsigned i = 0; i < count; ++i) {
        const float coordinate = float(i) * 4 / (retained ? count : count - 1);
        const unsigned segment = std::min(3u, unsigned(coordinate));
        const float t = coordinate - segment;
        const F a = starts[segment], b = ends[segment];
        result[i] = {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
                     a.w + (b.w - a.w) * t};
    }
    return result;
}
}  // namespace
int main(int argc, const char** argv) {
    try {
        int width = 1024, height = 768, shade = -1;
        float step = 0.25f;
        bool surface = false, tricubic = false, referenceScene = false;
        std::filesystem::path input = std::filesystem::path(GVDB_SAMPLES_ASSETS_DIR) / "explosion.vbx",
                              output = "out_rendtofile.png";
        struct LayerInput {
            std::filesystem::path path;
            dm::float3 offset{};
        };
        std::vector<LayerInput> additional;
        for (int i = 1; i < argc; ++i) {
            const auto argument = [&]() -> const char* {
                if (++i >= argc)
                    throw std::invalid_argument("Missing option value");
                return argv[i];
            };
            if (!std::strcmp(argv[i], "--help")) {
                std::puts(
                    "render-to-file [-dx11|-dx12|-vk|-cuda] [--adapter N] [--validation]\n"
                    "  [--input volume.vbx] [--output image.png] [--width N] [--height N] [--step N] "
                    "[--surface|--tricubic]\n"
                    "  [--shade 0..7] [--reference-scene] [--layer volume.vbx [--layer-offset X Y Z]]...\n"
                    "  Layers require --shade, share index coordinates, and compose in argument order.\n"
                    "  Shade: voxel, section2D, section3D, empty-skip, trilinear, tricubic, level-set, "
                    "volume.");
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
            else if (!std::strcmp(argv[i], "--surface"))
                surface = true;
            else if (!std::strcmp(argv[i], "--tricubic"))
                tricubic = true;
            else if (!std::strcmp(argv[i], "--shade"))
                shade = std::stoi(argument());
            else if (!std::strcmp(argv[i], "--reference-scene"))
                referenceScene = true;
            else if (!std::strcmp(argv[i], "--layer"))
                additional.push_back({argument(), {}});
            else if (!std::strcmp(argv[i], "--layer-offset")) {
                if (additional.empty())
                    throw std::invalid_argument("--layer-offset requires a preceding --layer");
                auto& offset = additional.back().offset;
                offset.x = std::stof(argument());
                offset.y = std::stof(argument());
                offset.z = std::stof(argument());
                if (!std::isfinite(offset.x) || !std::isfinite(offset.y) || !std::isfinite(offset.z))
                    throw std::invalid_argument("Nonfinite layer offset");
            }
        }
        if (shade < -1 || shade > 7 || ((shade >= 0 || referenceScene) && (surface || tricubic)))
            throw std::invalid_argument("Choose one shading option; --shade is 0..7");
        if (referenceScene && shade < 0)
            shade = 7;
        if (additional.size() > 63 || (!additional.empty() && shade < 0))
            throw std::invalid_argument("Layers require --shade; maximum 64 total layers");
        if (surface && tricubic)
            throw std::invalid_argument("Choose either --surface or --tricubic");
        if (width < 1 || height < 1 || uint64_t(width) * height > 16 * 1024 * 1024 || !std::isfinite(step) ||
            step <= 0)
            throw std::invalid_argument("Invalid image dimensions or ray step");
        ethereal::slang::ExampleDevice device(argc, argv);
        const auto shaderPath = donut::app::GetDirectoryWithExecutable() / "shaders/gvdb_owned" /
                                donut::app::GetShaderTypeName(device.get()->getGraphicsAPI());
        auto volume =
            gvdb::sample::loadOwnedVBX(device.get(), shaderPath / "GVDBAtlasRepack_relocateAtlas.bin", input);
        const auto snapshot = volume->getDesc();
        gvdb::portable::ChannelSnapshotDesc scalar;
        check(volume->getChannel(0, scalar), "get scalar channel");
        if (snapshot.channelMask != 1 || scalar.desc.format != gvdb::portable::AtlasFormat::R32_FLOAT)
            throw std::runtime_error("render-to-file requires exactly one R32_FLOAT channel");
        gvdb::portable::LevelInfo leafLevel;
        check(volume->getLevel(0, leafLevel), "get leaf level");
        const auto& info = snapshot.volume;
        // Rendering requires only the retained snapshot; no duplicate atlas/topology owner.
        ethereal::slang::ComputeRuntime runtime(device.get(), shaderPath);
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
        p.image = {uint32_t(width), uint32_t(height), 256, uint32_t(maximumSteps)};
        p.rayOrigin = vector4(center + backwards * radius * 2.8f);
        p.direction00 = vector4(forward - right * horizontal + up * vertical);
        p.directionDx = vector4(right * (2 * horizontal / width));
        p.directionDy = vector4(up * (-2 * vertical / height));
        p.march = {step, 0, radius * 6, 1.5f};
        p.valueRange = {0, 1, 0.1f, 0.995f};
        p.background = {0.1f, 0.2f, 0.4f, 1};
        p.light = vector4(center + dm::float3(-radius, radius * 2, -radius), 0.1f);
        if (referenceScene) {
            gvdb::slang::VbxRawVolume raw;
            std::string error;
            if (!gvdb::slang::loadRawVBXFile(input, raw, error))
                throw std::runtime_error(error);
            const auto v = [](gvdb::slang::VbxFloat3 a) { return dm::float3(a.x, a.y, a.z); };
            const auto& t = raw.transform;
            if (t.scale.x == 0 || t.scale.y == 0 || t.scale.z == 0)
                throw std::invalid_argument("Singular VBX transform");
            const auto inverse = dm::inverse(dm::translation(v(t.preTranslation)) * dm::scaling(v(t.scale)) *
                                             dm::rotationQuat(dm::radians(v(t.angles))).toAffine() *
                                             dm::translation(v(t.translation)));
            const auto orbit = [](float x, float y) {
                x = dm::radians(x);
                y = dm::radians(y);
                return dm::float3(std::cos(y) * std::sin(x), std::sin(y), std::cos(y) * std::cos(x));
            };
            const auto back = orbit(20, 30), right = dm::normalize(dm::cross(dm::float3(0, 1, 0), back)),
                       up = dm::cross(back, right);
            p.rayOrigin = vector4(inverse.transformPoint(dm::float3(125, 160, 125) + back * 500.f));
            p.direction00 = vector4(inverse.transformVector(-back - right * horizontal + up * vertical));
            p.directionDx = vector4(inverse.transformVector(right * (2 * horizontal / width)));
            p.directionDy = vector4(inverse.transformVector(up * (-2 * vertical / height)));
            p.light = vector4(inverse.transformPoint(dm::float3(132, -20, 50) + orbit(299, 57.3f) * 200.f));
            p.valueRange = {0, .1f, .1f, .995f};
        }
        uint64_t topologyBytes = 0;
        for (auto kind : {gvdb::portable::TopologyBuffer::Nodes, gvdb::portable::TopologyBuffer::Children,
                          gvdb::portable::TopologyBuffer::Levels, gvdb::portable::TopologyBuffer::AtlasMap}) {
            gvdb::portable::BorrowedBufferBinding binding;
            check(volume->getTopologyBuffer(kind, binding), "get topology memory");
            topologyBytes += binding.byteSize;
        }
        std::printf("Logical retained storage: topology %llu bytes, atlas %llu bytes\n",
                    static_cast<unsigned long long>(topologyBytes),
                    static_cast<unsigned long long>(scalar.raw.byteSize));
        std::vector<gvdb::sample::OwnedVolumeLayer> layers;
        if (shade >= 0) {
            const gvdb::sample::LayerShading mapping[] = {
                gvdb::sample::LayerShading::Voxel,     gvdb::sample::LayerShading::Section2D,
                gvdb::sample::LayerShading::Section3D, gvdb::sample::LayerShading::EmptySkip,
                gvdb::sample::LayerShading::Trilinear, gvdb::sample::LayerShading::Tricubic,
                gvdb::sample::LayerShading::LevelSet,  gvdb::sample::LayerShading::Deep};
            gvdb::sample::OwnedVolumeLayer layer;
            layer.snapshot = volume;
            layer.shading = mapping[shade];
            layer.camera = p;
            layer.transfer = transferFunction(true);
            layer.section.normal = {0, 1, 0, 0};
            layer.section.epsilon = .001f;
            layer.tricubic.epsilon = .001f;
            layer.tricubic.fineStep = .25f;
            layer.modes.epsilon = .001f;
            layer.modes.albedo = 1.5f;
            layer.modes.maxLeafSteps = 4096;
            layers.push_back(layer);
            for (const auto& extra : additional) {
                layer.snapshot =
                    std::filesystem::equivalent(extra.path, input)
                        ? volume
                        : gvdb::sample::loadOwnedVBX(
                              device.get(), shaderPath / "GVDBAtlasRepack_relocateAtlas.bin", extra.path);
                layer.camera = p;
                layer.camera.rayOrigin.x -= extra.offset.x;
                layer.camera.rayOrigin.y -= extra.offset.y;
                layer.camera.rayOrigin.z -= extra.offset.z;
                layer.camera.light.x -= extra.offset.x;
                layer.camera.light.y -= extra.offset.y;
                layer.camera.light.z -= extra.offset.z;
                layers.push_back(layer);
            }
        }
        runtime.flush();  // Exclude asset upload from GPU render timing.
        const auto start = std::chrono::steady_clock::now();
        gvdb::slang::RaycastOutput rendered;
        std::vector<uint32_t> packed;
        if (shade >= 0) {
            auto composed = gvdb::sample::renderOwnedLayers(runtime, uint32_t(width), uint32_t(height),
                                                            p.background, layers);
            packed.resize(size_t(width) * height);
            runtime.read(composed.rgba8, packed.data(), packed.size() * 4);
            uint32_t diagnostics = 0;
            runtime.read(composed.diagnostics, &diagnostics, 4);
            if (diagnostics)
                throw std::runtime_error("Layer traversal exhausted or invalid: " +
                                         std::to_string(diagnostics));
            std::printf("Composited %zu retained volume layers in scene order\n", layers.size());
        } else if (tricubic) {
            gvdb::sample::TricubicOptions options;
            rendered = gvdb::sample::renderOwnedTricubic(runtime, volume, 0, UINT32_MAX, p, options);
        } else
            rendered = gvdb::sample::raycastOwnedVolume(runtime, volume, 0, p, transferFunction(), surface);
        if (tricubic) {
            std::vector<float> depths(size_t(width) * height);
            runtime.read(rendered.depth, depths.data(), depths.size() * 4);
            for (float depth : depths)
                if (std::isnan(depth) || depth < 0)
                    throw std::runtime_error("Traversal exhausted or invalid");
        }
        std::vector<gvdb::slang::Float4> color(size_t(width) * height);
        if (packed.empty())
            runtime.read(rendered.color, color.data(), color.size() * sizeof(color[0]));
        else
            for (size_t i = 0; i < packed.size(); ++i)
                color[i] = {float(packed[i] & 255) / 255, float((packed[i] >> 8) & 255) / 255,
                            float((packed[i] >> 16) & 255) / 255, float(packed[i] >> 24) / 255};
        const double milliseconds =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::vector<uint8_t> rgba(color.size() * 4);
        size_t changed = 0;
        for (size_t i = 0; i < color.size(); ++i) {
            const auto& c = color[i];
            if (!std::isfinite(c.x) || !std::isfinite(c.y) || !std::isfinite(c.z) || !std::isfinite(c.w))
                throw std::runtime_error("Renderer produced nonfinite pixels");
            if (std::abs(c.x - p.background.x) + std::abs(c.y - p.background.y) +
                    std::abs(c.z - p.background.z) >
                0.03f)
                ++changed;
            const float channels[4] = {c.x, c.y, c.z, c.w};
            for (unsigned j = 0; j < 4; ++j)
                rgba[i * 4 + j] = packed.empty() ? uint8_t(std::clamp(channels[j], 0.f, 1.f) * 255 + .5f)
                                                 : uint8_t(packed[i] >> (j * 8));
        }
        if (!changed && shade < 0)
            throw std::runtime_error("No volume samples contributed to the image");
        if (!stbi_write_png(output.string().c_str(), width, height, 4, rgba.data(), width * 4))
            throw std::runtime_error("Cannot write output image");
        std::printf("%u leaves, %zu voxels, %zu non-background pixels; render/readback %.3f ms\n",
                    leafLevel.nodes.z, size_t(scalar.storage.voxelCount), changed, milliseconds);
        std::printf("Wrote %s\n", output.string().c_str());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "render-to-file: %s\n", error.what());
        return 1;
    }
}
