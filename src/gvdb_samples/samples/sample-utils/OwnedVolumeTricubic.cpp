// SPDX-License-Identifier: Apache-2.0
#include "OwnedVolumeTricubic.h"
#include "OwnedVolumeRawViews.h"
#include "ComputeRuntime.h"
#include <cmath>
#include <stdexcept>
namespace gvdb::sample {
slang::RaycastOutput renderOwnedTricubic(ethereal::slang::ComputeRuntime& runtime,
                                         portable::SnapshotHandle snapshot, uint32_t scalarChannel,
                                         uint32_t colorChannel, slang::RaycastParameters camera,
                                         TricubicOptions options) {
    auto require = [](bool value, const char* text) {
        if (!value)
            throw std::invalid_argument(text);
    };
    require(snapshot && snapshot->getDevice() == runtime.device(), "Tricubic snapshot device");
    require(std::isfinite(options.fineStep) && options.fineStep > 0 && options.fineStep <= 1e6f &&
                std::isfinite(options.epsilon) && options.epsilon >= 0 && std::isfinite(options.shadowAmount) &&
                options.shadowAmount >= 0 && options.shadowAmount <= 1 && std::isfinite(options.shadowBias) &&
                options.shadowBias >= 0 && options.maxLeafSteps && options.maxLeafVisits,
            "Tricubic options");
    require(camera.direction00.w == 0, "Tricubic requires ray-distance clipping");
    camera.volume = snapshot->getDesc().volume;
    slang::validateRaycastParameters(camera, {
                                                 {1, 1, 1, 1},
                                                 {1, 1, 1, 1}
    });
    const uint64_t pixels = uint64_t(camera.image.x) * camera.image.y;
    require(pixels <= portable::c_AddressSpaceBytes / 16 &&
                camera.image.x / 8 + uint32_t(camera.image.x % 8 != 0) <= 65535 &&
                camera.image.y / 8 + uint32_t(camera.image.y % 8 != 0) <= 65535,
            "Tricubic image address/dispatch range");
    portable::ChannelSnapshotDesc scalar;
    require(NVRHI_SUCCEEDED(snapshot->getChannel(scalarChannel, scalar)) &&
                scalar.desc.format == portable::AtlasFormat::R32_FLOAT,
            "Tricubic scalar channel");
    if (colorChannel != UINT32_MAX) {
        portable::ChannelSnapshotDesc color;
        require(colorChannel != scalarChannel && NVRHI_SUCCEEDED(snapshot->getChannel(colorChannel, color)) &&
                    color.desc.format == portable::AtlasFormat::RGBA8_UINT,
                "Tricubic color channel");
    }
    if (!snapshot->getDesc().activeLeafCount) {
        std::vector<slang::Float4> colors(size_t(pixels), camera.background);
        std::vector<float> depths(size_t(pixels), INFINITY);
        return {runtime.buffer(colors.size() * 16, 16, colors.data()),
                runtime.buffer(depths.size() * 4, 4, depths.data()), camera.image.x, camera.image.y};
    }
    OwnedVolumeRawViews views(runtime.device(), std::move(snapshot), scalarChannel, colorChannel);
    nvrhi::BufferHandle emptyChildren, emptyColor;
    const uint32_t missing[2] = {UINT32_MAX, UINT32_MAX};
    if (!views.children.buffer) {
        emptyChildren = runtime.buffer(8, 0, missing, true);
        views.children = {emptyChildren, 0, 8};
    }
    if (colorChannel == UINT32_MAX) {
        emptyColor = runtime.buffer(4, 0, missing, true);
        views.color.raw = {emptyColor, 0, 4};
    }
    struct Parameters {
        slang::RaycastParameters camera;
        slang::UInt4 counts;
        slang::Float4 sampling, options;
    };
    static_assert(sizeof(Parameters) == 272);
    Parameters p{
        camera,
        {0,                    options.maxLeafSteps, options.maxLeafVisits,                  0               },
        {camera.march.x,       camera.valueRange.z,  options.epsilon,                        options.fineStep},
        {options.shadowAmount, options.shadowBias,   colorChannel == UINT32_MAX ? 0.f : 1.f,
          options.borderZero ? 1.f : 0.f                                                                      }
    };
    slang::RaycastOutput result{runtime.buffer(size_t(pixels) * 16, 16), runtime.buffer(size_t(pixels) * 4, 4),
                                camera.image.x, camera.image.y};
    using B = nvrhi::BindingSetItem;
    runtime.dispatch(
        "OwnedVolumeTricubic", "renderTricubic", &p, sizeof(p),
        {B::RawBuffer_SRV(0, views.scalar.raw.buffer, OwnedVolumeRawViews::range(views.scalar.raw)),
         B::RawBuffer_SRV(1, views.nodes.buffer, OwnedVolumeRawViews::range(views.nodes)),
         B::RawBuffer_SRV(2, views.children.buffer, OwnedVolumeRawViews::range(views.children)),
         B::StructuredBuffer_SRV(3, views.levels.buffer, nvrhi::Format::UNKNOWN,
                                 OwnedVolumeRawViews::range(views.levels)),
         B::RawBuffer_SRV(5, views.color.raw.buffer, OwnedVolumeRawViews::range(views.color.raw)),
         B::StructuredBuffer_UAV(0, result.color), B::StructuredBuffer_UAV(1, result.depth)},
        (camera.image.x + 7) / 8, (camera.image.y + 7) / 8);
    return result;
}
}  // namespace gvdb::sample
