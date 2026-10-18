// SPDX-License-Identifier: Apache-2.0
#pragma once
// M5 Step A DRAFT: additive ownership vocabulary, not a replacement for GVDB.h.
// There are no exported factories, device/queue wrappers, or operator implementations
// in this header. Include it without CUDA, OptiX, graphics SDKs, Donut, or ComputeRuntime.
#include <nvrhi/nvrhi.h>
#include "slang/GVDBPortable.h"
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace gvdb::portable {
// Draft revision, not a binary compatibility promise or NVRHI header version.
inline constexpr uint32_t c_DraftContractRevision = 1;
inline constexpr uint32_t c_MaxLevels = slang::kMaxLevels;
inline constexpr uint32_t c_MaxChannels = 32;
inline constexpr uint64_t c_AddressSpaceBytes = uint64_t(UINT32_MAX) + 1;
inline constexpr uint32_t c_InvalidIndex = UINT32_MAX;
using AtlasFormat = slang::AtlasFormat;
using Float4 = slang::Float4;
using LevelInfo = slang::LevelInfo;
using VolumeInfo = slang::VolumeInfo;
struct UInt3 { uint32_t x = 0, y = 0, z = 0; };
struct Int3 { int32_t x = 0, y = 0, z = 0; };

// Explicit encoding; never serialize native C++ bitfields. The null reference
// is all ones. Bounds against actual pools are checked by the host topology core.
struct NodeId {
    uint64_t value = UINT64_MAX;
    constexpr bool isNull() const noexcept { return value == UINT64_MAX; }
    constexpr uint8_t group() const noexcept { return uint8_t(value); }
    constexpr uint8_t level() const noexcept { return uint8_t(value >> 8); }
    constexpr uint64_t index() const noexcept { return value >> 16; }
    constexpr bool operator==(NodeId other) const noexcept { return value == other.value; }
    constexpr bool operator!=(NodeId other) const noexcept { return !(*this == other); }
    // Clears output on failure, including attempts to truncate a 48-bit index.
    static constexpr bool encode(uint32_t group, uint32_t level, uint64_t index, NodeId& output) noexcept {
        output = {};
        if (group > 1 || level >= c_MaxLevels || index > (UINT64_MAX >> 16))
            return false;
        output.value = group | (uint64_t(level) << 8) | (index << 16);
        return true;
    }
};
struct alignas(16) Node {
    uint8_t level = 0, flags = 0, priority = 0, reserved = 0;
    Int3 position;
    Int3 atlasInterior;
    float range[3]{};
    NodeId parent;
    NodeId children;
    uint64_t mask = 0;
};
static_assert(sizeof(NodeId) == 8 && sizeof(Node) == 64 && alignof(Node) == 16);
static_assert(offsetof(Node, parent) == 40 && offsetof(Node, children) == 48 && offsetof(Node, mask) == 56);
static_assert(std::is_trivially_copyable_v<Node>);

struct LevelConfig {
    uint32_t levelCount = 0;
    std::array<uint32_t, c_MaxLevels> logDimension{};
    std::array<uint32_t, c_MaxLevels> initialNodeCapacity{};
    // Legacy root-to-leaf argument order: r0 configures level zero. Nonpositive
    // values retain legacy clamping to one; unsupported positive values and
    // extent overflow fail and clear output. Reservations retain 4,4,2,1,1.
    static constexpr bool fromBranching(int r4, int r3, int r2, int r1, int r0,
                                       LevelConfig& output) noexcept;
};
// A CPU pool view borrows bytes. Allocation/growth invalidates it; reacquire it
// before further edits. A mutable borrow ends at the next topology commit; do not
// retain it and mutate a finished generation. Reacquire through an edit method.
// Only an explicit topology commit publishes manual edits.
struct HostPoolView {
    const void* data = nullptr;
    uint64_t count = 0;
    uint32_t stride = 0;
};
struct MutableHostPoolView {
    void* data = nullptr;
    uint64_t count = 0;
    uint32_t stride = 0;
};

// Metadata revisions belong to one volume, not to a process-wide clock.
// Epoch changes invalidate NodeIds (clear/reconfigure/full rebuild); topology
// changes invalidate traversal/bounds; placement changes invalidate atlas maps.
// Zero denotes unpublished metadata. Overflow must fail rather than wrap to zero.
struct TopologyRevision {
    uint64_t epoch = 0, topology = 0, placement = 0;
};
constexpr bool nextRevision(uint64_t current, uint64_t& next) noexcept {
    next = current == UINT64_MAX ? 0 : current + 1;
    return next != 0;
}
struct ChannelRevision {
    uint64_t resource = 0; // Increment when allocation or binding range changes.
    uint64_t content = 0;  // Increment when a write is recorded, not when it completes.
};
enum class TopologyAuthority : uint32_t {
    Synchronized,
    HostModified,   // Record an upload after CPU validation.
    DeviceModified // Explicit completion/readback/CPU validation before host edits.
};

struct ChannelDesc {
    AtlasFormat format = AtlasFormat::R32_FLOAT;
    // Storage units: integer channels use whole bytes [0,255], float channels finite floats.
    // This is the outside-volume/apron value, not the normalized color-fill API.
    Float4 voidValue{};
};
struct ChannelStorage {
    uint32_t voxelCount = 0;
    uint32_t voxelStride = 0;
    uint64_t logicalBytes = 0;   // VBX and host data exclude physical padding.
    uint64_t allocatedBytes = 0; // Rounded to four bytes for raw buffer operations.
};

enum class ValidationField : uint32_t {
    None, LevelCount, Branching, Extent, Format, Dimensions, AddressRange,
    Value, Buffer, ViewKind, Stride, Components
};
struct ValidationResult {
    nvrhi::FRESULT result = nvrhi::FS_OK;
    ValidationField field = ValidationField::None;
    uint32_t index = c_InvalidIndex;
    constexpr explicit operator bool() const noexcept { return NVRHI_SUCCEEDED(result); }
};
constexpr ValidationResult invalid(ValidationField field, uint32_t index = c_InvalidIndex) noexcept {
    return {nvrhi::FE_INVALID_ARGS, field, index};
}
constexpr ValidationResult unsupported(ValidationField field) noexcept {
    return {nvrhi::FE_UNSUPPORTED, field, c_InvalidIndex};
}

// These validators only inspect their documented descriptor/range. They neither
// submit GPU work nor validate node graph contents, device identity or API limits.
constexpr ValidationResult validateLevelConfig(const LevelConfig& config) noexcept {
    if (!config.levelCount || config.levelCount > c_MaxLevels)
        return invalid(ValidationField::LevelCount);
    uint64_t extent = 1;
    for (uint32_t level = 0; level < config.levelCount; ++level) {
        const uint32_t logarithm = config.logDimension[level];
        if (!logarithm || logarithm > 10)
            return unsupported(ValidationField::Branching);
        const uint32_t width = 1u << logarithm;
        if (extent > uint64_t(INT32_MAX) / width)
            return unsupported(ValidationField::Extent);
        extent *= width;
    }
    return {};
}
constexpr bool LevelConfig::fromBranching(int r4, int r3, int r2, int r1, int r0,
                                          LevelConfig& output) noexcept {
    output = {};
    LevelConfig candidate;
    candidate.levelCount = 5;
    const int leafToRoot[5] = {r0, r1, r2, r3, r4};
    const uint32_t capacities[5] = {4, 4, 2, 1, 1};
    for (uint32_t level = 0; level < 5; ++level) {
        candidate.logDimension[level] = leafToRoot[level] <= 0 ? 1u : uint32_t(leafToRoot[level]);
        candidate.initialNodeCapacity[level] = capacities[level];
    }
    if (!validateLevelConfig(candidate))
        return false;
    output = candidate;
    return true;
}
constexpr ValidationResult describeChannel(AtlasFormat format, UInt3 resolution, ChannelStorage& output) noexcept {
    output = {};
    const uint32_t stride = slang::atlasStride(format);
    if (!stride)
        return unsupported(ValidationField::Format);
    // All-zero dimensions denote no live atlas; partial-zero shapes are invalid.
    if (!(resolution.x || resolution.y || resolution.z)) {
        output.voxelStride = stride;
        return {};
    }
    if (!resolution.x || !resolution.y || !resolution.z)
        return invalid(ValidationField::Dimensions);
    const uint64_t plane = uint64_t(resolution.x) * resolution.y;
    if (plane > UINT32_MAX || resolution.z > UINT32_MAX / plane)
        return unsupported(ValidationField::AddressRange);
    const uint64_t voxels = plane * resolution.z;
    if (voxels > c_AddressSpaceBytes / stride)
        return unsupported(ValidationField::AddressRange);
    const uint64_t logical = voxels * stride;
    const uint64_t physical = (logical + 3) & ~uint64_t(3);
    if (physical > c_AddressSpaceBytes)
        return unsupported(ValidationField::AddressRange);
    output = {uint32_t(voxels), stride, logical, physical};
    return {};
}
inline ValidationResult validateChannelDesc(const ChannelDesc& channel) noexcept {
    const uint32_t stride = slang::atlasStride(channel.format);
    if (!stride)
        return unsupported(ValidationField::Format);
    const float values[] = {channel.voidValue.x, channel.voidValue.y, channel.voidValue.z, channel.voidValue.w};
    for (uint32_t component = 0; component < 4; ++component)
        if (!std::isfinite(values[component]))
            return invalid(ValidationField::Value, component);
    const uint32_t integerComponents = channel.format == AtlasFormat::R8_UINT ? 1 :
        (channel.format == AtlasFormat::RGBA8_UINT ? 4 : 0);
    for (uint32_t component = 0; component < integerComponents; ++component)
        if (values[component] < 0 || values[component] > 255 || std::floor(values[component]) != values[component])
            return invalid(ValidationField::Value, component);
    return {};
}

// Explicit byte length: zero means empty, never NVRHI's "remaining buffer".
struct BufferView {
    nvrhi::BufferHandle buffer;
    uint64_t byteOffset = 0;
    uint64_t byteSize = 0;
};
constexpr bool containsBytes(uint64_t capacity, uint64_t offset, uint64_t bytes) noexcept {
    return offset <= capacity && bytes <= capacity - offset;
}
constexpr bool containsElements(uint64_t capacity, uint64_t offset, uint64_t stride,
                                uint64_t count, uint64_t elementBytes) noexcept {
    if (offset > capacity)
        return false;
    if (!count)
        return true;
    if (!elementBytes || stride < elementBytes || elementBytes > capacity - offset)
        return false;
    // Division avoids overflowing offset + (count - 1) * stride + elementBytes.
    return count - 1 <= (capacity - offset - elementBytes) / stride;
}
inline ValidationResult validateRawView(const BufferView& view, uint64_t minimumBytes, bool writable) noexcept {
    if (!view.buffer) {
        if (minimumBytes || view.byteOffset || view.byteSize)
            return invalid(ValidationField::Buffer);
        return {};
    }
    const auto& desc = view.buffer->getDesc();
    if (!containsBytes(desc.byteSize, view.byteOffset, view.byteSize) || view.byteSize < minimumBytes ||
        view.byteSize > c_AddressSpaceBytes || view.byteOffset % 4 || view.byteSize % 4)
        return invalid(ValidationField::AddressRange);
    if (!desc.canHaveRawViews || desc.structStride != 0 || (writable && !desc.canHaveUAVs) || desc.isVolatile)
        return invalid(ValidationField::ViewKind);
    return {};
}

enum class PointComponent : uint32_t { UInt8, UInt16, Float32, Float64 };
constexpr uint32_t componentBytes(PointComponent component) noexcept {
    switch (component) {
    case PointComponent::UInt8: return 1;
    case PointComponent::UInt16: return 2;
    case PointComponent::Float32: return 4;
    case PointComponent::Float64: return 8;
    default: return 0;
    }
}
struct PointStream {
    nvrhi::BufferHandle buffer; // Retained canonical raw allocation; structStride must be zero.
    uint64_t byteOffset = 0, byteStride = 0, count = 0;
    PointComponent component = PointComponent::Float32;
    uint32_t components = 3;
};
inline ValidationResult validatePointStream(const PointStream& stream) noexcept {
    const uint32_t width = componentBytes(stream.component);
    if (!width)
        return unsupported(ValidationField::Format);
    if (!stream.components || stream.components > 4)
        return invalid(ValidationField::Components);
    if (stream.count > UINT32_MAX || stream.byteOffset > UINT32_MAX || stream.byteStride > UINT32_MAX)
        return unsupported(ValidationField::AddressRange);
    if (!stream.buffer)
        return !stream.count && !stream.byteOffset ? ValidationResult{} : invalid(ValidationField::Buffer);
    const auto& desc = stream.buffer->getDesc();
    if (!containsElements(desc.byteSize, stream.byteOffset, stream.byteStride, stream.count,
                          uint64_t(width) * stream.components))
        return invalid(ValidationField::AddressRange);
    if (!desc.canHaveRawViews || desc.structStride != 0 || desc.isVolatile)
        return invalid(ValidationField::ViewKind);
    // A 32-bit shader byte offset must address the final complete element too.
    if (!containsElements(c_AddressSpaceBytes, stream.byteOffset, stream.byteStride, stream.count,
                          uint64_t(width) * stream.components))
        return unsupported(ValidationField::AddressRange);
    return {};
}

struct ChannelBinding {
    ChannelDesc desc;
    ChannelStorage storage;
    // Canonical raw allocation: structStride == 0, canHaveRawViews == true.
    // Existing structured-float shader consumers need a raw-load adapter; do
    // not request simultaneous raw/structured flags (not portable to D3D11).
    BufferView raw;
    ChannelRevision revision;
};
constexpr VolumeInfo emptyVolumeInfo() noexcept {
    VolumeInfo info{};
    info.hierarchy.y = c_InvalidIndex;
    return info;
}
struct SnapshotBindings {
    // Device is declared first, therefore released after every buffer member.
    // The publishing volume validates that all resources belong to this device.
    nvrhi::DeviceHandle device;
    VolumeInfo volume = emptyVolumeInfo();
    std::array<LevelInfo, c_MaxLevels> levels{};
    TopologyRevision revision;
    uint32_t activeLeafCount = 0;
    BufferView nodes, children; // Raw serialized nodes and explicit 64-bit references.
    BufferView levelMetadata;  // Structured LevelInfo records (64 bytes each).
    BufferView atlasMap;       // Structured slang::AtlasNode records (16 bytes each).
    uint32_t channelMask = 0;  // Channel i is present iff bit i is set; at most 32.
    std::array<ChannelBinding, c_MaxChannels> channels{};
};
// An immutable *binding description*, not a frozen copy of buffer contents.
// Construction copies/retains handles and metadata; it does not certify them.
// Only snapshots published by an owner carry its validation guarantee. Caller-
// constructed descriptions must never be accepted as authenticated owner tickets.
// Step B/C's publishing owner must validate the complete graph and descriptors.
// Existing snapshots keep old allocations after repacking or ping-pong publication.
// Writes to a retained allocation remain visible according to NVRHI queue ordering.
class BindingSnapshot {
    const SnapshotBindings m_bindings;
 public:
    explicit BindingSnapshot(SnapshotBindings bindings) noexcept : m_bindings(std::move(bindings)) {}
    BindingSnapshot(const BindingSnapshot&) noexcept = default;
    BindingSnapshot& operator=(const BindingSnapshot&) = delete;
    [[nodiscard]] const SnapshotBindings& bindings() const noexcept { return m_bindings; }
};

// Borrowed for one recording call, not a new command-list abstraction. A future
// operator must require an open list on the same device, retain its bindings in
// recorded work, and never open/close/execute the caller's list. This descriptor
// intentionally performs no GPU calls and cannot prove that a list is open.
struct RecordingContext {
    nvrhi::IDevice* device = nullptr;
    nvrhi::ICommandList* commands = nullptr;
};
// CPU-dependent topology work is explicitly multi-phase: record compact output;
// caller closes/submits/waits using NVRHI; obtain and validate CPU readback; edit
// host topology; record upload/repack; publish a complete new binding generation.
// A recorded content revision is not a fence or proof that readback is ready.
// Empty configured volumes have null root (hierarchy.y == UINT32_MAX), zero
// active leaves, zero bounds and no live atlas. Retained capacity is private to
// the owner. Existing Slang kernels still need Step B/C empty-volume dispatch
// guards before this representation can be submitted.
} // namespace gvdb::portable
