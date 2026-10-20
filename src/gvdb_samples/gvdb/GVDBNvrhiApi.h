// SPDX-License-Identifier: Apache-2.0
#pragma once
// Queryable NVRHI-owned GVDB API. Legacy gvdb::IGVDBVolume and its IID stay separate.
// The binary boundary exposes interfaces and plain descriptors, never STL ownership.
#include "GVDBNvrhi.h"

#if defined(GVDB_NVRHI_SHARED_LIBRARY_BUILD)
# if defined(_WIN32)
#  define GVDB_NVRHI_API __declspec(dllexport)
# else
#  define GVDB_NVRHI_API __attribute__((visibility("default")))
# endif
#elif defined(GVDB_NVRHI_SHARED_LIBRARY_INCLUDE)
# if defined(_WIN32)
#  define GVDB_NVRHI_API __declspec(dllimport)
# else
#  define GVDB_NVRHI_API __attribute__((visibility("default")))
# endif
#else
# define GVDB_NVRHI_API
#endif
#define GVDB_NVRHI_C_API extern "C" GVDB_NVRHI_API

namespace gvdb::portable {
inline constexpr uint32_t c_GVDBNvrhiApiVersion = 1;
struct VolumeCreateDesc {
    uint32_t structSize = sizeof(VolumeCreateDesc);
    uint32_t reserved = 0;
    nvrhi::IDevice* device = nullptr;
    LevelConfig configuration;
    uint64_t maxHostPoolBytes = c_AddressSpaceBytes;
    const void* remapArtifact = nullptr;
    uint64_t remapArtifactBytes = 0;
};
struct PlacementDesc {
    UInt3 minimumBrickCount{16,16,1};
    uint32_t apron = 1;
    uint32_t preserve = 1; // Exactly zero or one; spatially preserve compatible leaf interiors.
};
struct ChannelUploadDesc {
    uint32_t channel = 0;
    uint32_t reserved = 0;
    const void* data = nullptr;
    uint64_t byteCount = 0; // Full destination logical atlas bytes; copied during preparation.
};
struct GenerationDesc {
    PlacementDesc placement;
    uint32_t reserved = 0;
    const ChannelUploadDesc* uploads = nullptr;
    uint64_t uploadCount = 0;
    uint64_t maxAllocatedBytes = UINT64_MAX;
    uint32_t maxBuffers = UINT32_MAX;
    uint32_t reserved2 = 0;
};
struct HostExtents {
    uint32_t level = 0, cellWidth = 0;
    Int3 minimum{}, maximum{}, resolution{};
    uint64_t count = 0;
};
struct HostBounds {
    Int3 lower{}, upper{}; // Half-open; zeros when empty.
    uint32_t empty = 1;
};
struct BorrowedBufferBinding {
    nvrhi::IBuffer* buffer = nullptr;
    uint64_t byteOffset = 0, byteSize = 0;
};
struct BorrowedBytes {
    const void* data = nullptr;
    uint64_t byteCount = 0;
};
enum class TopologyBuffer : uint32_t { Nodes, Children, Levels, AtlasMap };
struct SnapshotDesc {
    VolumeInfo volume = emptyVolumeInfo();
    TopologyRevision revision;
    uint32_t activeLeafCount = 0, levelCount = 0, channelMask = 0, reserved = 0;
};
struct ChannelSnapshotDesc {
    ChannelDesc desc;
    ChannelStorage storage;
    ChannelRevision revision;
    BorrowedBufferBinding raw;
};
static_assert(std::is_standard_layout_v<VolumeCreateDesc> && std::is_trivially_copyable_v<VolumeCreateDesc>);
static_assert(std::is_standard_layout_v<GenerationDesc> && std::is_trivially_copyable_v<GenerationDesc>);
static_assert(sizeof(LevelConfig) == 84 && sizeof(TopologyRevision) == 24);
static_assert(sizeof(VolumeCreateDesc) == 128 && offsetof(VolumeCreateDesc, device) == 8 &&
              offsetof(VolumeCreateDesc, remapArtifact) == 112);
static_assert(sizeof(PlacementDesc) == 20 && sizeof(ChannelUploadDesc) == 24 && sizeof(GenerationDesc) == 56);
static_assert(sizeof(SnapshotDesc) == 128 && sizeof(ChannelSnapshotDesc) == 88 && sizeof(HostBounds) == 28);
static_assert(sizeof(HostExtents) == 56 && sizeof(BorrowedBufferBinding) == 24 && sizeof(BorrowedBytes) == 16);

NVRHI_IID(IGVDBHostVolume, "bde05d97-01ad-4d53-936c-b44348e59d64")
struct IGVDBHostVolume : nvrhi::IRHIObject {
    NVRHI_DECLARE_UUID_TRAITS(IGVDBHostVolume)
    // Single-threaded edit/commit contract. Configure/clear change the identity epoch;
    // GPU snapshots remain valid until released, and publication is always explicit.
    virtual nvrhi::FRESULT configure(const LevelConfig&) noexcept = 0;
    virtual nvrhi::FRESULT clearTopology() noexcept = 0;
    virtual const LevelConfig& getConfiguration() const noexcept = 0;
    virtual TopologyRevision getTopologyRevision() const noexcept = 0;
    virtual bool isTopologyDirty() const noexcept = 0;
    virtual NodeId getRootId() const noexcept = 0;
    virtual uint32_t getRange(int32_t level) const noexcept = 0;
    virtual uint32_t getWidth(uint32_t level) const noexcept = 0;
    virtual uint64_t getPoolCount(uint32_t group, uint32_t level) const noexcept = 0;
    virtual uint64_t getPoolCapacity(uint32_t group, uint32_t level) const noexcept = 0;
    virtual uint32_t getPoolStride(uint32_t group, uint32_t level) const noexcept = 0;
    virtual HostPoolView getPool(uint32_t group, uint32_t level) const noexcept = 0;
    virtual MutableHostPoolView editPool(uint32_t group, uint32_t level) noexcept = 0;
    virtual const Node* getNode(NodeId) const noexcept = 0;
    // A successful mutable borrow dirties topology, even without a write. Reacquire
    // after finishTopology; flags preserve exact bytes and any nonzero byte is active.
    virtual Node* editNode(NodeId) noexcept = 0;
    virtual const NodeId* getChildList(NodeId) const noexcept = 0;
    virtual NodeId* editChildList(NodeId) noexcept = 0;
    virtual nvrhi::FRESULT reservePool(uint32_t group, uint32_t level, uint64_t elements) noexcept = 0;
    virtual nvrhi::FRESULT allocateNode(uint32_t level, Int3 origin, bool active, NodeId& output) noexcept = 0;
    virtual nvrhi::FRESULT allocateChildList(uint32_t level, NodeId& output) noexcept = 0;
    virtual nvrhi::FRESULT setRoot(NodeId) noexcept = 0;
    // Assigns exactly 0 or 1; an identical byte is a no-op. Does not publish.
    virtual nvrhi::FRESULT setActive(NodeId, bool active) noexcept = 0;
    virtual nvrhi::FRESULT activate(double x, double y, double z, NodeId& output) noexcept = 0;
    virtual nvrhi::FRESULT activateAtLevel(uint32_t level, Int3 position, NodeId& output) noexcept = 0;
    virtual NodeId findLeaf(Int3 position) const noexcept = 0;
    virtual bool isAllocated(Int3 position) const noexcept = 0;
    virtual bool isActive(Int3 position) const noexcept = 0;
    virtual nvrhi::FRESULT computeExtents(uint32_t level, Float4 minimum, Float4 maximum, HostExtents& output) const noexcept = 0;
    virtual nvrhi::FRESULT activateRegion(const HostExtents&, uint64_t& selected) noexcept = 0;
    virtual nvrhi::FRESULT activateRegionFromMasks(const HostExtents&, const uint8_t*, uint64_t count, uint64_t& selected) noexcept = 0;
    virtual nvrhi::FRESULT activateRegionFromValues(const HostExtents&, const float*, uint64_t count, float threshold, uint64_t& selected) noexcept = 0;
    virtual nvrhi::FRESULT activateHalo(const HostExtents&, uint64_t& selected) noexcept = 0;
    virtual nvrhi::FRESULT validateTopology() const noexcept = 0;
    virtual nvrhi::FRESULT finishTopology() noexcept = 0;
    virtual uint32_t getActiveLeafCount() const noexcept = 0;
    virtual HostBounds getBounds() const noexcept = 0;
};
using HostVolumeHandle = nvrhi::AutoPtr<IGVDBHostVolume>;

NVRHI_IID(IGVDBSnapshot, "41cacafc-8d1a-49ee-ae90-d55b730860cc")
struct IGVDBSnapshot : nvrhi::IRHIObject {
    NVRHI_DECLARE_UUID_TRAITS(IGVDBSnapshot)
    virtual nvrhi::IDevice* getDevice() const noexcept = 0; // Borrowed; retained by the snapshot.
    virtual SnapshotDesc getDesc() const noexcept = 0;
    virtual nvrhi::FRESULT getLevel(uint32_t level, LevelInfo& output) const noexcept = 0;
    virtual nvrhi::FRESULT getTopologyBuffer(TopologyBuffer, BorrowedBufferBinding& output) const noexcept = 0;
    virtual nvrhi::FRESULT getChannel(uint32_t channel, ChannelSnapshotDesc& output) const noexcept = 0;
    // Returned resources are borrowed for this snapshot's lifetime. AddRef any
    // resource kept independently. Publication cannot mutate these bindings.
};
using SnapshotHandle = nvrhi::AutoPtr<IGVDBSnapshot>;

NVRHI_IID(IGVDBGenerationTicket, "97dc7ded-0459-4d0b-bdae-79683ac33997")
struct IGVDBGenerationTicket : nvrhi::IRHIObject {
    NVRHI_DECLARE_UUID_TRAITS(IGVDBGenerationTicket)
    virtual nvrhi::IDevice* getDevice() const noexcept = 0; // Opaque one-use preparation.
};
using GenerationTicketHandle = nvrhi::AutoPtr<IGVDBGenerationTicket>;

NVRHI_IID(IGVDBReadback, "11d4936f-6a4e-453b-b303-69bfe81c5ba2")
struct IGVDBReadback : nvrhi::IRHIObject {
    NVRHI_DECLARE_UUID_TRAITS(IGVDBReadback)
    virtual nvrhi::IDevice* getDevice() const noexcept = 0;
    virtual uint32_t getChannelMask() const noexcept = 0;
    // Caller must submit and wait using NVRHI first. Does not submit, wait, or infer
    // completion from revisions. Remains usable after the volume owner is released.
    virtual nvrhi::FRESULT finishAfterCallerCompletion() noexcept = 0;
    virtual nvrhi::FRESULT getChannelData(uint32_t channel, BorrowedBytes& output) const noexcept = 0;
    // Logical bytes remain owned by this readback object; no cross-DLL vector frees.
};
using ReadbackHandle = nvrhi::AutoPtr<IGVDBReadback>;

NVRHI_IID(IGVDBVolume, "8dc94fb6-63d9-4641-95c8-a8b1ee117cac")
struct IGVDBVolume : IGVDBHostVolume {
    NVRHI_DECLARE_UUID_TRAITS(IGVDBVolume)
    virtual nvrhi::IDevice* getDevice() const noexcept = 0; // Borrowed; owner retains the device.
    virtual nvrhi::FRESULT setChannel(uint32_t channel, const ChannelDesc&) noexcept = 0;
    virtual nvrhi::FRESULT removeChannel(uint32_t channel) noexcept = 0;
    virtual nvrhi::FRESULT describePlacement(const PlacementDesc&, VolumeInfo& output) const noexcept = 0;
    // New owned reference on success; clears output first, including on failure.
    // Existing independently retained tickets/snapshots and the published
    // generation remain unchanged on failed preparation.
    virtual nvrhi::FRESULT prepareGeneration(const GenerationDesc&, IGVDBGenerationTicket** output) noexcept = 0;
    virtual nvrhi::FRESULT recordAndPublish(RecordingContext, IGVDBGenerationTicket*) noexcept = 0;
    // New owned reference on success; clears output on failure/no publication.
    virtual nvrhi::FRESULT getSnapshot(IGVDBSnapshot** output) const noexcept = 0;
    virtual nvrhi::FRESULT prepareReadback(IGVDBSnapshot*, uint32_t channelMask, IGVDBReadback** output) noexcept = 0;
    virtual nvrhi::FRESULT recordReadback(RecordingContext, IGVDBReadback*) noexcept = 0;
    // Lists must be open and same-device; generation needs graphics/compute.
    // Recording replaces compute state; rebind before subsequent dispatches.
    // Validation/allocation failure is atomic. Recording/device failure cannot
    // roll back commands already recorded. Publication means recording, not GPU
    // completion. Submit generations in order and synchronize cross-queue use.
    // No method opens/closes/submits/waits on a caller list or adopts gp objects.
};
using VolumeHandle = nvrhi::AutoPtr<IGVDBVolume>;

GVDB_NVRHI_C_API uint32_t gvdbGetNvrhiApiVersion() noexcept;
// Clears output first. Validates version/descriptor size, copies artifact bytes,
// retains device, and creates the implemented CPU/GPU owner. No compiler required.
GVDB_NVRHI_C_API nvrhi::FRESULT gvdbCreateNvrhiVolume(uint32_t apiVersion,
    const VolumeCreateDesc* desc, IGVDBVolume** output) noexcept;
inline nvrhi::FRESULT createVolume(const VolumeCreateDesc& desc, IGVDBVolume** output) noexcept {
    return gvdbCreateNvrhiVolume(c_GVDBNvrhiApiVersion, &desc, output);
}
} // namespace gvdb::portable
