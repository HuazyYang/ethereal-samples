// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "GVDBNvrhiApi.h"
namespace gvdb::portable {
struct ReductionCreateDesc {
    uint32_t structSize = sizeof(ReductionCreateDesc), reserved = 0;
    IGVDBVolume* volume = nullptr;
    const void* artifact = nullptr;
    uint64_t artifactBytes = 0;
};
struct ReductionPreparationBudget {
    uint64_t maxAllocatedBytes = UINT64_MAX;
    uint32_t maxBuffers = UINT32_MAX, reserved = 0;
};
struct ReductionValue {
    float sum = 0;
    uint32_t reserved = 0;
    uint64_t voxelCount = 0;
};
static_assert(sizeof(ReductionCreateDesc)==32 && sizeof(ReductionPreparationBudget)==16 && sizeof(ReductionValue)==16);
static_assert(std::is_standard_layout_v<ReductionValue> && std::is_trivially_copyable_v<ReductionValue>);
NVRHI_IID(IGVDBReductionTicket, "d54ce62f-ff40-4a9c-9613-1a1ab8380c6c")
struct IGVDBReductionTicket : nvrhi::IRHIObject {
    NVRHI_DECLARE_UUID_TRAITS(IGVDBReductionTicket)
    virtual nvrhi::IDevice* getDevice() const noexcept = 0; // Borrowed, retained by ticket.
    // Caller has submitted and waited. No implicit completion check, submit or wait.
    // One successful finish; failures preserve output and allow retry. Works after
    // operator/volume release. GPU float column sums, double CPU accumulation,
    // final float conversion; NaN/infinity propagate under these operations.
    virtual nvrhi::FRESULT finishAfterCallerCompletion(ReductionValue& output) noexcept = 0;
};
NVRHI_IID(IGVDBReduction, "7e7cb8d4-7f04-49d7-98cd-b5998dc2d835")
struct IGVDBReduction : nvrhi::IRHIObject {
    NVRHI_DECLARE_UUID_TRAITS(IGVDBReduction)
    virtual IGVDBVolume* getVolume() const noexcept = 0; // Borrowed; operator retains volume.
    // Authenticated old/current snapshot from this volume, R32_FLOAT only. Sums
    // active brick interiors; excludes aprons and unused slots. Empty result is 0.
    // Preparation clears output on failure and never records or changes volume.
    virtual nvrhi::FRESULT prepare(IGVDBSnapshot*,uint32_t channel,
        const ReductionPreparationBudget&,IGVDBReductionTicket** output) noexcept = 0;
    // One recording per ticket into an open same-device graphics/compute list.
    // Replaces compute state; caller owns submission/synchronization. Ticket must
    // originate from this operator. Does not consult current owner publication.
    virtual nvrhi::FRESULT record(RecordingContext,IGVDBReductionTicket*) noexcept = 0;
};
using ReductionHandle = nvrhi::AutoPtr<IGVDBReduction>;
using ReductionTicketHandle = nvrhi::AutoPtr<IGVDBReductionTicket>;
GVDB_NVRHI_C_API nvrhi::FRESULT gvdbCreateNvrhiReduction(uint32_t apiVersion,
    const ReductionCreateDesc*,IGVDBReduction** output) noexcept;
inline nvrhi::FRESULT createReduction(const ReductionCreateDesc& desc,IGVDBReduction** output) noexcept {
    return gvdbCreateNvrhiReduction(c_GVDBNvrhiApiVersion,&desc,output);
}
} // namespace gvdb::portable
