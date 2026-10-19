// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "GVDBNvrhiPointApi.h"
namespace gvdb::portable {
enum class DepositKind : uint32_t {
    Central = 0,
    SixNeighbors = 1,
    SupportXZ = 2
};
enum class DepositColor : uint32_t {
    None = 0,
    LastOrdinal = 1,
    AverageRGB = 2
};
struct DepositCreateDesc {
    uint32_t structSize = sizeof(DepositCreateDesc), reserved = 0;
    IGVDBVolume* volume = nullptr;
    const void* artifact = nullptr;
    uint64_t artifactBytes = 0;
};
struct DepositRequestDesc {
    uint32_t structSize = sizeof(DepositRequestDesc), reserved = 0;
    IGVDBPointSet* points = nullptr;
    DepositKind kind = DepositKind::Central;
    uint32_t channel = 0, colorChannel = UINT32_MAX, materialChannel = UINT32_MAX;
    DepositColor color = DepositColor::None;
    uint32_t reserved2 = 0;
    float radius = 1, directionOffset = 0;
    Float4 translation{};
};
struct DepositPreparationBudget {
    uint64_t maxPoints = UINT64_MAX, maxContributions = UINT64_MAX, maxAllocatedBytes = UINT64_MAX,
             maxHostBytes = UINT64_MAX;
    uint32_t maxBuffers = UINT32_MAX, reserved = 0;
};
static_assert(sizeof(DepositCreateDesc) == 32 && sizeof(DepositRequestDesc) == 64 &&
              sizeof(DepositPreparationBudget) == 40);
static_assert(offsetof(DepositRequestDesc, points) == 8 && offsetof(DepositRequestDesc, translation) == 48);
static_assert(std::is_standard_layout_v<DepositRequestDesc> &&
              std::is_trivially_copyable_v<DepositRequestDesc>);
NVRHI_IID(IGVDBDeposit, "737e4c6b-64c8-47fd-8a2d-e242b283d318")
struct IGVDBDeposit : nvrhi::IRHIObject {
    NVRHI_DECLARE_UUID_TRAITS(IGVDBDeposit)
    virtual IGVDBVolume* getVolume() const noexcept = 0;
    // Same-binary authenticated points, same device, finished current publication.
    // Accumulates R32_FLOAT polynomial contributions in original ordinal order.
    // Expansion writes the associated brick's physical aprons, not neighbor interiors.
    // SupportXZ uses canonical velocity XYZ as direction and writes R8 material1.
    // Color requires RGBA8_UINT and is central-only: last ordinal or integer RGB
    // average with alpha255. Support does not color. No implicit apron refresh.
    // Clears output on failure; no record/submit/wait or topology mutation.
    virtual nvrhi::FRESULT prepare(const DepositRequestDesc&, const DepositPreparationBudget&,
                                   IGVDBGenerationTicket** output) noexcept = 0;
};
using DepositHandle = nvrhi::AutoPtr<IGVDBDeposit>;
GVDB_NVRHI_C_API nvrhi::FRESULT gvdbCreateNvrhiDeposit(uint32_t apiVersion, const DepositCreateDesc*,
                                                       IGVDBDeposit** output) noexcept;
inline nvrhi::FRESULT createDeposit(const DepositCreateDesc& desc, IGVDBDeposit** output) noexcept {
    return gvdbCreateNvrhiDeposit(c_GVDBNvrhiApiVersion, &desc, output);
}
}  // namespace gvdb::portable
