// SPDX-License-Identifier: Apache-2.0
#include "OwnedVolumeLayers.h"
#include "ComputeRuntime.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace gvdb::sample {
LayeredVolumeOutput renderOwnedLayers(ethereal::slang::ComputeRuntime& runtime, uint32_t width, uint32_t height,
                                      slang::Float4 background, const std::vector<OwnedVolumeLayer>& layers) {
    const uint64_t pixels = uint64_t(width) * height;
    if (!width || !height || pixels > portable::c_AddressSpaceBytes / 16 || (uint64_t(width) + 7) / 8 > 65535 ||
        (uint64_t(height) + 7) / 8 > 65535 || layers.size() > 64)
        throw std::invalid_argument("Layered image extent or layer count (maximum 64)");
    if (!std::isfinite(background.x) || !std::isfinite(background.y) || !std::isfinite(background.z) ||
        !std::isfinite(background.w))
        throw std::invalid_argument("Layered background");
    for (const auto& layer : layers)
        if (uint32_t(layer.shading) > uint32_t(LayerShading::Deep) ||
            (layer.shading != LayerShading::Off &&
             (!layer.snapshot || layer.snapshot->getDevice() != runtime.device())))
            throw std::invalid_argument("Layer shading or snapshot device");
    const uint32_t count = uint32_t(pixels), zero = 0, groups = (count + 255) / 256;
    LayeredVolumeOutput result{{}, runtime.buffer(4, 4, &zero), width, height};
    struct Parameters {
        slang::UInt4 work;
        slang::Float4 background;
    };
    static_assert(sizeof(Parameters) == 32);
    using B = nvrhi::BindingSetItem;
    auto clear = [&](slang::Float4 value) {
        auto buffer = runtime.buffer(size_t(pixels) * 4, 4);
        Parameters p{
            {count, 0, 0, 0},
            value
        };
        runtime.dispatch("OwnedVolumeLayers", "clearLayers", &p, sizeof(p),
                         {B::StructuredBuffer_UAV(0, buffer)}, std::min(groups, 65535u),
                         (groups + 65534) / 65535);
        return buffer;
    };
    if (layers.empty())
        result.rgba8 = clear(background);
    for (size_t index = 0; index < layers.size(); ++index) {
        const auto& layer = layers[index];
        if (layer.shading == LayerShading::Off) {
            if (index == 0)
                result.rgba8 = clear({});
            continue;
        }
        auto camera = layer.camera;
        camera.image.x = width;
        camera.image.y = height;
        camera.background = index == 0 ? background : slang::Float4{};
        slang::RaycastOutput image;
        if (layer.shading == LayerShading::Section2D || layer.shading == LayerShading::Section3D) {
            auto options = layer.section;
            options.mode =
                layer.shading == LayerShading::Section2D ? SectionMode::Slice2D : SectionMode::Slice3D;
            image = renderOwnedSection(runtime, layer.snapshot, layer.scalarChannel, layer.colorChannel, camera,
                                       layer.transfer, options);
        } else if (layer.shading == LayerShading::Tricubic) {
            image = renderOwnedTricubic(runtime, layer.snapshot, layer.scalarChannel, layer.colorChannel,
                                        camera, layer.tricubic);
        } else {
            auto options = layer.modes;
            switch (layer.shading) {
                case LayerShading::Voxel:
                    options.mode = VolumeMode::Voxel;
                    break;
                case LayerShading::EmptySkip:
                    options.mode = VolumeMode::EmptySkip;
                    break;
                case LayerShading::Trilinear:
                    options.mode = VolumeMode::Trilinear;
                    break;
                case LayerShading::LevelSet:
                    options.mode = VolumeMode::LevelSet;
                    break;
                default:
                    options.mode = VolumeMode::Deep;
                    break;
            }
            image = renderOwnedMode(runtime, layer.snapshot, layer.scalarChannel, layer.colorChannel, camera,
                                    layer.transfer, options);
        }
        // Modes/sections already contain exact byte/255 values. Recover those
        // bytes by rounding; tricubic retains the original uchar truncation.
        Parameters p{
            {count, layer.shading != LayerShading::Tricubic, 0, 0},
            {}
        };
        auto packed = runtime.buffer(size_t(pixels) * 4, 4);
        runtime.dispatch("OwnedVolumeLayers", "packLayer", &p, sizeof(p),
                         {B::StructuredBuffer_SRV(0, image.color), B::StructuredBuffer_SRV(1, image.depth),
                          B::StructuredBuffer_UAV(0, packed), B::StructuredBuffer_UAV(1, result.diagnostics)},
                         std::min(groups, 65535u), (groups + 65534) / 65535);
        if (index == 0)
            result.rgba8 = packed;
        else
            result.rgba8 =
                compositeOverRGBA8(runtime, {packed, 0, pixels * 4}, {result.rgba8, 0, pixels * 4}, count);
    }
    return result;
}
}  // namespace gvdb::sample
