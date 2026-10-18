// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "GVDBNvrhiApi.h"
namespace gvdb::portable {
struct DownsampleCreateDesc {
    uint32_t structSize = sizeof(DownsampleCreateDesc), reserved = 0;
    nvrhi::IDevice* device = nullptr;
    const void* artifact = nullptr;
    uint64_t artifactBytes = 0;
};
struct DownsampleRequestDesc {
    uint32_t structSize = sizeof(DownsampleRequestDesc), reserved[3]{};
    const void* sourceData = nullptr; // Complete dense X-fast Float32, copied by prepare.
    uint64_t sourceBytes = 0;
    UInt3 sourceResolution{};
    uint32_t reserved2 = 0;
    UInt3 destinationResolution{};
    uint32_t reserved3 = 0;
    Float4 destinationExtent{1,1,1,0}; // XYZ positive; all four components finite.
    Float4 transformRows[3]{{1,0,0,0},{0,1,0,0},{0,0,1,0}};
    Float4 remap{0,1,0,1}; // Input min/max, output min/max; finite nonzero input span.
};
struct DownsamplePreparationBudget {
    uint64_t maxInputBytes = UINT64_MAX; // Logical source bytes copied; excludes trailing span bytes.
    uint64_t maxAllocatedBytes = UINT64_MAX; // NVRHI buffer descriptor bytes, including readback.
    uint64_t maxSampleVisits = UINT64_MAX; // Conservative sourceCount * destinationCount, NOT actual visits.
    uint32_t maxBuffers = UINT32_MAX, reserved = 0;
};
static_assert(sizeof(DownsampleCreateDesc)==32 && sizeof(DownsampleRequestDesc)==144 && sizeof(DownsamplePreparationBudget)==32);
static_assert(offsetof(DownsampleRequestDesc,sourceData)==16 && offsetof(DownsampleRequestDesc,transformRows)==80 && offsetof(DownsampleRequestDesc,remap)==128);
static_assert(std::is_standard_layout_v<DownsampleRequestDesc> && std::is_trivially_copyable_v<DownsampleRequestDesc>);
NVRHI_IID(IGVDBDownsampleTicket,"aab17c34-f3f1-4a14-9298-e2ece5c283c0")
struct IGVDBDownsampleTicket : nvrhi::IRHIObject {
    NVRHI_DECLARE_UUID_TRAITS(IGVDBDownsampleTicket)
    virtual nvrhi::IDevice* getDevice() const noexcept = 0; // Borrowed; ticket retains device.
    virtual UInt3 getDestinationResolution() const noexcept = 0;
    // Caller has submitted and waited. No implicit submit/wait/readiness check.
    // One successful completion; failure preserves output and permits retry.
    // GPU serial Float32 sums; CPU final float(double(sum)/double(sampleCount)).
    // Borrowed immutable X-fast Float32 bytes remain valid until ticket release.
    // Completion works after operator release. Empty output is {nullptr,0}.
    virtual nvrhi::FRESULT finishAfterCallerCompletion(BorrowedBytes& output) noexcept = 0;
    // Repeated result access after successful completion; failure preserves output.
    virtual nvrhi::FRESULT getValues(BorrowedBytes& output) const noexcept = 0;
};
NVRHI_IID(IGVDBDownsample,"6a7268b6-f384-4b30-90d8-b7a9783f9c09")
struct IGVDBDownsample : nvrhi::IRHIObject {
    NVRHI_DECLARE_UUID_TRAITS(IGVDBDownsample)
    virtual nvrhi::IDevice* getDevice() const noexcept = 0; // Borrowed; operator retains device.
    // Device operation independent of volume ownership/topology. Source bytes may
    // be destroyed after prepare. IEEE nonfinite samples follow remap/sum arithmetic.
    // Only two region endpoints are transformed; truncation/clamping precedes
    // inclusive Z/Y/X iteration (X fastest). This is not an eight-corner box transform.
    // Failure clears output. No commands/submissions/waits occur here.
    // Any zero destination axis produces a no-work ticket. Source geometry/full
    // nonnull span and every descriptor still validate, but source is not dereferenced.
    virtual nvrhi::FRESULT prepare(const DownsampleRequestDesc&,const DownsamplePreparationBudget&,
        IGVDBDownsampleTicket** output) noexcept = 0;
    // One record into an open same-device graphics/compute command list. Ticket
    // must originate from this factory. Replaces compute state; caller submits/waits.
    virtual nvrhi::FRESULT record(RecordingContext,IGVDBDownsampleTicket*) noexcept = 0;
};
using DownsampleHandle = nvrhi::AutoPtr<IGVDBDownsample>;
using DownsampleTicketHandle = nvrhi::AutoPtr<IGVDBDownsampleTicket>;
GVDB_NVRHI_C_API nvrhi::FRESULT gvdbCreateNvrhiDownsample(uint32_t apiVersion,
    const DownsampleCreateDesc*,IGVDBDownsample** output) noexcept;
inline nvrhi::FRESULT createDownsample(const DownsampleCreateDesc& desc,IGVDBDownsample** output) noexcept {
    return gvdbCreateNvrhiDownsample(c_GVDBNvrhiApiVersion,&desc,output);
}
} // namespace gvdb::portable
