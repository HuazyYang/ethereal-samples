// SPDX-License-Identifier: Apache-2.0
#pragma once
// CPU topology owner. No GPU work, device wrapper, or exported ABI.
// Single-threaded mutable owner: synchronize access externally. IDs remain stable
// across growth; clear/reconfigure invalidate them by changing revision().epoch.
#include "GVDBNvrhi.h"
#include <vector>

namespace gvdb::portable {
namespace detail { class VolumeAdoptionBuilder; }
struct IndexBounds {
    Int3 lower{}, upper{}; // Half-open; all zeros when empty.
    bool empty = true;
};
struct CellExtents {
    uint32_t level = 0, cellWidth = 0;
    Int3 minimum{}, maximum{}, resolution{}; // Inclusive cell indices.
    uint64_t count = 0;
};
struct HostVolumeLimits {
    uint64_t maxPoolBytes = c_AddressSpaceBytes; // Per pool; injectable CPU allocation budget.
};
// Topology only. This is deliberately NOT slang::PackedVolume: no placement or
// channels exist yet, and it must not be submitted to current GPU kernels.
// Empty configured topology has levels, null root, no pool bytes and zero bounds.
struct PackedHostTopology {
    VolumeInfo volume = emptyVolumeInfo();
    std::vector<LevelInfo> levels;
    std::vector<uint32_t> nodes, children;
    TopologyRevision revision;
    uint32_t activeLeafCount = 0;
};
class HostVolume {
 public:
    explicit HostVolume(HostVolumeLimits limits = {}) noexcept : m_limits(limits) {}
    HostVolume(const HostVolume&) = delete;
    HostVolume& operator=(const HostVolume&) = delete;
    nvrhi::FRESULT configure(const LevelConfig& config) noexcept;
    nvrhi::FRESULT clear() noexcept; // Keep capacities/configuration; invalidate old IDs by epoch.
    const LevelConfig& configuration() const noexcept { return m_config; }
    const TopologyRevision& revision() const noexcept { return m_revision; }
    bool dirty() const noexcept { return m_dirty; }
    NodeId root() const noexcept { return m_root; }
    uint32_t range(int level) const noexcept;
    uint32_t width(uint32_t level) const noexcept;
    uint64_t count(uint32_t group, uint32_t level) const noexcept;
    uint64_t capacity(uint32_t group, uint32_t level) const noexcept;
    uint32_t stride(uint32_t group, uint32_t level) const noexcept;
    HostPoolView pool(uint32_t group, uint32_t level) const noexcept;
    MutableHostPoolView editPool(uint32_t group, uint32_t level) noexcept;
    const Node* node(NodeId id) const noexcept;
    Node* editNode(NodeId id) noexcept;
    const NodeId* childList(NodeId id) const noexcept;
    NodeId* editChildList(NodeId id) noexcept;
    // Growth may invalidate every borrowed pointer in the affected pool, even
    // when a later step of a transactional activation fails. Logical IDs survive.
    nvrhi::FRESULT reserve(uint32_t group, uint32_t level, uint64_t elements) noexcept;
    nvrhi::FRESULT allocateNode(uint32_t level, Int3 origin, bool active, NodeId& output) noexcept;
    nvrhi::FRESULT allocateChildList(uint32_t level, NodeId& output) noexcept;
    nvrhi::FRESULT setRoot(NodeId root) noexcept;
    nvrhi::FRESULT setActive(NodeId node, bool active) noexcept;
    nvrhi::FRESULT activate(double x, double y, double z, NodeId& output) noexcept;
    nvrhi::FRESULT activateAtLevel(uint32_t level, Int3 position, NodeId& output) noexcept;
    // Choose one compatible ancestor for a complete incoming half-open batch.
    // Preserves the whole existing root, every existing node ID/origin and leaf
    // phase. Allocates only an ancestor and its path to the old root; callers
    // still activate selected leaves in their chosen order. Empty bounds do
    // nothing. Failure rolls back logical state; pool reservations may grow.
    nvrhi::FRESULT encloseBounds(const IndexBounds& bounds, uint32_t minimumLevel = 0) noexcept;
    // Extents select level-1 cells in x-fast order. selected counts matching
    // cells, including existing nodes. Level zero is a no-op. Failure clears
    // selected and rolls back the entire batch; reservations may still grow.
    nvrhi::FRESULT activateRegion(const CellExtents& extents, uint64_t& selected) noexcept;
    nvrhi::FRESULT activateRegionFromMasks(const CellExtents& extents, const uint8_t* masks,
                                         uint64_t maskCount, uint64_t& selected) noexcept;
    nvrhi::FRESULT activateRegionFromValues(const CellExtents& extents, const float* values,
                                          uint64_t valueCount, float threshold, uint64_t& selected) noexcept;
    // One simultaneous six-face layer, using allocated leaf presence exactly
    // like legacy isActive (including flags==0), not newly added halo cells.
    nvrhi::FRESULT activateHalo(const CellExtents& extents, uint64_t& selected) noexcept;
    NodeId findLeaf(Int3 position) const noexcept;
    bool isAllocated(Int3 position) const noexcept { return !findLeaf(position).isNull(); }
    bool isActive(Int3 position) const noexcept;
    // Validate graph/geometry/reciprocal links and publish CPU bounds/revision.
    // Manual pool edits need this explicit boundary. No upload or wait is implied.
    // Every successful finish advances topology, including an unchanged graph.
    nvrhi::FRESULT finishTopology() noexcept;
    nvrhi::FRESULT validate() const noexcept;
    uint32_t activeLeafCount() const noexcept { return m_activeLeaves; } // Last finishTopology.
    const IndexBounds& bounds() const noexcept { return m_bounds; }    // Last finishTopology.
    // Requires a finished topology, preserves output on failure. Packing emits
    // fixed 64/8-byte records and checked 32-bit relative pool offsets.
    nvrhi::FRESULT packTopology(PackedHostTopology& output) const noexcept;
    // Preserve legacy subtract-in-float, then truncate-toward-zero extents.
    // Point activation and covering-node placement instead use floor.
    nvrhi::FRESULT computeExtents(uint32_t level, Float4 minimum, Float4 maximum,
                                 CellExtents& output) const noexcept;
 private:
    struct Transaction;
    HostVolumeLimits m_limits;
    LevelConfig m_config;
    std::array<uint32_t, c_MaxLevels> m_ranges{};
    std::array<std::vector<Node>, c_MaxLevels> m_nodes;
    std::array<std::vector<NodeId>, c_MaxLevels> m_children;
    NodeId m_root;
    TopologyRevision m_revision;
    IndexBounds m_bounds;
    uint32_t m_activeLeaves = 0;
    bool m_dirty = false, m_manualEdits = false;
    uint64_t childEntries(uint32_t level) const noexcept;
    Node* nodePtr(NodeId id) noexcept;
    NodeId* childPtr(NodeId id) noexcept;
    void reserveImpl(uint32_t group, uint32_t level, uint64_t elements);
    NodeId allocateNodeImpl(uint32_t level, Int3 origin, bool active);
    NodeId allocateChildImpl(uint32_t level);
    uint32_t childBit(const Node& parent, Int3 position) const noexcept;
    void link(NodeId parent, NodeId child, uint32_t bit, Transaction& transaction);
    NodeId descend(NodeId parent, Int3 position, uint32_t stopLevel,
                   NodeId existing, Transaction& transaction);
    NodeId activateImpl(uint32_t level, Int3 position, Transaction& transaction);
    enum class RegionSelection { All, Masks, Values };
    void validateExtents(const CellExtents& extents) const;
    nvrhi::FRESULT activateRegionImpl(const CellExtents& extents, RegionSelection selection,
                                     const void* data, uint64_t dataCount, float threshold,
                                     uint64_t& selected) noexcept;
    void validateImpl() const;
    void swap(HostVolume& other) noexcept;
    friend class NvrhiVolume;
    friend class detail::VolumeAdoptionBuilder;
};
} // namespace gvdb::portable
