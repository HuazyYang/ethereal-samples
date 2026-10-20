// SPDX-License-Identifier: Apache-2.0
#include "OwnedVolumeModes.h"
#include "OwnedVolumeRawViews.h"
#include "ComputeRuntime.h"
#include <cmath>
#include <stdexcept>
namespace gvdb::sample {
namespace {
void require(bool ok, const char* text) {
    if (!ok)
        throw std::invalid_argument(text);
}
}  // namespace
slang::RaycastOutput renderOwnedMode(ethereal::slang::ComputeRuntime& runtime,
                                     portable::SnapshotHandle snapshot, uint32_t scalarChannel,
                                     uint32_t colorChannel, slang::RaycastParameters camera,
                                     const std::vector<slang::Float4>& transfer, VolumeModeOptions options) {
    require(snapshot && snapshot->getDevice() == runtime.device(), "Volume mode snapshot device");
    require(uint32_t(options.mode) <= 6 && std::isfinite(options.epsilon) && options.epsilon >= 0 &&
                options.maxLeafSteps && options.maxLeafVisits && std::isfinite(options.shadowAmount) &&
                options.shadowAmount >= 0 && options.shadowAmount <= 1 && std::isfinite(options.shadowBias) &&
                options.shadowBias >= 0 && std::isfinite(options.extinction) && options.extinction >= 0 &&
                std::isfinite(options.albedo) && options.albedo >= 0 && std::isfinite(options.minimumValue) &&
                std::isfinite(options.alphaCutoff) && options.alphaCutoff >= 0 && options.alphaCutoff <= 1,
            "Volume mode options");
    require(transfer.size() == 16384, "Volume mode needs 16384 transfer entries");
    require(camera.direction00.w == 0, "Volume mode requires index-space ray directions");
    camera.volume = snapshot->getDesc().volume;
    slang::validateRaycastParameters(camera, transfer);
    const uint64_t pixels = uint64_t(camera.image.x) * camera.image.y;
    require(pixels <= portable::c_AddressSpaceBytes / 16 && (camera.image.x + 7) / 8 <= 65535 &&
                (camera.image.y + 7) / 8 <= 65535,
            "Volume mode image extent");
    portable::ChannelSnapshotDesc scalar, color;
    require(NVRHI_SUCCEEDED(snapshot->getChannel(scalarChannel, scalar)) &&
                scalar.desc.format == portable::AtlasFormat::R32_FLOAT,
            "Volume mode scalar channel");
    if (colorChannel != UINT32_MAX)
        require(colorChannel != scalarChannel && NVRHI_SUCCEEDED(snapshot->getChannel(colorChannel, color)) &&
                    color.desc.format == portable::AtlasFormat::RGBA8_UINT,
                "Volume mode color channel");
    const uint32_t missing[16] = {};
    auto dummyRaw = runtime.buffer(64, 0, missing, true), dummyLevels = runtime.buffer(64, 64, missing);
    portable::BorrowedBufferBinding nodes{dummyRaw, 0, 64}, children = nodes, levels{dummyLevels, 0, 64},
                                                            scalarView = nodes, colorView = nodes;
    const auto info = snapshot->getDesc();
    if (info.activeLeafCount) {
        OwnedVolumeRawViews views(runtime.device(), snapshot, scalarChannel, colorChannel);
        nodes = views.nodes;
        levels = views.levels;
        scalarView = views.scalar.raw;
        if (views.children.buffer)
            children = views.children;
        if (colorChannel != UINT32_MAX)
            colorView = views.color.raw;
    }
    struct Parameters {
        slang::RaycastParameters camera;
        slang::UInt4 counts;
        slang::Float4 sampling;
        slang::UInt4 mode;
        slang::Float4 lighting, integration;
    };
    static_assert(sizeof(Parameters) == 304);
    Parameters p{
        camera,
        {0,                      options.maxLeafSteps,       options.maxLeafVisits, 0             },
        {camera.march.x,         camera.valueRange.z,        options.epsilon,       0             },
        {uint32_t(options.mode), colorChannel != UINT32_MAX, info.activeLeafCount,  0             },
        {options.shadowAmount,   options.shadowBias,         options.extinction,    options.albedo},
        {options.minimumValue,   options.alphaCutoff,        0,                     0             }
    };
    auto table = runtime.buffer(transfer.size() * 16, 16, transfer.data());
    slang::RaycastOutput result{runtime.buffer(size_t(pixels) * 16, 16), runtime.buffer(size_t(pixels) * 4, 4),
                                camera.image.x, camera.image.y};
    using B = nvrhi::BindingSetItem;
    runtime.dispatch(
        "OwnedVolumeModes", "renderMode", &p, sizeof(p),
        {B::RawBuffer_SRV(0, scalarView.buffer, OwnedVolumeRawViews::range(scalarView)),
         B::RawBuffer_SRV(1, nodes.buffer, OwnedVolumeRawViews::range(nodes)),
         B::RawBuffer_SRV(2, children.buffer, OwnedVolumeRawViews::range(children)),
         B::StructuredBuffer_SRV(3, levels.buffer, nvrhi::Format::UNKNOWN, OwnedVolumeRawViews::range(levels)),
         B::StructuredBuffer_SRV(4, table),
         B::RawBuffer_SRV(5, colorView.buffer, OwnedVolumeRawViews::range(colorView)),
         B::StructuredBuffer_UAV(0, result.color), B::StructuredBuffer_UAV(1, result.depth)},
        (camera.image.x + 7) / 8, (camera.image.y + 7) / 8);
    return result;
}
}  // namespace gvdb::sample
