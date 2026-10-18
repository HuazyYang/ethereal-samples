// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "GVDBNvrhiApi.h"
namespace gvdb::portable {
struct VoxelOpsCreateDesc {
    uint32_t structSize = sizeof(VoxelOpsCreateDesc);
    uint32_t reserved = 0;
    IGVDBVolume* volume = nullptr;
    const void* artifact = nullptr;
    uint64_t artifactBytes = 0;
};
struct VoxelPreparationBudget {
    uint64_t maxAllocatedBytes = UINT64_MAX;
    uint32_t maxBuffers = UINT32_MAX;
    uint32_t reserved = 0;
};
static_assert(std::is_standard_layout_v<VoxelOpsCreateDesc> && std::is_trivially_copyable_v<VoxelOpsCreateDesc>);
static_assert(sizeof(VoxelOpsCreateDesc) == 32 && offsetof(VoxelOpsCreateDesc,volume) == 8);
static_assert(sizeof(VoxelPreparationBudget) == 16);

NVRHI_IID(IGVDBVoxelOps, "d886efba-4ee8-4fac-84aa-675723e8ef78")
struct IGVDBVoxelOps : nvrhi::IRHIObject {
    NVRHI_DECLARE_UUID_TRAITS(IGVDBVoxelOps)
    virtual IGVDBVolume* getVolume() const noexcept = 0; // Borrowed; operator retains this owner.
    // Prepare only: no recording, submission or wait. Output is cleared first;
    // success returns an owned one-use ticket for volume->recordAndPublish.
    // All components must be finite. Retained legacy conventions: all-zero
    // means clear; R8 truncates x in [0,255]; RGBA8 truncates normalized xyz
    // to bytes and writes alpha255; RGB32 stores xyz; RGBA32 stores xyz/alpha1.
    // R32 uses x, or the retained atlas sine pattern when z is negative.
    virtual nvrhi::FRESULT prepareFill(uint32_t channel, Float4 value,
        const VoxelPreparationBudget&, IGVDBGenerationTicket** output) noexcept = 0;
    // Clear writes zero to active bricks (including aprons). A zero mask is a no-op.
    virtual nvrhi::FRESULT prepareClear(uint32_t channelMask,
        const VoxelPreparationBudget&, IGVDBGenerationTicket** output) noexcept = 0;
    // Copies same-format active brick bytes, including aprons, preserving bit
    // patterns such as NaN/-0. An identical source/destination is a no-op.
    virtual nvrhi::FRESULT prepareCopy(uint32_t source, uint32_t destination,
        const VoxelPreparationBudget&, IGVDBGenerationTicket** output) noexcept = 0;
    // Regenerates apron faces, edges and corners from the current interiors and
    // each destination channel's void value. Empty/zero-mask/no-apron is a no-op.
    virtual nvrhi::FRESULT prepareApron(uint32_t channelMask,
        const VoxelPreparationBudget&, IGVDBGenerationTicket** output) noexcept = 0;
    // Operations retain unchanged bindings and replace only written channels;
    // old snapshots remain valid. Unused atlas slots and tail padding stay zero.
    // Single-threaded preparation is invalidated by host/config/base changes.
};
using VoxelOpsHandle = nvrhi::AutoPtr<IGVDBVoxelOps>;
GVDB_NVRHI_C_API nvrhi::FRESULT gvdbCreateNvrhiVoxelOps(uint32_t apiVersion,
    const VoxelOpsCreateDesc*, IGVDBVoxelOps** output) noexcept;
inline nvrhi::FRESULT createVoxelOps(const VoxelOpsCreateDesc& desc, IGVDBVoxelOps** output) noexcept {
    return gvdbCreateNvrhiVoxelOps(c_GVDBNvrhiApiVersion,&desc,output);
}
} // namespace gvdb::portable
