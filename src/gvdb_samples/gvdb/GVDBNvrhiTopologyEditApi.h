// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "GVDBNvrhiApi.h"
namespace gvdb::portable {
struct TopologyEditCreateDesc {
    uint32_t structSize = sizeof(TopologyEditCreateDesc), reserved = 0;
    IGVDBVolume* volume = nullptr;
};
struct TopologyEditDesc {
    uint32_t structSize = sizeof(TopologyEditDesc), reserved = 0;
    PlacementDesc placement;
    uint32_t preserveMetadata = 1;
    uint64_t maxReadbackBytes = UINT64_MAX;   // Logical node + child payload bytes.
    uint64_t maxAllocatedBytes = UINT64_MAX;  // Private GPU copies + staging buffers.
    uint64_t maxPublicationBytes = UINT64_MAX;
    uint32_t maxBuffers = UINT32_MAX, maxPublicationBuffers = UINT32_MAX;
};
static_assert(sizeof(TopologyEditCreateDesc) == 16 && sizeof(TopologyEditDesc) == 64);
static_assert(std::is_standard_layout_v<TopologyEditDesc> && std::is_trivially_copyable_v<TopologyEditDesc>);
NVRHI_IID(IGVDBTopologyEditTicket, "11f1bdda-c383-43c4-85d0-d4aca9f23b62")
struct IGVDBTopologyEditTicket : nvrhi::IRHIObject {
    NVRHI_DECLARE_UUID_TRAITS(IGVDBTopologyEditTicket)
    virtual nvrhi::IDevice* getDevice() const noexcept = 0;
    virtual SnapshotDesc getSourceDesc() const noexcept = 0;
    virtual nvrhi::FRESULT getLevel(uint32_t, LevelInfo&) const noexcept = 0;
    // After recordBegin and before recordFetch: private writable Nodes/Children;
    // Levels is retained read-only source metadata. AtlasMap is unsupported.
    // Counts, strides, root ID and level configuration stay fixed. Never write
    // published snapshot buffers or edit these private buffers after recordFetch.
    virtual nvrhi::FRESULT getTopologyBuffer(TopologyBuffer, BorrowedBufferBinding&) const noexcept = 0;
    // Explicit caller submission/completion required. Validates actual GPU bytes
    // and prepares an ordinary one-use publication ticket. Does not publish/wait.
    // Failure preserves owner/current snapshot and clears output. A successful
    // finish consumes this edit ticket; allocation failures may be retried.
    virtual nvrhi::FRESULT finishAfterCallerCompletion(IGVDBGenerationTicket** output) noexcept = 0;
};
NVRHI_IID(IGVDBTopologyEdit, "66a6f6ec-b481-497e-809c-818c285cd51b")
struct IGVDBTopologyEdit : nvrhi::IRHIObject {
    NVRHI_DECLARE_UUID_TRAITS(IGVDBTopologyEdit)
    virtual IGVDBVolume* getVolume() const noexcept = 0;  // Borrowed, retained by factory.
    // Requires finished host topology matching the current publication. Captures
    // host/config/publication revisions; later changes make the edit stale.
    virtual nvrhi::FRESULT prepare(const TopologyEditDesc&, IGVDBTopologyEditTicket** output) noexcept = 0;
    // Open same-device graphics/compute list. Copies current topology into private
    // buffers. Caller may then record its Slang edits, with normal UAV ordering.
    virtual nvrhi::FRESULT recordBegin(RecordingContext, IGVDBTopologyEditTicket*) noexcept = 0;
    // Record after all edits. Copies only validated declared ranges into staging;
    // never infers allocation counts/root metadata from device-written bytes.
    virtual nvrhi::FRESULT recordFetch(RecordingContext, IGVDBTopologyEditTicket*) noexcept = 0;
};
using TopologyEditHandle = nvrhi::AutoPtr<IGVDBTopologyEdit>;
using TopologyEditTicketHandle = nvrhi::AutoPtr<IGVDBTopologyEditTicket>;
// Offline GVDBTopology artifacts, in this order: applyLeafMarkers,
// delinkLeafBricks, delinkInternalBricks, initializeChildLists, linkBricks.
struct TopologyActivityArtifact {
    const void* data = nullptr;
    uint64_t byteSize = 0;
};
struct TopologyActivityCreateDesc {
    uint32_t structSize = sizeof(TopologyActivityCreateDesc), reserved = 0;
    nvrhi::IDevice* device = nullptr;
    std::array<TopologyActivityArtifact, 5> artifacts;
};
struct TopologyActivityDesc {
    uint32_t structSize = sizeof(TopologyActivityDesc), reserved = 0;
    // Same-device structured uint buffer, one marker per allocated leaf.
    // Zero deactivates; every nonzero value activates. Borrowed during record.
    BorrowedBufferBinding markers;
    // One constant buffer: 256 bytes per nonempty dispatch. For root level R>0
    // with nonempty connected pools, 2 + 3*R dispatches; R=0 needs one.
    // Add one 8-byte dummy buffer if the single-leaf root has no child pool.
    uint64_t maxAllocatedBytes = UINT64_MAX;
    uint32_t maxBuffers = UINT32_MAX, reserved2 = 0;
};
static_assert(sizeof(TopologyActivityCreateDesc) == 96 && sizeof(TopologyActivityDesc) == 48);
static_assert(std::is_standard_layout_v<TopologyActivityDesc> &&
              std::is_trivially_copyable_v<TopologyActivityDesc>);
NVRHI_IID(IGVDBTopologyActivity, "7c18702a-313f-43d8-9ada-d411077da767")
struct IGVDBTopologyActivity : nvrhi::IRHIObject {
    NVRHI_DECLARE_UUID_TRAITS(IGVDBTopologyActivity)
    // Between recordBegin and recordFetch, on the same device. Derives internal
    // activity bottom-up by delinking, then restores ALL allocated links top-down.
    // Inactive nodes remain owned and connected, ready for later reactivation.
    // Counts/coordinates/IDs must still match the source; this does not allocate
    // nodes or compact pools. Caller retains normal submission/completion control.
    virtual nvrhi::FRESULT record(RecordingContext, IGVDBTopologyEditTicket*,
                                  const TopologyActivityDesc&) noexcept = 0;
};
using TopologyActivityHandle = nvrhi::AutoPtr<IGVDBTopologyActivity>;
GVDB_NVRHI_C_API nvrhi::FRESULT gvdbCreateNvrhiTopologyActivity(uint32_t apiVersion,
                                                                const TopologyActivityCreateDesc*,
                                                                IGVDBTopologyActivity** output) noexcept;
inline nvrhi::FRESULT createTopologyActivity(const TopologyActivityCreateDesc& desc,
                                             IGVDBTopologyActivity** output) noexcept {
    return gvdbCreateNvrhiTopologyActivity(c_GVDBNvrhiApiVersion, &desc, output);
}
GVDB_NVRHI_C_API nvrhi::FRESULT gvdbCreateNvrhiTopologyEdit(uint32_t apiVersion, const TopologyEditCreateDesc*,
                                                            IGVDBTopologyEdit** output) noexcept;
inline nvrhi::FRESULT createTopologyEdit(const TopologyEditCreateDesc& desc,
                                         IGVDBTopologyEdit** output) noexcept {
    return gvdbCreateNvrhiTopologyEdit(c_GVDBNvrhiApiVersion, &desc, output);
}
}  // namespace gvdb::portable
