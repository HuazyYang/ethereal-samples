// SPDX-License-Identifier: Apache-2.0
#include "OwnedVolumeSections.h"
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
bool finite(slang::Float4 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w);
}
}  // namespace
slang::RaycastOutput renderOwnedSection(ethereal::slang::ComputeRuntime& runtime,
                                        portable::SnapshotHandle snapshot, uint32_t scalarChannel,
                                        uint32_t colorChannel, slang::RaycastParameters camera,
                                        const std::vector<slang::Float4>& transfer, SectionOptions options) {
    require(snapshot && snapshot->getDevice() == runtime.device(), "Section snapshot device");
    require(uint32_t(options.mode) <= 1 && finite(options.point) && finite(options.normal) &&
                std::isfinite(options.epsilon) && options.epsilon >= 0 && options.maxLeafSteps &&
                options.maxLeafVisits,
            "Section options");
    require(transfer.size() == 16384, "Section needs 16384 transfer entries");
    require(camera.direction00.w == 0, "Section requires index-space ray directions");
    camera.volume = snapshot->getDesc().volume;
    slang::validateRaycastParameters(camera, transfer);
    const uint64_t pixels = uint64_t(camera.image.x) * camera.image.y;
    require(pixels <= portable::c_AddressSpaceBytes / 16 && (camera.image.x + 7) / 8 <= 65535 &&
                (camera.image.y + 7) / 8 <= 65535,
            "Section image extent");
    portable::ChannelSnapshotDesc scalar, color;
    require(NVRHI_SUCCEEDED(snapshot->getChannel(scalarChannel, scalar)) &&
                scalar.desc.format == portable::AtlasFormat::R32_FLOAT,
            "Section scalar channel");
    if (colorChannel != UINT32_MAX)
        require(colorChannel != scalarChannel && NVRHI_SUCCEEDED(snapshot->getChannel(colorChannel, color)) &&
                    color.desc.format == portable::AtlasFormat::RGBA8_UINT,
                "Section color channel");
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
        slang::Float4 sampling, point, normal;
        slang::UInt4 options;
    };
    static_assert(sizeof(Parameters) == 304);
    Parameters p{
        camera,
        {0,                      options.maxLeafSteps,       options.maxLeafVisits, 0},
        {camera.march.x,         camera.valueRange.z,        options.epsilon,       0},
        options.point,
        options.normal,
        {uint32_t(options.mode), colorChannel != UINT32_MAX, info.activeLeafCount,  0}
    };
    auto table = runtime.buffer(transfer.size() * 16, 16, transfer.data());
    slang::RaycastOutput result{runtime.buffer(size_t(pixels) * 16, 16), runtime.buffer(size_t(pixels) * 4, 4),
                                camera.image.x, camera.image.y};
    using B = nvrhi::BindingSetItem;
    runtime.dispatch(
        "OwnedVolumeSections", "renderSection", &p, sizeof(p),
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
nvrhi::BufferHandle compositeOverRGBA8(ethereal::slang::ComputeRuntime& runtime,
                                       portable::BorrowedBufferBinding source,
                                       portable::BorrowedBufferBinding destination, uint32_t count) {
    require(count <= UINT32_MAX / 4, "Composite count");
    for (const auto& b : {source, destination}) {
        require(!count || (b.buffer && b.buffer->getDesc().structStride == 4 && b.byteOffset % 4 == 0 &&
                           b.byteSize % 4 == 0 && b.byteSize >= uint64_t(count) * 4 &&
                           portable::containsBytes(b.buffer->getDesc().byteSize, b.byteOffset, b.byteSize)),
                "Composite buffer range");
    }
    if (!count)
        return {};
    slang::UInt4 work{count, 0, 0, 0};
    auto result = runtime.buffer(size_t(count) * 4, 4);
    using B = nvrhi::BindingSetItem;
    const uint32_t groups = (count + 255) / 256;
    runtime.dispatch(
        "CompositeOver", "compositeOver", &work, sizeof(work),
        {B::StructuredBuffer_SRV(0, source.buffer, nvrhi::Format::UNKNOWN, OwnedVolumeRawViews::range(source)),
         B::StructuredBuffer_SRV(1, destination.buffer, nvrhi::Format::UNKNOWN,
                                 OwnedVolumeRawViews::range(destination)),
         B::StructuredBuffer_UAV(0, result)},
        std::min(groups, 65535u), (groups + 65534) / 65535);
    return result;
}
}  // namespace gvdb::sample
