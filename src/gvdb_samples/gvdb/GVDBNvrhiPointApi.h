// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "GVDBNvrhiApi.h"
namespace gvdb::portable {
enum class PointAttribute : uint32_t { Position, Velocity, Color };
struct HostPointStreamDesc {
    const void* data = nullptr;
    uint64_t byteSize = 0, byteOffset = 0, byteStride = 0, count = 0;
    PointComponent component = PointComponent::Float32;
    uint32_t components = 3;
};
struct PointAffineTransform {
    Float4 minimum{}, delta{1,1,1,1}, scale{1,1,1,1}, translation{};
};
struct HostPointAttributeDesc { HostPointStreamDesc stream; PointAffineTransform transform; };
struct PointSetDesc {
    uint32_t structSize = sizeof(PointSetDesc), reserved = 0;
    HostPointAttributeDesc positions, velocities;
    HostPointAttributeDesc colors{{nullptr,0,0,0,0,PointComponent::UInt8,4},{}};
};
struct PointPreparationBudget {
    uint64_t maxInputBytes = UINT64_MAX; // Copied source bytes including prefixes/gaps, excluding zero tail padding.
    uint64_t maxActivationCandidates = UINT64_MAX; // Counts center visits plus support cells before deduplication.
    uint64_t maxAllocatedBytes = UINT64_MAX;
    uint32_t maxBuffers = UINT32_MAX, reserved = 0;
};
struct PointSetInfo {
    uint64_t count = 0;
    uint32_t attributeMask = 0, reserved = 0;
    Float4 boundsMin{}, boundsMax{}; // Exact converted XYZ extrema; no legacy padding/clamp.
    uint32_t empty = 1, reserved2[3]{};
};
struct BorrowedPointStream {
    nvrhi::IBuffer* buffer = nullptr;
    uint64_t byteSize = 0, byteOffset = 0, byteStride = 0, count = 0;
    PointComponent component = PointComponent::Float32;
    uint32_t components = 3;
};
enum class PointTopologyMode : uint32_t { FullRebuild, Accumulate };
enum class PointSupportCoverage : uint32_t { Legacy = 0, FloatSupport = 1 };
constexpr uint32_t c_PointTopologyDescV1Size = 32;
struct PointTopologyDesc {
    uint32_t structSize = sizeof(PointTopologyDesc), reserved = 0;
    PointTopologyMode mode = PointTopologyMode::FullRebuild;
    uint32_t minDepth = 0; // Minimum root level: leaf root is 0, not a count of levels.
    float radius = 0;
    uint32_t preserveMetadata = 1;
    uint32_t reserved2 = 0, reserved3 = 0;
    // Appended extension. The original 32-byte descriptor remains accepted and
    // defaults to Legacy without reading this tail. No interface/vtable change.
    PointSupportCoverage coverage = PointSupportCoverage::Legacy;
    uint32_t reserved4 = 0;
};
struct PointOpsCreateDesc {
    uint32_t structSize = sizeof(PointOpsCreateDesc), reserved = 0;
    IGVDBVolume* volume = nullptr;
    const void* conversionArtifact = nullptr;
    uint64_t conversionArtifactBytes = 0;
};
static_assert(sizeof(HostPointStreamDesc)==48 && sizeof(PointAffineTransform)==64 && sizeof(PointSetDesc)==344);
static_assert(sizeof(PointPreparationBudget)==32 && sizeof(PointSetInfo)==64 && sizeof(BorrowedPointStream)==48);
static_assert(sizeof(PointTopologyDesc)==40 && sizeof(PointOpsCreateDesc)==32);
static_assert(offsetof(PointTopologyDesc,coverage)==c_PointTopologyDescV1Size);
static_assert(std::is_standard_layout_v<PointSetDesc> && std::is_trivially_copyable_v<PointSetDesc>);
NVRHI_IID(IGVDBPointSet, "87291603-6087-4c71-b51a-127d1f840d04")
struct IGVDBPointSet : nvrhi::IRHIObject {
    NVRHI_DECLARE_UUID_TRAITS(IGVDBPointSet)
    virtual nvrhi::IDevice* getDevice() const noexcept = 0;
    virtual PointSetInfo getDesc() const noexcept = 0;
    // Read-only canonical buffers. Do not write them: CPU bytes and GPU streams describe
    // the same immutable dataset. Position/velocity retain 3 or 4 source components as
    // Float32 (XYZ used for geometry, W preserved). Colors are four UInt8 components in
    // byte storage units, never implicitly normalized; use transform.scale=255 for [0,1].
    virtual nvrhi::FRESULT getStream(PointAttribute, BorrowedPointStream&) const noexcept = 0;
    virtual nvrhi::FRESULT getData(PointAttribute, BorrowedBytes&) const noexcept = 0;
};
NVRHI_IID(IGVDBPointUploadTicket, "ea135bc8-f455-493a-a882-834b2bf754a3")
struct IGVDBPointUploadTicket : nvrhi::IRHIObject {
    NVRHI_DECLARE_UUID_TRAITS(IGVDBPointUploadTicket)
    virtual nvrhi::IDevice* getDevice() const noexcept = 0;
    // Caller must submit/wait first. Does not submit/wait; works after factory/volume
    // release. Uses actual GPU-converted bytes, then validates finite position XYZ. W/velocity nonfinite values are preserved
    // under converter semantics. One successful finish consumes the ticket; allocation
    // failure can retry.
    virtual nvrhi::FRESULT finishAfterCallerCompletion(IGVDBPointSet** output) noexcept = 0;
};
NVRHI_IID(IGVDBPointOps, "371d2db8-8baa-4489-a2cb-e55436423a1a")
struct IGVDBPointOps : nvrhi::IRHIObject {
    NVRHI_DECLARE_UUID_TRAITS(IGVDBPointOps)
    virtual IGVDBVolume* getVolume() const noexcept = 0; // Borrowed; retained by factory.
    // Deep-copies CPU streams. Optional velocity/color streams have count 0 or the
    // position count. All four source encodings are accepted; transforms use the
    // established binary32 converter semantics, including software Float64 decoding.
    virtual nvrhi::FRESULT preparePoints(const PointSetDesc&, const PointPreparationBudget&,
        IGVDBPointUploadTicket** output) noexcept = 0;
    virtual nvrhi::FRESULT recordPoints(RecordingContext, IGVDBPointUploadTicket*) noexcept = 0;
    // Finished same-device PointSet only. Full rebuild replaces the active leaf set
    // and advances epoch; accumulation preserves existing IDs/epoch and reactivates
    // required inactive leaves. Empty FullRebuild clears; empty Accumulate keeps.
    // Accumulate requires preserveMetadata=1 and PlacementDesc.preserve=1.
    // Both return existing generation tickets for volume->recordAndPublish. No waits.
    virtual nvrhi::FRESULT prepareTopology(IGVDBPointSet*, const PointTopologyDesc&,
        const PlacementDesc&, const PointPreparationBudget&, IGVDBGenerationTicket** output) noexcept = 0;
};
using PointSetHandle = nvrhi::AutoPtr<IGVDBPointSet>;
using PointUploadTicketHandle = nvrhi::AutoPtr<IGVDBPointUploadTicket>;
using PointOpsHandle = nvrhi::AutoPtr<IGVDBPointOps>;
GVDB_NVRHI_C_API nvrhi::FRESULT gvdbCreateNvrhiPointOps(uint32_t apiVersion,
    const PointOpsCreateDesc*, IGVDBPointOps** output) noexcept;
inline nvrhi::FRESULT createPointOps(const PointOpsCreateDesc& desc, IGVDBPointOps** output) noexcept {
    return gvdbCreateNvrhiPointOps(c_GVDBNvrhiApiVersion,&desc,output);
}
} // namespace gvdb::portable
