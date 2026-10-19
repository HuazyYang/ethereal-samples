// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "GVDBNvrhiPointApi.h"
namespace gvdb::portable {
enum class PointQuantizationBounds : uint32_t {
    Explicit = 0,
    LegacyPadded = 1
};
struct PointQuantizeCreateDesc {
    uint32_t structSize = sizeof(PointQuantizeCreateDesc), reserved = 0;
    nvrhi::IDevice* device = nullptr;
    const void* encodeArtifact = nullptr;
    uint64_t encodeArtifactBytes = 0;
    const void* decodeArtifact = nullptr;
    uint64_t decodeArtifactBytes = 0;
};
struct PointQuantizeRequestDesc {
    uint32_t structSize = sizeof(PointQuantizeRequestDesc), reserved = 0;
    IGVDBPointSet* points = nullptr;
    Float4 minimum{}, range{1, 1, 1, 0};
    PointQuantizationBounds bounds = PointQuantizationBounds::Explicit;
    uint32_t reserved2 = 0;
};
struct PointQuantizePreparationBudget {
    uint64_t maxPoints = UINT64_MAX, maxAllocatedBytes = UINT64_MAX, maxHostBytes = UINT64_MAX;
    uint32_t maxBuffers = UINT32_MAX, reserved = 0;
};
static_assert(sizeof(PointQuantizeCreateDesc) == 48 && sizeof(PointQuantizeRequestDesc) == 56 &&
              sizeof(PointQuantizePreparationBudget) == 32 &&
              offsetof(PointQuantizeRequestDesc, minimum) == 16);
static_assert(std::is_standard_layout_v<PointQuantizeRequestDesc> &&
              std::is_trivially_copyable_v<PointQuantizeRequestDesc>);
NVRHI_IID(IGVDBPointQuantizeTicket, "b05200d2-bb4a-4f5d-9ed7-704c93e18e7c")
struct IGVDBPointQuantizeTicket : nvrhi::IRHIObject {
    NVRHI_DECLARE_UUID_TRAITS(IGVDBPointQuantizeTicket)
    virtual nvrhi::IDevice* getDevice() const noexcept = 0;
    virtual void getBounds(Float4& minimum, Float4& range) const noexcept = 0;
    // Caller has submitted and waited. One successful completion; no implicit wait.
    // Output is an authenticated immutable PointSet of actual GPU decoded positions.
    // Position component count/W bits and velocity/color data remain unchanged.
    // Failure clears output and permits retry; factory/source owner may be released.
    virtual nvrhi::FRESULT finishAfterCallerCompletion(IGVDBPointSet** output) noexcept = 0;
    // Six bytes per point, original ordinal order, little-endian ushort3. Available
    // after successful completion; borrowed for ticket lifetime. Failure preserves output.
    virtual nvrhi::FRESULT getCodes(BorrowedBytes& output) const noexcept = 0;
    // Available after recording for explicit same-queue ordering/caller synchronization.
    // Range includes zero padding to four bytes. Empty input returns FE_NOT_FOUND.
    virtual nvrhi::FRESULT getCodeBuffer(BorrowedBufferBinding& output) const noexcept = 0;
};
NVRHI_IID(IGVDBPointQuantize, "ce1c9484-937a-430b-8924-b91ebf7d85fc")
struct IGVDBPointQuantize : nvrhi::IRHIObject {
    NVRHI_DECLARE_UUID_TRAITS(IGVDBPointQuantize)
    virtual nvrhi::IDevice* getDevice() const noexcept = 0;
    // Retains a completed authenticated same-device PointSet; no CPU/GPU recomputation
    // of its positions, implicit submission, or topology mutation. Budgets cover new
    // GPU storage and retained host result bytes, excluding already-owned input data.
    virtual nvrhi::FRESULT prepare(const PointQuantizeRequestDesc&, const PointQuantizePreparationBudget&,
                                   IGVDBPointQuantizeTicket** output) noexcept = 0;
    virtual nvrhi::FRESULT record(RecordingContext, IGVDBPointQuantizeTicket*) noexcept = 0;
};
using PointQuantizeHandle = nvrhi::AutoPtr<IGVDBPointQuantize>;
using PointQuantizeTicketHandle = nvrhi::AutoPtr<IGVDBPointQuantizeTicket>;
GVDB_NVRHI_C_API nvrhi::FRESULT gvdbCreateNvrhiPointQuantize(uint32_t apiVersion,
                                                             const PointQuantizeCreateDesc*,
                                                             IGVDBPointQuantize** output) noexcept;
inline nvrhi::FRESULT createPointQuantize(const PointQuantizeCreateDesc& desc,
                                          IGVDBPointQuantize** output) noexcept {
    return gvdbCreateNvrhiPointQuantize(c_GVDBNvrhiApiVersion, &desc, output);
}
}  // namespace gvdb::portable
