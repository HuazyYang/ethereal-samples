// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "GVDBNvrhiPointApi.h"
namespace gvdb::portable {
enum class GridQueryMode : uint32_t {
    ScalarVoxel = 0,
    MacYDiagnostic = 1
};
struct GridQueryCreateDesc {
    uint32_t structSize = sizeof(GridQueryCreateDesc), reserved = 0;
    nvrhi::IDevice* device = nullptr;
    const void* artifact = nullptr;
    uint64_t artifactBytes = 0;
};
struct GridQueryRequestDesc {
    uint32_t structSize = sizeof(GridQueryRequestDesc), channel = 0;
    IGVDBSnapshot* snapshot = nullptr;
    IGVDBPointSet* points = nullptr;
    GridQueryMode mode = GridQueryMode::ScalarVoxel;
    uint32_t reserved = 0;
};
struct GridQueryPreparationBudget {
    uint64_t maxHostBytes = UINT64_MAX;       // Completed Float4 values, excluding retained inputs.
    uint64_t maxAllocatedBytes = UINT64_MAX;  // Logical new GPU/readback/dummy buffer bytes.
    uint32_t maxBuffers = UINT32_MAX, reserved = 0;
};
static_assert(sizeof(GridQueryCreateDesc) == 32 && sizeof(GridQueryRequestDesc) == 32 &&
              sizeof(GridQueryPreparationBudget) == 24);
static_assert(offsetof(GridQueryRequestDesc, mode) == 24 && std::is_standard_layout_v<GridQueryRequestDesc> &&
              std::is_trivially_copyable_v<GridQueryRequestDesc>);
NVRHI_IID(IGVDBGridQueryTicket, "2942dad9-b9f7-4b4f-aa08-fd2fb5c2eedc")
struct IGVDBGridQueryTicket : nvrhi::IRHIObject {
    NVRHI_DECLARE_UUID_TRAITS(IGVDBGridQueryTicket)
    virtual nvrhi::IDevice* getDevice() const noexcept = 0;
    virtual uint64_t getCount() const noexcept = 0;
    // After record: borrowed structured Float4 buffer. Caller synchronizes use.
    // Output tuple: scalar/interpolated value, comparison (diagnostic only),
    // active-leaf validity (0 or 1), zero. Misses are all zero. Empty is {nullptr,0,0}.
    virtual nvrhi::FRESULT getOutputBuffer(BorrowedBufferBinding&) const noexcept = 0;
    // Exactly one successful finish AFTER caller submission/wait. No hidden wait.
    // Borrowed immutable Float4 bytes survive operator/owner release until ticket release.
    // Failure preserves output and permits retry. Empty is {nullptr,0}.
    virtual nvrhi::FRESULT finishAfterCallerCompletion(BorrowedBytes&) noexcept = 0;
    virtual nvrhi::FRESULT getValues(BorrowedBytes&) const noexcept = 0;
};
NVRHI_IID(IGVDBGridQuery, "a5152480-c5d0-4099-b173-f868a09cd6de")
struct IGVDBGridQuery : nvrhi::IRHIObject {
    NVRHI_DECLARE_UUID_TRAITS(IGVDBGridQuery)
    virtual nvrhi::IDevice* getDevice() const noexcept = 0;
    // Retains authenticated same-device snapshot/points. R32_FLOAT only. Coordinates
    // are unit voxels. ScalarVoxel reads floor(position) in the active leaf.
    // MacYDiagnostic interpolates Y faces (i+.5,j,k+.5) in that leaf's stored atlas;
    // comparison uses the aligned lower corner. Both use zero atlas border, without
    // neighbor-leaf relookup or apron refresh. IEEE atlas values propagate.
    // No submission/wait/publication. Clears ticket output on failure.
    virtual nvrhi::FRESULT prepare(const GridQueryRequestDesc&, const GridQueryPreparationBudget&,
                                   IGVDBGridQueryTicket** output) noexcept = 0;
    // Once, with this operator's ticket and an open same-device graphics/compute list.
    virtual nvrhi::FRESULT record(RecordingContext, IGVDBGridQueryTicket*) noexcept = 0;
};
using GridQueryHandle = nvrhi::AutoPtr<IGVDBGridQuery>;
using GridQueryTicketHandle = nvrhi::AutoPtr<IGVDBGridQueryTicket>;
GVDB_NVRHI_C_API nvrhi::FRESULT gvdbCreateNvrhiGridQuery(uint32_t apiVersion, const GridQueryCreateDesc*,
                                                         IGVDBGridQuery** output) noexcept;
inline nvrhi::FRESULT createGridQuery(const GridQueryCreateDesc& desc, IGVDBGridQuery** output) noexcept {
    return gvdbCreateNvrhiGridQuery(c_GVDBNvrhiApiVersion, &desc, output);
}
}  // namespace gvdb::portable
