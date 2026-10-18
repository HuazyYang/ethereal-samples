// SPDX-License-Identifier: Apache-2.0
#pragma once
// An unrelated public implementation used only to verify factory authentication.
// No operation is functional; the factory must reject it before borrowing an owner.
#include "GVDBNvrhiApi.h"
#include <nvrhi/core/foundation.h>
namespace gvdb_api_test {
using namespace gvdb::portable;
class ForeignVolume final : public nvrhi::ObjectImpl<IGVDBVolume> {
 public:
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(ForeignVolume)
        NVRHI_IMPLEMENTS_INTERFACE(IGVDBVolume)
        NVRHI_IMPLEMENTS_INTERFACE(IGVDBHostVolume)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    nvrhi::FRESULT configure(const LevelConfig&) noexcept override { return nvrhi::FE_UNSUPPORTED; }
    nvrhi::FRESULT clearTopology() noexcept override { return nvrhi::FE_UNSUPPORTED; }
    const LevelConfig& getConfiguration() const noexcept override { static const LevelConfig empty; return empty; }
    TopologyRevision getTopologyRevision() const noexcept override { return {}; }
    bool isTopologyDirty() const noexcept override { return {}; }
    NodeId getRootId() const noexcept override { return {}; }
    uint32_t getRange(int32_t level) const noexcept override { return {}; }
    uint32_t getWidth(uint32_t level) const noexcept override { return {}; }
    uint64_t getPoolCount(uint32_t group, uint32_t level) const noexcept override { return {}; }
    uint64_t getPoolCapacity(uint32_t group, uint32_t level) const noexcept override { return {}; }
    uint32_t getPoolStride(uint32_t group, uint32_t level) const noexcept override { return {}; }
    HostPoolView getPool(uint32_t group, uint32_t level) const noexcept override { return {}; }
    MutableHostPoolView editPool(uint32_t group, uint32_t level) noexcept override { return {}; }
    const Node* getNode(NodeId) const noexcept override { return {}; }
    Node* editNode(NodeId) noexcept override { return {}; }
    const NodeId* getChildList(NodeId) const noexcept override { return {}; }
    NodeId* editChildList(NodeId) noexcept override { return {}; }
    nvrhi::FRESULT reservePool(uint32_t group, uint32_t level, uint64_t elements) noexcept override { return nvrhi::FE_UNSUPPORTED; }
    nvrhi::FRESULT allocateNode(uint32_t level, Int3 origin, bool active, NodeId& output) noexcept override { return nvrhi::FE_UNSUPPORTED; }
    nvrhi::FRESULT allocateChildList(uint32_t level, NodeId& output) noexcept override { return nvrhi::FE_UNSUPPORTED; }
    nvrhi::FRESULT setRoot(NodeId) noexcept override { return nvrhi::FE_UNSUPPORTED; }
    nvrhi::FRESULT setActive(NodeId, bool active) noexcept override { return nvrhi::FE_UNSUPPORTED; }
    nvrhi::FRESULT activate(double x, double y, double z, NodeId& output) noexcept override { return nvrhi::FE_UNSUPPORTED; }
    nvrhi::FRESULT activateAtLevel(uint32_t level, Int3 position, NodeId& output) noexcept override { return nvrhi::FE_UNSUPPORTED; }
    NodeId findLeaf(Int3 position) const noexcept override { return {}; }
    bool isAllocated(Int3 position) const noexcept override { return {}; }
    bool isActive(Int3 position) const noexcept override { return {}; }
    nvrhi::FRESULT computeExtents(uint32_t level, Float4 minimum, Float4 maximum, HostExtents& output) const noexcept override { return nvrhi::FE_UNSUPPORTED; }
    nvrhi::FRESULT activateRegion(const HostExtents&, uint64_t& selected) noexcept override { return nvrhi::FE_UNSUPPORTED; }
    nvrhi::FRESULT activateRegionFromMasks(const HostExtents&, const uint8_t*, uint64_t count, uint64_t& selected) noexcept override { return nvrhi::FE_UNSUPPORTED; }
    nvrhi::FRESULT activateRegionFromValues(const HostExtents&, const float*, uint64_t count, float threshold, uint64_t& selected) noexcept override { return nvrhi::FE_UNSUPPORTED; }
    nvrhi::FRESULT activateHalo(const HostExtents&, uint64_t& selected) noexcept override { return nvrhi::FE_UNSUPPORTED; }
    nvrhi::FRESULT validateTopology() const noexcept override { return nvrhi::FE_UNSUPPORTED; }
    nvrhi::FRESULT finishTopology() noexcept override { return nvrhi::FE_UNSUPPORTED; }
    uint32_t getActiveLeafCount() const noexcept override { return {}; }
    HostBounds getBounds() const noexcept override { return {}; }
    nvrhi::IDevice* getDevice() const noexcept override { return {}; }
    nvrhi::FRESULT setChannel(uint32_t channel, const ChannelDesc&) noexcept override { return nvrhi::FE_UNSUPPORTED; }
    nvrhi::FRESULT removeChannel(uint32_t channel) noexcept override { return nvrhi::FE_UNSUPPORTED; }
    nvrhi::FRESULT describePlacement(const PlacementDesc&, VolumeInfo& output) const noexcept override { return nvrhi::FE_UNSUPPORTED; }
    nvrhi::FRESULT prepareGeneration(const GenerationDesc&, IGVDBGenerationTicket** output) noexcept override { return nvrhi::FE_UNSUPPORTED; }
    nvrhi::FRESULT recordAndPublish(RecordingContext, IGVDBGenerationTicket*) noexcept override { return nvrhi::FE_UNSUPPORTED; }
    nvrhi::FRESULT getSnapshot(IGVDBSnapshot** output) const noexcept override { return nvrhi::FE_UNSUPPORTED; }
    nvrhi::FRESULT prepareReadback(IGVDBSnapshot*, uint32_t channelMask, IGVDBReadback** output) noexcept override { return nvrhi::FE_UNSUPPORTED; }
    nvrhi::FRESULT recordReadback(RecordingContext, IGVDBReadback*) noexcept override { return nvrhi::FE_UNSUPPORTED; }
};
} // namespace gvdb_api_test
