// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "GVDBNvrhiPointApi.h"
namespace gvdb::portable {
struct SubcellCreateDesc {
    uint32_t structSize = sizeof(SubcellCreateDesc), reserved = 0;
    nvrhi::IDevice* device = nullptr;
    const void* artifact = nullptr;
    uint64_t artifactBytes = 0;
};
struct SubcellRequestDesc {
    uint32_t structSize = sizeof(SubcellRequestDesc), subcellWidth = 1;
    IGVDBSnapshot* snapshot = nullptr;
    IGVDBPointSet* points = nullptr;
    float radius = 0;
    uint32_t reserved = 0;
    Float4 translation{};
};
struct SubcellPreparationBudget {
    uint64_t maxCells = UINT64_MAX, maxCandidateVisits = UINT64_MAX, maxPointPairs = UINT64_MAX;
    uint64_t maxHostBytes = UINT64_MAX, maxAllocatedBytes = UINT64_MAX;
    uint32_t maxBuffers = UINT32_MAX, reserved = 0;
};
struct SubcellInfo {
    uint32_t leafPoolCount = 0, subcellsPerLeaf = 0, cellCount = 0, attributeMask = 0;
    uint64_t pointPairs = 0;
};
// Active source leaf pool index and signed cell origin. Inactive slots are zero
// origin / UINT32_MAX, independent of whether an active cell contains any points.
struct SubcellLocation {
    Int3 origin{};
    uint32_t leafIndex = UINT32_MAX;
};
static_assert(sizeof(SubcellLocation) == 16);
static_assert(sizeof(SubcellCreateDesc) == 32 && sizeof(SubcellRequestDesc) == 48 &&
              sizeof(SubcellPreparationBudget) == 48 && sizeof(SubcellInfo) == 24);
static_assert(offsetof(SubcellRequestDesc, translation) == 32);
NVRHI_IID(IGVDBSubcellTicket, "570f38f1-6970-48e0-b294-1a2ecb29bfde")
struct IGVDBSubcellTicket : nvrhi::IRHIObject {
    NVRHI_DECLARE_UUID_TRAITS(IGVDBSubcellTicket)
    virtual nvrhi::IDevice* getDevice() const noexcept = 0;
    virtual SubcellInfo getDesc() const noexcept = 0;
    // Available after preparation. Borrowed immutable bytes; empty is {nullptr,0}.
    // uint2(offset,count) per leaf-pool/local-zyx cell, then stable original ordinals.
    virtual nvrhi::FRESULT getCellRanges(BorrowedBytes& output) const noexcept = 0;
    virtual nvrhi::FRESULT getCellLocations(BorrowedBytes& output) const noexcept = 0;
    virtual nvrhi::FRESULT getSourceOrdinals(BorrowedBytes& output) const noexcept = 0;
    // Raw-buffer mirrors, available after record. Caller owns GPU synchronization.
    // Empty is {nullptr,0,0}; failed getters preserve output.
    virtual nvrhi::FRESULT getCellBuffer(BorrowedBufferBinding& output) const noexcept = 0;
    virtual nvrhi::FRESULT getLocationBuffer(BorrowedBufferBinding& output) const noexcept = 0;
    virtual nvrhi::FRESULT getOrdinalBuffer(BorrowedBufferBinding& output) const noexcept = 0;
    // Once after caller submission/completion: actual GPU-expanded canonical PointSet.
    // All component counts/W bits and velocity/color association are retained.
    // Position XYZ includes translation; metadata bounds come from GPU results.
    // Failure clears output and permits retry. No implicit submission or wait.
    virtual nvrhi::FRESULT finishAfterCallerCompletion(IGVDBPointSet** output) noexcept = 0;
};
NVRHI_IID(IGVDBSubcells, "215376ed-f2cd-4846-b683-45763923fb0e")
struct IGVDBSubcells : nvrhi::IRHIObject {
    NVRHI_DECLARE_UUID_TRAITS(IGVDBSubcells)
    virtual nvrhi::IDevice* getDevice() const noexcept = 0;
    // Authenticated same-device retained inputs; width must divide leaf width.
    // Inclusive support [p-radius,p+1+radius], signed floor and active leaves.
    // Stable input ordinal within each cell. Inactive leaf-pool slots stay empty.
    virtual nvrhi::FRESULT prepare(const SubcellRequestDesc&, const SubcellPreparationBudget&,
                                   IGVDBSubcellTicket** output) noexcept = 0;
    virtual nvrhi::FRESULT record(RecordingContext, IGVDBSubcellTicket*) noexcept = 0;
};
using SubcellsHandle = nvrhi::AutoPtr<IGVDBSubcells>;
using SubcellTicketHandle = nvrhi::AutoPtr<IGVDBSubcellTicket>;
GVDB_NVRHI_C_API nvrhi::FRESULT gvdbCreateNvrhiSubcells(uint32_t apiVersion, const SubcellCreateDesc*,
                                                        IGVDBSubcells** output) noexcept;
inline nvrhi::FRESULT createSubcells(const SubcellCreateDesc& desc, IGVDBSubcells** output) noexcept {
    return gvdbCreateNvrhiSubcells(c_GVDBNvrhiApiVersion, &desc, output);
}
}  // namespace gvdb::portable
