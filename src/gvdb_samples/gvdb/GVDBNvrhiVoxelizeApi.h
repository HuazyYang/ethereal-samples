// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "GVDBNvrhiApi.h"
namespace gvdb::portable {
enum class MeshVoxelFormat : uint32_t {
    R8_UINT = 0,
    R32_FLOAT = 3,
    R32_SINT = 6
};
struct VoxelizeArtifact {
    const void* data = nullptr;
    uint64_t byteCount = 0;
};
struct VoxelizeCreateDesc {
    uint32_t structSize = sizeof(VoxelizeCreateDesc), reserved = 0;
    nvrhi::IDevice* device = nullptr;
    // clearTriangleBins, countTriangleBins, scatterTriangleBins, solidVoxelize, in this order.
    VoxelizeArtifact artifacts[4]{};
};
struct VoxelizeRequestDesc {
    uint32_t structSize = sizeof(VoxelizeRequestDesc), reserved = 0;
    const Float4* vertices = nullptr;
    uint64_t vertexCount = 0;
    const uint32_t* indices = nullptr;
    uint64_t indexCount = 0;  // Flat triangle list; divisible by three. Inputs copied by prepare.
    UInt3 resolution{};       // Positive, X-fast dense grid.
    MeshVoxelFormat format = MeshVoxelFormat::R32_FLOAT;
    uint32_t reserved2[2]{};
    Float4 boundsMinimum{}, boundsMaximum{1, 1, 1, 0};
    Float4 values{1, .5f, 0, 0};  // Surface, interior, exterior. W must be zero.
    Float4 transformRows[3]{
        {1, 0, 0, 0},
        {0, 1, 0, 0},
        {0, 0, 1, 0}
    };
};
struct VoxelizePreparationBudget {
    uint64_t maxInputBytes = UINT64_MAX;
    uint64_t maxAllocatedBytes =
        UINT64_MAX;  // All retained buffer descriptor bytes, including staging/dummies.
    uint64_t maxTriangleBinVisits = UINT64_MAX;    // Conservative 2 * triangleCount * binCount.
    uint64_t maxVoxelTriangleVisits = UINT64_MAX;  // Conservative voxelCount * triangleCount.
    uint32_t maxBuffers = UINT32_MAX, reserved = 0;
};
static_assert(sizeof(VoxelizeCreateDesc) == 80 && sizeof(VoxelizeRequestDesc) == 160 &&
              sizeof(VoxelizePreparationBudget) == 40);
static_assert(offsetof(VoxelizeRequestDesc, resolution) == 40 &&
              offsetof(VoxelizeRequestDesc, transformRows) == 112);
static_assert(std::is_standard_layout_v<VoxelizeRequestDesc> &&
              std::is_trivially_copyable_v<VoxelizeRequestDesc>);
NVRHI_IID(IGVDBVoxelizeTicket, "1b6cefb6-d03a-4d7a-b02a-32d2e656d153")
struct IGVDBVoxelizeTicket : nvrhi::IRHIObject {
    NVRHI_DECLARE_UUID_TRAITS(IGVDBVoxelizeTicket)
    virtual nvrhi::IDevice* getDevice() const noexcept = 0;
    virtual UInt3 getResolution() const noexcept = 0;
    virtual MeshVoxelFormat getFormat() const noexcept = 0;
    // After caller submits/waits for recordCounts. Validates counts and allocates
    // checked prefix offsets, triangle storage, dense output and readback storage.
    // Failure leaves the stage unchanged and permits retry; no implicit submit/wait.
    virtual nvrhi::FRESULT finishCountsAfterCallerCompletion() noexcept = 0;
    // Available after recordVoxelization; caller owns ordering/completion before use.
    // Borrowed raw buffer; logical byteSize excludes zero-filled R8 padding.
    virtual nvrhi::FRESULT getOutputBuffer(BorrowedBufferBinding&) const noexcept = 0;
    // After caller submits/waits for recordVoxelization. Exactly one successful finish.
    // Immutable borrowed result bytes survive operator release, until ticket release.
    // Output preserved on failure. Repeated access uses getValues.
    virtual nvrhi::FRESULT finishAfterCallerCompletion(BorrowedBytes&) noexcept = 0;
    virtual nvrhi::FRESULT getValues(BorrowedBytes&) const noexcept = 0;
};
NVRHI_IID(IGVDBVoxelize, "d3b25967-6857-41d5-9ff6-bda38819fb9f")
struct IGVDBVoxelize : nvrhi::IRHIObject {
    NVRHI_DECLARE_UUID_TRAITS(IGVDBVoxelize)
    virtual nvrhi::IDevice* getDevice() const noexcept = 0;
    // No volume ownership, commands or submissions. Empty meshes fill the grid
    // with exterior values. Failure clears output. See the numerical contract.
    virtual nvrhi::FRESULT prepare(const VoxelizeRequestDesc&, const VoxelizePreparationBudget&,
                                   IGVDBVoxelizeTicket**) noexcept = 0;
    // Authenticated, one-use phases into an open same-device graphics/compute list.
    // Each replaces compute state. Caller submits and waits between stages.
    virtual nvrhi::FRESULT recordCounts(RecordingContext, IGVDBVoxelizeTicket*) noexcept = 0;
    virtual nvrhi::FRESULT recordVoxelization(RecordingContext, IGVDBVoxelizeTicket*) noexcept = 0;
};
using VoxelizeHandle = nvrhi::AutoPtr<IGVDBVoxelize>;
using VoxelizeTicketHandle = nvrhi::AutoPtr<IGVDBVoxelizeTicket>;
GVDB_NVRHI_C_API nvrhi::FRESULT gvdbCreateNvrhiVoxelize(uint32_t apiVersion, const VoxelizeCreateDesc*,
                                                        IGVDBVoxelize**) noexcept;
inline nvrhi::FRESULT createVoxelize(const VoxelizeCreateDesc& desc, IGVDBVoxelize** output) noexcept {
    return gvdbCreateNvrhiVoxelize(c_GVDBNvrhiApiVersion, &desc, output);
}
}  // namespace gvdb::portable
