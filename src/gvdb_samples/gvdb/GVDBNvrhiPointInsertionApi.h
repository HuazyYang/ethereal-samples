// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "GVDBNvrhiSubcellApi.h"
namespace gvdb::portable {
enum class PointInsertionMode : uint32_t {
    Center = 0,
    VelocitySupport = 1
};
struct PointInsertionRequestDesc {
    uint32_t structSize = sizeof(PointInsertionRequestDesc);
    PointInsertionMode mode = PointInsertionMode::Center;
    IGVDBSnapshot* snapshot = nullptr;
    IGVDBPointSet* points = nullptr;
    float supportOffset = 0;
    uint32_t reserved = 0;
    Float4 translation{};
};
static_assert(sizeof(PointInsertionRequestDesc) == 48 &&
              offsetof(PointInsertionRequestDesc, translation) == 32);
NVRHI_IID(IGVDBPointInsertion, "17d56a74-6faa-477f-b9ca-0514e04fba46")
struct IGVDBPointInsertion : nvrhi::IRHIObject {
    NVRHI_DECLARE_UUID_TRAITS(IGVDBPointInsertion)
    virtual nvrhi::IDevice* getDevice() const noexcept = 0;
    // Center: lookup p+translation, offset must be zero. VelocitySupport: lookup
    // (p+translation)+velocity*offset, requiring a canonical velocity attribute.
    // Pre-translation p.z==1e10 is excluded. Finite XYZ math is required.
    // One cell per leaf-pool slot, stable source ordinals; misses/inactive excluded.
    // Completion returns original attributes with only position translation applied.
    // maxCandidateVisits counts every input point once, including misses/sentinels.
    // Other budgets and ticket/record semantics match the subcell API.
    virtual nvrhi::FRESULT prepare(const PointInsertionRequestDesc&, const SubcellPreparationBudget&,
                                   IGVDBSubcellTicket** output) noexcept = 0;
    virtual nvrhi::FRESULT record(RecordingContext, IGVDBSubcellTicket*) noexcept = 0;
};
using PointInsertionHandle = nvrhi::AutoPtr<IGVDBPointInsertion>;
// Uses the same expandSubcellPoints offline artifact and creation descriptor.
GVDB_NVRHI_C_API nvrhi::FRESULT gvdbCreateNvrhiPointInsertion(uint32_t apiVersion, const SubcellCreateDesc*,
                                                              IGVDBPointInsertion** output) noexcept;
inline nvrhi::FRESULT createPointInsertion(const SubcellCreateDesc& desc,
                                           IGVDBPointInsertion** output) noexcept {
    return gvdbCreateNvrhiPointInsertion(c_GVDBNvrhiApiVersion, &desc, output);
}
}  // namespace gvdb::portable
