// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "GVDBNvrhiApi.h"
namespace gvdb::portable {
enum class VoxelFilterOperation : uint32_t { Smooth, Grow, Noise, Cut, ExpandByte, ExpandColor };
struct VoxelFiltersCreateDesc {
    uint32_t structSize = sizeof(VoxelFiltersCreateDesc);
    uint32_t reserved = 0;
    IGVDBVolume* volume = nullptr;
    const void* artifact = nullptr;
    uint64_t artifactBytes = 0;
};
struct VoxelFilterRequest {
    uint32_t structSize = sizeof(VoxelFilterRequest);
    uint32_t reserved = 0;
    VoxelFilterOperation operation = VoxelFilterOperation::Smooth;
    uint32_t channel = 0;
    uint32_t iterations = 1;
    uint32_t updateApron = 1; // Boolean: exactly 0 or 1.
    uint32_t skipOverAprons = 1; // Both settings preserve retained interior-only filtering.
    uint32_t reserved2 = 0;
    Float4 parameters{1,0,0,0};
    Float4 boundaryValue{}; // Storage units; integer formats require whole bytes.
};
struct VoxelFilterPreparationBudget {
    uint64_t maxAllocatedBytes = UINT64_MAX;
    uint32_t maxBuffers = UINT32_MAX;
    uint32_t reserved = 0;
};
static_assert(sizeof(VoxelFiltersCreateDesc) == 32 && sizeof(VoxelFilterRequest) == 64);
static_assert(sizeof(VoxelFilterPreparationBudget) == 16 && offsetof(VoxelFilterRequest,parameters) == 32);
static_assert(std::is_standard_layout_v<VoxelFilterRequest> && std::is_trivially_copyable_v<VoxelFilterRequest>);
NVRHI_IID(IGVDBVoxelFilters, "d9d335bf-3c4b-43ef-a346-955f49629644")
struct IGVDBVoxelFilters : nvrhi::IRHIObject {
    NVRHI_DECLARE_UUID_TRAITS(IGVDBVoxelFilters)
    virtual IGVDBVolume* getVolume() const noexcept = 0; // Borrowed; filters retain this volume.
    // Prepare only. Caller records the returned existing generation ticket through
    // volume->recordAndPublish, then submits/synchronizes using NVRHI. No hidden waits.
    // Failure clears output and leaves the current volume unchanged. A zero-iteration
    // request validates/authenticates normally and returns a one-use no-op ticket.
    // Up to 4096 iterations: filter interiors, then optionally update aprons after each
    // iteration. There is no preupdate; the first pass observes stored apron values.
    // Only the selected channel gets new storage/revisions; old snapshots stay valid.
    // Smooth supports float1/3/4; Grow/Noise/Cut R32; ExpandByte R8; ExpandColor RGBA8
    // (output alpha byte 1). All parameter and boundary components must be finite.
    virtual nvrhi::FRESULT prepare(const VoxelFilterRequest&, const VoxelFilterPreparationBudget&,
        IGVDBGenerationTicket** output) noexcept = 0;
};
using VoxelFiltersHandle = nvrhi::AutoPtr<IGVDBVoxelFilters>;
GVDB_NVRHI_C_API nvrhi::FRESULT gvdbCreateNvrhiVoxelFilters(uint32_t apiVersion,
    const VoxelFiltersCreateDesc*, IGVDBVoxelFilters** output) noexcept;
inline nvrhi::FRESULT createVoxelFilters(const VoxelFiltersCreateDesc& desc, IGVDBVoxelFilters** output) noexcept {
    return gvdbCreateNvrhiVoxelFilters(c_GVDBNvrhiApiVersion,&desc,output);
}
} // namespace gvdb::portable
