// SPDX-License-Identifier: Apache-2.0
#include "GVDBHostVolume.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <unordered_map>

namespace gvdb::portable {
namespace {
struct Failure { nvrhi::FRESULT result; };
[[noreturn]] void fail(nvrhi::FRESULT result = nvrhi::FE_INVALID_ARGS) { throw Failure{result}; }
#define GVDB_HOST_CATCH \
    catch (const Failure& e) { return e.result; } \
    catch (const std::bad_alloc&) { return nvrhi::FE_OUT_OF_MEMORY; } \
    catch (const std::length_error&) { return nvrhi::FE_OUT_OF_MEMORY; } \
    catch (...) { return nvrhi::FE_GENERIC_ERROR; }
int64_t floorMultiple(int64_t value, int64_t width) {
    int64_t quotient = value / width;
    if (value % width < 0) --quotient;
    return quotient * width;
}
bool equal(Int3 a, Int3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
bool validOrigin(Int3 p, uint32_t width) {
    return int64_t(p.x) + width <= INT32_MAX && int64_t(p.y) + width <= INT32_MAX &&
           int64_t(p.z) + width <= INT32_MAX;
}
NodeId reference(uint32_t group, uint32_t level, uint64_t index) {
    NodeId result;
    if (!NodeId::encode(group, level, index, result)) fail();
    return result;
}
uint64_t increment(uint64_t value) {
    uint64_t next;
    if (!nextRevision(value, next)) fail(nvrhi::FE_UNSUPPORTED);
    return next;
}
}
// Roll back logical state, not reservation growth. Any attempted growth can
// invalidate borrowed pointers even if activation later fails. Existing IDs,
// records, links, root, revisions and committed bounds remain unchanged.
struct HostVolume::Transaction {
    HostVolume& owner;
    NodeId oldRoot;
    bool oldDirty, oldManual, committed = false, batch = false;
    // Single-point activation uses the bounded allocation-free journal below.
    // Batches may touch arbitrarily many existing slots. Journal growth occurs
    // before each mutation; allocation failure therefore remains reversible.
    std::unordered_map<uint64_t, Node> batchNodes;
    std::unordered_map<uint64_t, NodeId> batchSlots;
    std::array<size_t, c_MaxLevels> nodeCounts{}, childSizes{};
    struct NodeChange { NodeId id; Node value; };
    struct SlotChange { NodeId list; uint32_t bit; NodeId value; };
    std::array<NodeChange, c_MaxLevels * 3> nodeChanges{};
    std::array<SlotChange, c_MaxLevels * 3> slotChanges{};
    size_t numNodes = 0, numSlots = 0;
    explicit Transaction(HostVolume& volume, bool isBatch = false) : owner(volume), oldRoot(volume.m_root),
        oldDirty(volume.m_dirty), oldManual(volume.m_manualEdits), batch(isBatch) {
        for (uint32_t l = 0; l < c_MaxLevels; ++l) {
            nodeCounts[l] = owner.m_nodes[l].size();
            childSizes[l] = owner.m_children[l].size();
        }
    }
    void saveNode(NodeId id) {
        if (id.index() >= nodeCounts[id.level()]) return;
        if (batch) { batchNodes.try_emplace(id.value, *owner.nodePtr(id)); return; }
        for (size_t i = 0; i < numNodes; ++i) if (nodeChanges[i].id == id) return;
        if (numNodes == nodeChanges.size()) fail(nvrhi::FE_GENERIC_ERROR);
        nodeChanges[numNodes++] = {id, *owner.nodePtr(id)};
    }
    void saveSlot(NodeId list, uint32_t bit) {
        if (list.index() * owner.childEntries(list.level()) >= childSizes[list.level()]) return;
        if (batch) {
            const uint64_t absoluteSlot = list.index() * owner.childEntries(list.level()) + bit;
            batchSlots.try_emplace((absoluteSlot << 8) | list.level(), owner.childPtr(list)[bit]);
            return;
        }
        for (size_t i = 0; i < numSlots; ++i)
            if (slotChanges[i].list == list && slotChanges[i].bit == bit) return;
        if (numSlots == slotChanges.size()) fail(nvrhi::FE_GENERIC_ERROR);
        slotChanges[numSlots++] = {list, bit, owner.childPtr(list)[bit]};
    }
    ~Transaction() {
        if (committed) return;
        for (const auto& change : batchSlots)
            owner.m_children[uint8_t(change.first)][size_t(change.first >> 8)] = change.second;
        for (const auto& change : batchNodes)
            *owner.nodePtr(NodeId{change.first}) = change.second;
        for (size_t i = 0; i < numSlots; ++i)
            owner.childPtr(slotChanges[i].list)[slotChanges[i].bit] = slotChanges[i].value;
        for (size_t i = 0; i < numNodes; ++i)
            *owner.nodePtr(nodeChanges[i].id) = nodeChanges[i].value;
        for (uint32_t l = 0; l < c_MaxLevels; ++l) {
            owner.m_nodes[l].resize(nodeCounts[l]);
            owner.m_children[l].resize(childSizes[l]);
        }
        owner.m_root = oldRoot;
        owner.m_dirty = oldDirty;
        owner.m_manualEdits = oldManual;
    }
};
uint32_t HostVolume::range(int level) const noexcept {
    if (level < 0 || !m_config.levelCount) return 1;
    return m_ranges[std::min(uint32_t(level), m_config.levelCount - 1)];
}
uint32_t HostVolume::width(uint32_t level) const noexcept {
    return level < m_config.levelCount ? 1u << m_config.logDimension[level] : 0;
}
uint64_t HostVolume::childEntries(uint32_t level) const noexcept {
    const uint64_t side = width(level);
    return level ? side * side * side : 0;
}
uint32_t HostVolume::stride(uint32_t group, uint32_t level) const noexcept {
    if (group > 1 || level >= m_config.levelCount) return 0;
    return group ? uint32_t(childEntries(level) * sizeof(NodeId)) : uint32_t(sizeof(Node));
}
uint64_t HostVolume::count(uint32_t group, uint32_t level) const noexcept {
    if (group > 1 || level >= m_config.levelCount) return 0;
    if (!group) return m_nodes[level].size();
    const uint64_t entries = childEntries(level);
    return entries ? m_children[level].size() / entries : 0;
}
uint64_t HostVolume::capacity(uint32_t group, uint32_t level) const noexcept {
    if (group > 1 || level >= m_config.levelCount) return 0;
    if (!group) return m_nodes[level].capacity();
    const uint64_t entries = childEntries(level);
    return entries ? m_children[level].capacity() / entries : 0;
}
HostPoolView HostVolume::pool(uint32_t group, uint32_t level) const noexcept {
    if (!stride(group, level)) return {};
    const void* data = group ? static_cast<const void*>(m_children[level].data()) : m_nodes[level].data();
    return {data, count(group, level), stride(group, level)};
}
MutableHostPoolView HostVolume::editPool(uint32_t group, uint32_t level) noexcept {
    const auto view = pool(group, level);
    if (view.stride) m_dirty = m_manualEdits = true;
    return {const_cast<void*>(view.data), view.count, view.stride};
}
const Node* HostVolume::node(NodeId id) const noexcept {
    return !id.isNull() && !id.group() && id.level() < m_config.levelCount && id.index() < m_nodes[id.level()].size()
        ? &m_nodes[id.level()][size_t(id.index())] : nullptr;
}
Node* HostVolume::nodePtr(NodeId id) noexcept { return const_cast<Node*>(static_cast<const HostVolume*>(this)->node(id)); }
Node* HostVolume::editNode(NodeId id) noexcept {
    auto* result = nodePtr(id);
    if (result) m_dirty = m_manualEdits = true;
    return result;
}
const NodeId* HostVolume::childList(NodeId id) const noexcept {
    return !id.isNull() && id.group() == 1 && id.level() > 0 && id.level() < m_config.levelCount &&
        id.index() < count(1, id.level()) ? m_children[id.level()].data() + size_t(id.index() * childEntries(id.level())) : nullptr;
}
NodeId* HostVolume::childPtr(NodeId id) noexcept { return const_cast<NodeId*>(static_cast<const HostVolume*>(this)->childList(id)); }
NodeId* HostVolume::editChildList(NodeId id) noexcept {
    auto* result = childPtr(id);
    if (result) m_dirty = m_manualEdits = true;
    return result;
}
void HostVolume::reserveImpl(uint32_t group, uint32_t level, uint64_t elements) {
    const uint64_t elementStride = stride(group, level);
    if (!elementStride) fail();
    const uint64_t maximum = std::min(m_limits.maxPoolBytes, c_AddressSpaceBytes) / elementStride;
    if (elements > maximum || elements > UINT32_MAX) fail(nvrhi::FE_OUT_OF_MEMORY);
    if (elements <= capacity(group, level)) return;
    uint64_t grown = std::max<uint64_t>(16, capacity(group, level) * 2);
    grown = std::min(maximum, std::max(grown, elements));
    if (group) m_children[level].reserve(size_t(grown * childEntries(level)));
    else m_nodes[level].reserve(size_t(grown));
}
nvrhi::FRESULT HostVolume::reserve(uint32_t group, uint32_t level, uint64_t elements) noexcept {
    try { reserveImpl(group, level, elements); return nvrhi::FS_OK; } GVDB_HOST_CATCH
}
void HostVolume::swap(HostVolume& other) noexcept {
    using std::swap;
    swap(m_limits, other.m_limits); swap(m_config, other.m_config); swap(m_ranges, other.m_ranges);
    swap(m_nodes, other.m_nodes); swap(m_children, other.m_children); swap(m_root, other.m_root);
    swap(m_revision, other.m_revision); swap(m_bounds, other.m_bounds); swap(m_activeLeaves, other.m_activeLeaves);
    swap(m_dirty, other.m_dirty); swap(m_manualEdits, other.m_manualEdits);
}
nvrhi::FRESULT HostVolume::configure(const LevelConfig& config) noexcept {
    try {
        const auto valid = validateLevelConfig(config);
        if (!valid) return valid.result;
        if (!m_limits.maxPoolBytes) fail();
        HostVolume candidate(m_limits);
        candidate.m_config = config;
        uint32_t extent = 1;
        for (uint32_t l = 0; l < config.levelCount; ++l) {
            extent *= 1u << config.logDimension[l];
            candidate.m_ranges[l] = extent;
            const uint64_t childBytes = candidate.childEntries(l) * sizeof(NodeId);
            if (childBytes > UINT32_MAX) fail(nvrhi::FE_UNSUPPORTED);
            const uint64_t initial = config.initialNodeCapacity[l] ? config.initialNodeCapacity[l] : 4;
            candidate.m_config.initialNodeCapacity[l] = uint32_t(initial);
            if (initial > m_limits.maxPoolBytes / sizeof(Node) || initial > c_AddressSpaceBytes / sizeof(Node))
                fail(nvrhi::FE_OUT_OF_MEMORY);
            candidate.m_nodes[l].reserve(size_t(initial));
            if (childBytes) {
                if (initial > std::min(m_limits.maxPoolBytes, c_AddressSpaceBytes) / childBytes)
                    fail(nvrhi::FE_OUT_OF_MEMORY);
                candidate.m_children[l].reserve(size_t(initial * candidate.childEntries(l)));
            }
        }
        for (uint32_t l = config.levelCount; l < c_MaxLevels; ++l) {
            candidate.m_config.logDimension[l] = 0;
            candidate.m_config.initialNodeCapacity[l] = 0;
        }
        candidate.m_revision = {increment(m_revision.epoch), increment(m_revision.topology), 0};
        swap(candidate);
        return nvrhi::FS_OK;
    } GVDB_HOST_CATCH
}
nvrhi::FRESULT HostVolume::clear() noexcept {
    try {
        if (!m_config.levelCount) fail();
        const TopologyRevision next{increment(m_revision.epoch), increment(m_revision.topology), 0};
        for (auto& nodes : m_nodes) nodes.clear();
        for (auto& children : m_children) children.clear();
        m_root = {}; m_bounds = {}; m_activeLeaves = 0; m_dirty = m_manualEdits = false; m_revision = next;
        return nvrhi::FS_OK;
    } GVDB_HOST_CATCH
}
NodeId HostVolume::allocateNodeImpl(uint32_t level, Int3 origin, bool active) {
    if (level >= m_config.levelCount || !validOrigin(origin, range(int(level)))) fail();
    const uint64_t index = count(0, level);
    reserveImpl(0, level, index + 1);
    Node value;
    value.level = uint8_t(level); value.flags = active ? 1 : 0; value.position = origin;
    value.atlasInterior = {-1, -1, -1};
    m_nodes[level].push_back(value);
    m_dirty = true;
    return reference(0, level, index);
}
NodeId HostVolume::allocateChildImpl(uint32_t level) {
    if (!level || level >= m_config.levelCount) fail();
    const uint64_t index = count(1, level);
    reserveImpl(1, level, index + 1);
    m_children[level].resize(size_t((index + 1) * childEntries(level))); // NodeId defaults to null.
    m_dirty = true;
    return reference(1, level, index);
}
nvrhi::FRESULT HostVolume::allocateNode(uint32_t level, Int3 origin, bool active, NodeId& output) noexcept {
    output = {};
    try { output = allocateNodeImpl(level, origin, active); m_manualEdits = true; return nvrhi::FS_OK; } GVDB_HOST_CATCH
}
nvrhi::FRESULT HostVolume::allocateChildList(uint32_t level, NodeId& output) noexcept {
    output = {};
    try { output = allocateChildImpl(level); m_manualEdits = true; return nvrhi::FS_OK; } GVDB_HOST_CATCH
}
nvrhi::FRESULT HostVolume::setRoot(NodeId id) noexcept {
    if (!id.isNull() && (!node(id) || !node(id)->parent.isNull())) return nvrhi::FE_INVALID_ARGS;
    m_root = id; m_dirty = m_manualEdits = true; return nvrhi::FS_OK;
}
nvrhi::FRESULT HostVolume::setActive(NodeId id, bool active) noexcept {
    auto* value = nodePtr(id);
    if (!value) return nvrhi::FE_INVALID_ARGS;
    if (value->flags != uint8_t(active)) { value->flags = uint8_t(active); m_dirty = true; }
    return nvrhi::FS_OK;
}
uint32_t HostVolume::childBit(const Node& parent, Int3 position) const noexcept {
    if (parent.level >= m_config.levelCount) return UINT32_MAX;
    const int64_t x = int64_t(position.x) - parent.position.x, y = int64_t(position.y) - parent.position.y,
                  z = int64_t(position.z) - parent.position.z;
    const uint32_t extent = range(parent.level);
    if (x < 0 || y < 0 || z < 0 || x >= extent || y >= extent || z >= extent) return UINT32_MAX;
    const uint32_t childWidth = range(int(parent.level) - 1), side = width(parent.level);
    return uint32_t((uint64_t(z / childWidth) * side + uint64_t(y / childWidth)) * side + uint64_t(x / childWidth));
}
void HostVolume::link(NodeId parentId, NodeId childId, uint32_t bit, Transaction& transaction) {
    const Node* parent = node(parentId); const Node* child = node(childId);
    if (!parent || !child || parent->level != child->level + 1 || bit >= childEntries(parent->level)) fail();
    transaction.saveNode(parentId); transaction.saveNode(childId);
    if (parent->children.isNull()) {
        const NodeId list = allocateChildImpl(parent->level);
        nodePtr(parentId)->children = list;
    }
    const NodeId list = node(parentId)->children;
    if (!childPtr(list)) fail();
    transaction.saveSlot(list, bit);
    if (!childPtr(list)[bit].isNull() && childPtr(list)[bit] != childId) fail();
    childPtr(list)[bit] = childId;
    nodePtr(childId)->parent = parentId;
}
NodeId HostVolume::descend(NodeId current, Int3 position, uint32_t stopLevel,
                           NodeId existing, Transaction& transaction) {
    for (uint32_t iteration = 0; iteration < c_MaxLevels; ++iteration) {
        const Node* currentNode = node(current);
        if (!currentNode || currentNode->level != current.level()) fail();
        const Node parent = *currentNode; // Never retain a pool pointer across allocation.
        if (parent.level == stopLevel) return current;
        if (parent.level < stopLevel) fail();
        const uint32_t bit = childBit(parent, position);
        if (bit == UINT32_MAX) fail();
        const NodeId* list = childList(parent.children);
        NodeId child = list ? list[bit] : NodeId{};
        if (!existing.isNull() && parent.level == existing.level() + 1) {
            if (!child.isNull() && child != existing) fail();
            link(current, existing, bit, transaction);
            return existing;
        }
        if (child.isNull()) {
            const uint32_t side = width(parent.level), childWidth = range(parent.level - 1);
            const Int3 origin{int32_t(int64_t(parent.position.x) + int64_t(bit % side) * childWidth),
                int32_t(int64_t(parent.position.y) + int64_t((bit / side) % side) * childWidth),
                int32_t(int64_t(parent.position.z) + int64_t(bit / uint64_t(side) / side) * childWidth)};
            child = allocateNodeImpl(parent.level - 1, origin, true);
            link(current, child, bit, transaction);
        }
        current = child;
    }
    fail();
}
NodeId HostVolume::activateImpl(uint32_t level, Int3 position, Transaction& transaction) {
    if (level >= m_config.levelCount) fail();
    if (m_root.isNull()) {
        const int64_t cellWidth = range(int(level));
        const Int3 origin{int32_t(floorMultiple(position.x, cellWidth)), int32_t(floorMultiple(position.y, cellWidth)),
                          int32_t(floorMultiple(position.z, cellWidth))};
        m_root = allocateNodeImpl(level, origin, true);
    } else if (node(m_root)->level < level || childBit(*node(m_root), position) == UINT32_MAX) {
        const NodeId previous = m_root;
        const Node old = *node(previous);
        const int64_t oldWidth = range(old.level);
        Int3 chosen{}; uint32_t chosenLevel = m_config.levelCount;
        for (uint32_t l = std::max(uint32_t(old.level) + 1, level); l < m_config.levelCount; ++l) {
            const int64_t newWidth = range(int(l));
            const int32_t oldCoordinates[] = {old.position.x, old.position.y, old.position.z};
            const int32_t target[] = {position.x, position.y, position.z};
            int32_t origin[3]{}; bool fits = true;
            for (uint32_t axis = 0; axis < 3; ++axis) {
                const int64_t oldStart = oldCoordinates[axis], targetPoint = target[axis];
                const int64_t minimum = std::min(oldStart, targetPoint);
                const int64_t maximum = std::max(oldStart + oldWidth, targetPoint + 1);
                // Prefer legacy global alignment when it preserves the whole old
                // subtree. Otherwise use a compatible old-root-relative grid.
                int64_t corner = floorMultiple(targetPoint, newWidth);
                if (corner > minimum || corner + newWidth < maximum || (oldStart - corner) % oldWidth)
                    corner = oldStart + floorMultiple(minimum - oldStart, oldWidth);
                if (corner < INT32_MIN || corner + newWidth > INT32_MAX || corner > minimum ||
                    corner + newWidth < maximum) { fits = false; break; }
                origin[axis] = int32_t(corner);
            }
            if (fits) { chosen = {origin[0], origin[1], origin[2]}; chosenLevel = l; break; }
        }
        if (chosenLevel == m_config.levelCount) fail(nvrhi::FE_UNSUPPORTED);
        m_root = allocateNodeImpl(chosenLevel, chosen, true);
        descend(m_root, old.position, old.level, previous, transaction);
    }
    return descend(m_root, position, level, {}, transaction);
}
nvrhi::FRESULT HostVolume::activateAtLevel(uint32_t level, Int3 position, NodeId& output) noexcept {
    output = {};
    try {
        if (m_manualEdits) validateImpl();
        Transaction transaction(*this);
        const NodeId result = activateImpl(level, position, transaction);
        transaction.committed = true;
        output = result;
        return nvrhi::FS_OK;
    } GVDB_HOST_CATCH
}
nvrhi::FRESULT HostVolume::activate(double x, double y, double z, NodeId& output) noexcept {
    output = {};
    const double values[] = {x, y, z}; int32_t coordinates[3]{};
    for (uint32_t axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(values[axis])) return nvrhi::FE_INVALID_ARGS;
        const double floored = std::floor(values[axis]);
        if (floored < INT32_MIN || floored > INT32_MAX) return nvrhi::FE_INVALID_ARGS;
        coordinates[axis] = int32_t(floored);
    }
    return activateAtLevel(0, {coordinates[0], coordinates[1], coordinates[2]}, output);
}
void HostVolume::validateExtents(const CellExtents& extents) const {
    if (extents.level >= m_config.levelCount || extents.cellWidth != range(int(extents.level) - 1)) fail();
    const int32_t low[] = {extents.minimum.x, extents.minimum.y, extents.minimum.z};
    const int32_t high[] = {extents.maximum.x, extents.maximum.y, extents.maximum.z};
    const int32_t resolution[] = {extents.resolution.x, extents.resolution.y, extents.resolution.z};
    uint64_t count = 1;
    for (uint32_t axis = 0; axis < 3; ++axis) {
        const int64_t dimension = std::max<int64_t>(0, int64_t(high[axis]) - low[axis] + 1);
        if (dimension > INT32_MAX || dimension != resolution[axis]) fail();
        if (dimension && count > UINT64_MAX / uint64_t(dimension)) fail(nvrhi::FE_UNSUPPORTED);
        count *= uint64_t(dimension);
    }
    if (count != extents.count) fail();
    if (!count || !extents.level) return;
    for (uint32_t axis = 0; axis < 3; ++axis) {
        const int64_t first = int64_t(low[axis]) * extents.cellWidth;
        const int64_t last = int64_t(high[axis]) * extents.cellWidth;
        if (first < INT32_MIN || last > INT32_MAX) fail(nvrhi::FE_UNSUPPORTED);
    }
}
nvrhi::FRESULT HostVolume::activateRegionImpl(const CellExtents& extents, RegionSelection selection,
                                             const void* data, uint64_t dataCount, float threshold,
                                             uint64_t& selected) noexcept {
    selected = 0;
    try {
        validateExtents(extents);
        if (!extents.level || !extents.count) return nvrhi::FS_OK;
        if (selection != RegionSelection::All) {
            if (!data || dataCount < extents.count) fail();
            const uint64_t elementBytes = selection == RegionSelection::Values ? sizeof(float) : sizeof(uint8_t);
            if (extents.count > SIZE_MAX / elementBytes) fail(nvrhi::FE_UNSUPPORTED);
        }
        if (m_manualEdits) validateImpl();
        // Resolve selection before growth: input may itself borrow host-pool
        // bytes, whose allocation can move during activation.
        std::vector<uint8_t> tags;
        if (selection != RegionSelection::All) {
            tags.resize(size_t(extents.count));
            for (uint64_t i = 0; i < extents.count; ++i)
                tags[size_t(i)] = selection == RegionSelection::Masks ? uint8_t(static_cast<const uint8_t*>(data)[i] != 0) :
                    uint8_t(static_cast<const float*>(data)[i] > threshold);
        }
        Transaction transaction(*this, true);
        uint64_t index = 0, matches = 0;
        for (int64_t z = extents.minimum.z; z <= extents.maximum.z; ++z)
            for (int64_t y = extents.minimum.y; y <= extents.maximum.y; ++y)
                for (int64_t x = extents.minimum.x; x <= extents.maximum.x; ++x, ++index) {
                    if (selection != RegionSelection::All && !tags[size_t(index)]) continue;
                    const Int3 position{int32_t(x * extents.cellWidth), int32_t(y * extents.cellWidth),
                                        int32_t(z * extents.cellWidth)};
                    activateImpl(extents.level - 1, position, transaction);
                    ++matches;
                }
        transaction.committed = true;
        selected = matches;
        return nvrhi::FS_OK;
    } GVDB_HOST_CATCH
}
nvrhi::FRESULT HostVolume::activateRegion(const CellExtents& extents, uint64_t& selected) noexcept {
    return activateRegionImpl(extents, RegionSelection::All, nullptr, 0, 0, selected);
}
nvrhi::FRESULT HostVolume::activateRegionFromMasks(const CellExtents& extents, const uint8_t* masks,
                                                 uint64_t maskCount, uint64_t& selected) noexcept {
    return activateRegionImpl(extents, RegionSelection::Masks, masks, maskCount, 0, selected);
}
nvrhi::FRESULT HostVolume::activateRegionFromValues(const CellExtents& extents, const float* values,
                                                  uint64_t valueCount, float threshold, uint64_t& selected) noexcept {
    return activateRegionImpl(extents, RegionSelection::Values, values, valueCount, threshold, selected);
}
nvrhi::FRESULT HostVolume::activateHalo(const CellExtents& extents, uint64_t& selected) noexcept {
    selected = 0;
    try {
        validateExtents(extents);
        if (!extents.level || !extents.count) return nvrhi::FS_OK;
        if (m_manualEdits) validateImpl();
        const Int3 directions[] = {{-1,0,0},{1,0,0},{0,-1,0},{0,1,0},{0,0,-1},{0,0,1}};
        std::vector<Int3> positions;
        for (int64_t z = extents.minimum.z; z <= extents.maximum.z; ++z)
            for (int64_t y = extents.minimum.y; y <= extents.maximum.y; ++y)
                for (int64_t x = extents.minimum.x; x <= extents.maximum.x; ++x) {
                    const Int3 position{int32_t(x * extents.cellWidth), int32_t(y * extents.cellWidth),
                                        int32_t(z * extents.cellWidth)};
                    if (isAllocated(position)) continue;
                    for (Int3 direction : directions) {
                        const int64_t nx = int64_t(position.x) + int64_t(direction.x) * extents.cellWidth;
                        const int64_t ny = int64_t(position.y) + int64_t(direction.y) * extents.cellWidth;
                        const int64_t nz = int64_t(position.z) + int64_t(direction.z) * extents.cellWidth;
                        if (nx < INT32_MIN || nx > INT32_MAX || ny < INT32_MIN || ny > INT32_MAX ||
                            nz < INT32_MIN || nz > INT32_MAX) continue; // Outside the supported index space.
                        if (isAllocated({int32_t(nx), int32_t(ny), int32_t(nz)})) {
                            positions.push_back(position);
                            break;
                        }
                    }
                }
        Transaction transaction(*this, true);
        for (Int3 position : positions) activateImpl(extents.level - 1, position, transaction);
        transaction.committed = true;
        selected = positions.size();
        return nvrhi::FS_OK;
    } GVDB_HOST_CATCH
}
NodeId HostVolume::findLeaf(Int3 position) const noexcept {
    NodeId current = m_root;
    for (uint32_t iteration = 0; iteration < c_MaxLevels; ++iteration) {
        const Node* value = node(current);
        if (!value || value->level != current.level()) return {};
        const uint32_t bit = childBit(*value, position);
        if (bit == UINT32_MAX) return {};
        if (!value->level) return current;
        const NodeId* list = childList(value->children);
        if (!list || value->children.level() != value->level || bit >= childEntries(value->level)) return {};
        current = list[bit];
    }
    return {};
}
bool HostVolume::isActive(Int3 position) const noexcept {
    const Node* value = node(findLeaf(position));
    return value && value->flags != 0;
}
void HostVolume::validateImpl() const {
    if (!validateLevelConfig(m_config)) fail();
    if (m_root.isNull()) {
        for (uint32_t l = 0; l < m_config.levelCount; ++l)
            if (count(0, l) || count(1, l)) fail();
        return;
    }
    if (!node(m_root) || !node(m_root)->parent.isNull()) fail();
    for (uint32_t l = 0; l < m_config.levelCount; ++l) {
        std::vector<uint8_t> owners(size_t(count(1, l)), 0);
        for (uint64_t i = 0; i < count(0, l); ++i) {
            const NodeId id = reference(0, l, i); const Node& value = *node(id);
            if (value.level != l || !validOrigin(value.position, range(int(l)))) fail();
            if (id != m_root) {
                const Node* parent = node(value.parent);
                if (!parent || value.parent.level() != l + 1 || parent->level != l + 1 ||
                    parent->children.level() != parent->level) fail();
                const uint32_t bit = childBit(*parent, value.position);
                const NodeId* list = childList(parent->children);
                if (!list || bit == UINT32_MAX || list[bit] != id) fail();
            }
            if (!l) { if (!value.children.isNull()) fail(); continue; }
            if (value.children.isNull()) continue;
            if (value.children.level() != l || !childList(value.children)) fail();
            if (owners[size_t(value.children.index())]++) fail();
            const NodeId* list = childList(value.children);
            const uint32_t side = width(l), childWidth = range(int(l) - 1);
            for (uint64_t bit = 0; bit < childEntries(l); ++bit) {
                if (list[bit].isNull()) continue;
                const Node* child = node(list[bit]);
                if (!child || child->level != l - 1 || child->parent != id) fail();
                const Int3 expected{int32_t(int64_t(value.position.x) + int64_t(bit % side) * childWidth),
                    int32_t(int64_t(value.position.y) + int64_t((bit / side) % side) * childWidth),
                    int32_t(int64_t(value.position.z) + int64_t(bit / side / side) * childWidth)};
                if (!equal(child->position, expected)) fail();
            }
        }
        for (uint8_t owner : owners) if (owner != 1) fail();
    }
}
nvrhi::FRESULT HostVolume::validate() const noexcept {
    try { validateImpl(); return nvrhi::FS_OK; } GVDB_HOST_CATCH
}
nvrhi::FRESULT HostVolume::finishTopology() noexcept {
    try {
        validateImpl();
        IndexBounds nextBounds; uint32_t active = 0;
        for (const Node& value : m_nodes[0]) if (value.flags) {
            const Int3 end{int32_t(int64_t(value.position.x) + range(0)), int32_t(int64_t(value.position.y) + range(0)),
                          int32_t(int64_t(value.position.z) + range(0))};
            if (nextBounds.empty) { nextBounds.lower = value.position; nextBounds.upper = end; nextBounds.empty = false; }
            else {
                nextBounds.lower = {std::min(nextBounds.lower.x, value.position.x), std::min(nextBounds.lower.y, value.position.y), std::min(nextBounds.lower.z, value.position.z)};
                nextBounds.upper = {std::max(nextBounds.upper.x, end.x), std::max(nextBounds.upper.y, end.y), std::max(nextBounds.upper.z, end.z)};
            }
            ++active;
        }
        const uint64_t next = increment(m_revision.topology);
        m_bounds = nextBounds; m_activeLeaves = active; m_revision.topology = next;
        m_dirty = m_manualEdits = false;
        return nvrhi::FS_OK;
    } GVDB_HOST_CATCH
}
nvrhi::FRESULT HostVolume::packTopology(PackedHostTopology& output) const noexcept {
    try {
        if (m_dirty) fail();
        validateImpl();
        PackedHostTopology packed;
        packed.revision = m_revision; packed.activeLeafCount = m_activeLeaves;
        packed.volume.hierarchy = {m_root.isNull() ? 0u : m_root.level(),
            m_root.isNull() ? UINT32_MAX : uint32_t(m_root.index()), m_config.levelCount, 0};
        packed.volume.boundsMin = {float(m_bounds.lower.x), float(m_bounds.lower.y), float(m_bounds.lower.z), 0};
        packed.volume.boundsMax = {float(m_bounds.upper.x), float(m_bounds.upper.y), float(m_bounds.upper.z), 0};
        uint64_t nodeBytes = 0, childBytes = 0;
        for (uint32_t l = 0; l < m_config.levelCount; ++l) {
            const uint64_t n = count(0, l) * sizeof(Node), c = count(1, l) * stride(1, l);
            if (nodeBytes > UINT32_MAX || childBytes > UINT32_MAX || n > c_AddressSpaceBytes - nodeBytes ||
                c > c_AddressSpaceBytes - childBytes) fail(nvrhi::FE_UNSUPPORTED);
            LevelInfo info{};
            info.nodes = {uint32_t(nodeBytes), sizeof(Node), uint32_t(count(0, l)), m_config.logDimension[l]};
            info.children = {uint32_t(childBytes), stride(1, l), uint32_t(count(1, l)), width(l)};
            const float childWidth = float(range(int(l) - 1));
            info.childExtent = {childWidth, childWidth, childWidth, 0};
            info.nodeExtent = {int32_t(range(int(l))), int32_t(range(int(l))), int32_t(range(int(l))), 0};
            packed.levels.push_back(info); nodeBytes += n; childBytes += c;
        }
        packed.nodes.resize(size_t(nodeBytes / 4)); packed.children.resize(size_t(childBytes / 4));
        for (uint32_t l = 0; l < m_config.levelCount; ++l) {
            if (!m_nodes[l].empty()) std::memcpy(reinterpret_cast<uint8_t*>(packed.nodes.data()) + packed.levels[l].nodes.x,
                m_nodes[l].data(), m_nodes[l].size() * sizeof(Node));
            if (!m_children[l].empty()) std::memcpy(reinterpret_cast<uint8_t*>(packed.children.data()) + packed.levels[l].children.x,
                m_children[l].data(), m_children[l].size() * sizeof(NodeId));
        }
        output = std::move(packed);
        return nvrhi::FS_OK;
    } GVDB_HOST_CATCH
}
nvrhi::FRESULT HostVolume::computeExtents(uint32_t level, Float4 minimum, Float4 maximum,
                                        CellExtents& output) const noexcept {
    output = {};
    if (level >= m_config.levelCount) return nvrhi::FE_INVALID_ARGS;
    CellExtents result; result.level = level; result.cellWidth = range(int(level) - 1);
    const float lower[] = {minimum.x, minimum.y, minimum.z}, upper[] = {maximum.x, maximum.y, maximum.z};
    int32_t low[3], high[3], dimensions[3]; uint64_t count = 1;
    for (uint32_t axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(lower[axis]) || !std::isfinite(upper[axis]) || lower[axis] > upper[axis]) return nvrhi::FE_INVALID_ARGS;
        const float lo = lower[axis] / float(result.cellWidth), hi = upper[axis] / float(result.cellWidth) - 1.f;
        if (double(lo) < INT32_MIN || double(lo) > INT32_MAX || double(hi) < INT32_MIN || double(hi) > INT32_MAX)
            return nvrhi::FE_UNSUPPORTED;
        low[axis] = int32_t(lo); high[axis] = int32_t(hi);
        const int64_t dimension = int64_t(high[axis]) - low[axis] + 1;
        if (dimension > INT32_MAX) return nvrhi::FE_UNSUPPORTED;
        dimensions[axis] = dimension > 0 ? int32_t(dimension) : 0;
        if (dimensions[axis] && count > UINT64_MAX / uint32_t(dimensions[axis])) return nvrhi::FE_UNSUPPORTED;
        count *= uint32_t(dimensions[axis]);
    }
    result.minimum = {low[0], low[1], low[2]}; result.maximum = {high[0], high[1], high[2]};
    result.resolution = {dimensions[0], dimensions[1], dimensions[2]}; result.count = count;
    output = result; return nvrhi::FS_OK;
}
#undef GVDB_HOST_CATCH
} // namespace gvdb::portable
