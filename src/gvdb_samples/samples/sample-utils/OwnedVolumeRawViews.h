// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <gvdb/GVDBNvrhiApi.h>
#include <stdexcept>
namespace gvdb::sample {
// Sample convenience for nonempty sparse scalar publications. Range checks do not
// authenticate third-party shader-visible contents; producers own that contract.
struct OwnedVolumeRawViews {
    portable::SnapshotHandle retained;
    portable::SnapshotDesc info;
    portable::ChannelSnapshotDesc scalar, color;
    portable::BorrowedBufferBinding nodes, children, levels;
    OwnedVolumeRawViews(nvrhi::IDevice* device, portable::SnapshotHandle snapshot, uint32_t scalarChannel = 0,
                        uint32_t colorChannel = UINT32_MAX)
        : retained(std::move(snapshot)) {
        auto require = [](bool v) {
            if (!v)
                throw std::invalid_argument("Invalid owned sparse snapshot view");
        };
        auto check = [&](nvrhi::FRESULT r) { require(NVRHI_SUCCEEDED(r)); };
        require(retained && retained->getDevice() == device);
        info = retained->getDesc();
        require(info.activeLeafCount && info.levelCount && info.levelCount <= portable::c_MaxLevels &&
                info.volume.hierarchy.z == info.levelCount && info.volume.hierarchy.x < info.levelCount);
        auto raw = [&](const portable::BorrowedBufferBinding& v, uint64_t bytes) {
            require(bool(portable::validateRawView({v.buffer, v.byteOffset, v.byteSize}, bytes, false)));
        };
        auto channel = [&](uint32_t index, portable::AtlasFormat format, portable::ChannelSnapshotDesc& out) {
            check(retained->getChannel(index, out));
            require(out.desc.format == format);
            portable::ChannelStorage storage;
            auto r = info.volume.atlasResolutionAndApron;
            require(bool(portable::describeChannel(format, {r.x, r.y, r.z}, storage)) && storage.voxelCount &&
                    storage.voxelCount == out.storage.voxelCount &&
                    storage.logicalBytes == out.storage.logicalBytes &&
                    storage.allocatedBytes == out.storage.allocatedBytes && out.storage.voxelStride == 4);
            raw(out.raw, storage.allocatedBytes);
        };
        channel(scalarChannel, portable::AtlasFormat::R32_FLOAT, scalar);
        if (colorChannel != UINT32_MAX) {
            require(colorChannel != scalarChannel);
            channel(colorChannel, portable::AtlasFormat::RGBA8_UINT, color);
        }
        check(retained->getTopologyBuffer(portable::TopologyBuffer::Nodes, nodes));
        check(retained->getTopologyBuffer(portable::TopologyBuffer::Children, children));
        check(retained->getTopologyBuffer(portable::TopologyBuffer::Levels, levels));
        raw(nodes, sizeof(portable::Node));
        raw(children, 0);
        require(levels.buffer && levels.buffer->getDesc().structStride == sizeof(portable::LevelInfo) &&
                levels.byteOffset % sizeof(portable::LevelInfo) == 0 &&
                levels.byteSize >= uint64_t(info.levelCount) * sizeof(portable::LevelInfo) &&
                portable::containsBytes(levels.buffer->getDesc().byteSize, levels.byteOffset, levels.byteSize));
        for (uint32_t i = 0; i < info.levelCount; ++i) {
            portable::LevelInfo level;
            check(retained->getLevel(i, level));
            require(level.nodes.y == sizeof(portable::Node) &&
                    portable::containsBytes(nodes.byteSize, level.nodes.x,
                                            uint64_t(level.nodes.y) * level.nodes.z));
        }
    }
    static nvrhi::BufferRange range(const portable::BorrowedBufferBinding& v) {
        return {v.byteOffset, v.byteSize};
    }
};
}  // namespace gvdb::sample
