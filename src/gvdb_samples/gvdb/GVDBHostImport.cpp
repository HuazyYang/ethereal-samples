// SPDX-License-Identifier: Apache-2.0
#include "GVDBHostImport.h"
#include <algorithm>
#include <cstring>
#include <new>
#include <stdexcept>
#include <vector>

namespace gvdb::portable {
namespace {
struct Failure {
    nvrhi::FRESULT result;
};
void require(bool condition, nvrhi::FRESULT result = nvrhi::FE_INVALID_ARGS) {
    if (!condition)
        throw Failure{result};
}
void check(nvrhi::FRESULT result) { require(NVRHI_SUCCEEDED(result), result); }
NodeId reference(uint32_t group, uint32_t level, uint64_t index) {
    NodeId id;
    require(NodeId::encode(group, level, index, id));
    return id;
}
struct Span {
    uint64_t first, last;
};
void validatePool(const slang::UInt4& pool, uint64_t bytes, uint64_t minimumStride, uint32_t alignment,
                  std::vector<Span>& spans) {
    require(pool.x % alignment == 0 && pool.x <= bytes);
    // Empty pools do not dereference their record stride.
    if (!pool.z)
        return;
    require(pool.y >= minimumStride && pool.y % alignment == 0);
    require(pool.z <= (bytes - pool.x) / pool.y);
    spans.push_back({pool.x, uint64_t(pool.x) + uint64_t(pool.y) * pool.z});
}
void disjoint(std::vector<Span>& spans) {
    std::sort(spans.begin(), spans.end(), [](Span a, Span b) { return a.first < b.first; });
    for (size_t i = 1; i < spans.size(); ++i)
        require(spans[i - 1].last <= spans[i].first);
}
Node readNode(const std::vector<uint32_t>& bytes, const LevelInfo& info, uint32_t index) {
    Node node;
    std::memcpy(&node,
                reinterpret_cast<const uint8_t*>(bytes.data()) + info.nodes.x + uint64_t(index) * info.nodes.y,
                sizeof(node));
    return node;
}
nvrhi::FRESULT import(const VolumeInfo& volume, const std::vector<LevelInfo>& levels,
                      const std::vector<uint32_t>& nodes, const std::vector<uint32_t>& children,
                      std::unique_ptr<HostVolume>& output, HostVolumeLimits limits) noexcept {
    try {
        require(!levels.empty() && levels.size() <= c_MaxLevels && volume.hierarchy.z == levels.size());
        require(nodes.size() <= c_AddressSpaceBytes / 4 && children.size() <= c_AddressSpaceBytes / 4,
                nvrhi::FE_UNSUPPORTED);
        LevelConfig config;
        config.levelCount = uint32_t(levels.size());
        for (uint32_t level = 0; level < config.levelCount; ++level)
            config.logDimension[level] = levels[level].nodes.w;
        check(validateLevelConfig(config).result);
        std::vector<Span> nodeSpans, childSpans;
        uint64_t childExtent = 1;
        for (uint32_t level = 0; level < config.levelCount; ++level) {
            const auto& info = levels[level];
            const uint32_t width = 1u << config.logDimension[level];
            const uint64_t extent = childExtent * width;
            require(info.children.w == width && info.nodeExtent.x == extent && info.nodeExtent.y == extent &&
                    info.nodeExtent.z == extent && info.childExtent.x == float(childExtent) &&
                    info.childExtent.y == float(childExtent) && info.childExtent.z == float(childExtent));
            validatePool(info.nodes, nodes.size() * 4ull, sizeof(Node), alignof(Node), nodeSpans);
            validatePool(info.children, children.size() * 4ull, uint64_t(width) * width * width * 8, 8,
                         childSpans);
            require(level || !info.children.z);
            childExtent = extent;
        }
        disjoint(nodeSpans);
        disjoint(childSpans);
        const bool empty = volume.hierarchy.y == UINT32_MAX;
        if (empty)
            require(nodeSpans.empty() && childSpans.empty());
        else
            require(volume.hierarchy.x < config.levelCount &&
                    volume.hierarchy.y < levels[volume.hierarchy.x].nodes.z);
        auto candidate = std::make_unique<HostVolume>(limits);
        check(candidate->configure(config));
        // Allocate every record before copying references: source IDs remain exact.
        for (uint32_t level = 0; level < config.levelCount; ++level) {
            const auto& info = levels[level];
            check(candidate->reserve(0, level, info.nodes.z));
            if (level)
                check(candidate->reserve(1, level, info.children.z));
            for (uint32_t index = 0; index < info.nodes.z; ++index) {
                const Node source = readNode(nodes, info, index);
                require(source.level == level);
                NodeId allocated;
                check(candidate->allocateNode(level, source.position, source.flags != 0, allocated));
                require(allocated == reference(0, level, index));
            }
            for (uint32_t index = 0; index < info.children.z; ++index) {
                NodeId allocated;
                check(candidate->allocateChildList(level, allocated));
                require(allocated == reference(1, level, index));
            }
        }
        for (uint32_t level = 0; level < config.levelCount; ++level) {
            const auto& info = levels[level];
            for (uint32_t index = 0; index < info.nodes.z; ++index) {
                Node source = readNode(nodes, info, index);
                source.atlasInterior = {-1, -1, -1};
                *candidate->editNode(reference(0, level, index)) = source;
            }
            for (uint32_t index = 0; index < info.children.z; ++index)
                std::memcpy(candidate->editChildList(reference(1, level, index)),
                            reinterpret_cast<const uint8_t*>(children.data()) + info.children.x +
                                uint64_t(index) * info.children.y,
                            candidate->stride(1, level));
        }
        if (!empty)
            check(candidate->setRoot(reference(0, volume.hierarchy.x, volume.hierarchy.y)));
        check(candidate->finishTopology());
        output = std::move(candidate);
        return nvrhi::FS_OK;
    } catch (const Failure& failure) {
        return failure.result;
    } catch (const std::bad_alloc&) {
        return nvrhi::FE_OUT_OF_MEMORY;
    } catch (const std::length_error&) {
        return nvrhi::FE_OUT_OF_MEMORY;
    } catch (...) {
        return nvrhi::FE_GENERIC_ERROR;
    }
}
}  // namespace
nvrhi::FRESULT importHostTopology(const PackedHostTopology& source, std::unique_ptr<HostVolume>& output,
                                  HostVolumeLimits limits) noexcept {
    return import(source.volume, source.levels, source.nodes, source.children, output, limits);
}
nvrhi::FRESULT importHostTopology(const slang::PackedVolume& source, std::unique_ptr<HostVolume>& output,
                                  HostVolumeLimits limits) noexcept {
    return import(source.volume, source.levels, source.nodes, source.children, output, limits);
}
}  // namespace gvdb::portable
