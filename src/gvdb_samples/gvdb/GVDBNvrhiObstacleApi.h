// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "GVDBNvrhiApi.h"
namespace gvdb::portable {
struct ObstacleCreateDesc {
    uint32_t structSize = sizeof(ObstacleCreateDesc), reserved = 0;
    nvrhi::IDevice* device = nullptr;
    const void* artifact = nullptr;
    uint64_t artifactBytes = 0;
};
struct ObstacleRequestDesc {
    uint32_t structSize = sizeof(ObstacleRequestDesc), subcellWidth = 1;
    IGVDBSnapshot* source = nullptr;
    IGVDBSnapshot* obstacle = nullptr;
    // Null means identity source leaf-pool order; both count fields must be zero.
    // Otherwise exactly source leaf-pool count entries, copied during prepare.
    // UINT32_MAX excludes a source slot; other entries must be unique and in range.
    const uint32_t* sourceToOutput = nullptr;
    uint32_t mappingCount = 0, outputBrickCount = 0;
    uint32_t reserved[2]{};
};
struct ObstaclePreparationBudget {
    uint64_t maxHostBytes = UINT64_MAX;       // Inverse slot map + completed CPU result.
    uint64_t maxAllocatedBytes = UINT64_MAX;  // NVRHI descriptor bytes, including readback/dummies.
    uint32_t maxBuffers = UINT32_MAX, reserved = 0;
};
static_assert(sizeof(ObstacleCreateDesc) == 32 && sizeof(ObstacleRequestDesc) == 48 &&
              sizeof(ObstaclePreparationBudget) == 24);
static_assert(offsetof(ObstacleRequestDesc, sourceToOutput) == 24 &&
              offsetof(ObstacleRequestDesc, reserved) == 40);
static_assert(std::is_standard_layout_v<ObstacleRequestDesc> &&
              std::is_trivially_copyable_v<ObstacleRequestDesc>);
NVRHI_IID(IGVDBObstacleTicket, "e830f8da-a0c5-4e5e-9e9c-89e9398a1fde")
struct IGVDBObstacleTicket : nvrhi::IRHIObject {
    NVRHI_DECLARE_UUID_TRAITS(IGVDBObstacleTicket)
    virtual nvrhi::IDevice* getDevice() const noexcept = 0;
    virtual uint32_t getOutputBrickCount() const noexcept = 0;
    virtual uint32_t getSubcellsPerBrick() const noexcept = 0;
    // After record, borrowed structured uint32 buffer; caller synchronizes before use.
    // Empty output is {nullptr,0,0}. Failure preserves output.
    virtual nvrhi::FRESULT getOutputBuffer(BorrowedBufferBinding& output) const noexcept = 0;
    // One successful finish, AFTER caller submission and wait. No readiness inference.
    // Borrowed immutable uint32 bytes, valid until ticket release. Empty is {nullptr,0}.
    // Failure preserves output and allows retry. Works after operator/owner release.
    virtual nvrhi::FRESULT finishAfterCallerCompletion(BorrowedBytes& output) noexcept = 0;
    virtual nvrhi::FRESULT getValues(BorrowedBytes& output) const noexcept = 0;
};
NVRHI_IID(IGVDBObstacle, "6db0d4b2-2ad2-4c16-9fa0-812ff179f20e")
struct IGVDBObstacle : nvrhi::IRHIObject {
    NVRHI_DECLARE_UUID_TRAITS(IGVDBObstacle)
    virtual nvrhi::IDevice* getDevice() const noexcept = 0;
    // Authenticated same-device snapshots, possibly from different/released owners.
    // Each source leaf ORIGIN maps to one active obstacle leaf by half-open bounds.
    // Its index repeats in every subcell. Missing/inactive/unmapped = UINT32_MAX.
    // Width must be a power of two dividing the source leaf width. No channel needed.
    // Clears output on failure. No recording, submission, wait, or publication.
    virtual nvrhi::FRESULT prepare(const ObstacleRequestDesc&, const ObstaclePreparationBudget&,
                                   IGVDBObstacleTicket** output) noexcept = 0;
    // Single record into an open same-device graphics/compute list. This operator's
    // ticket only; replaces compute state. Caller owns submission and ordering.
    virtual nvrhi::FRESULT record(RecordingContext, IGVDBObstacleTicket*) noexcept = 0;
};
using ObstacleHandle = nvrhi::AutoPtr<IGVDBObstacle>;
using ObstacleTicketHandle = nvrhi::AutoPtr<IGVDBObstacleTicket>;
GVDB_NVRHI_C_API nvrhi::FRESULT gvdbCreateNvrhiObstacle(uint32_t apiVersion, const ObstacleCreateDesc*,
                                                        IGVDBObstacle** output) noexcept;
inline nvrhi::FRESULT createObstacle(const ObstacleCreateDesc& desc, IGVDBObstacle** output) noexcept {
    return gvdbCreateNvrhiObstacle(c_GVDBNvrhiApiVersion, &desc, output);
}
}  // namespace gvdb::portable
