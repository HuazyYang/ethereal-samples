// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "GVDBNvrhiPointApi.h"
namespace gvdb::portable {
struct PointSplitCreateDesc {
    uint32_t structSize=sizeof(PointSplitCreateDesc),reserved=0;
    nvrhi::IDevice* device=nullptr;
    const void* artifact=nullptr;
    uint64_t artifactBytes=0;
};
struct PointSplitRequestDesc {
    uint32_t structSize=sizeof(PointSplitRequestDesc);
    PointAttribute attribute=PointAttribute::Position;
    IGVDBPointSet* points=nullptr;
    Float4 translation{}; // XYZ only; W must be zero.
};
struct PointSplitPreparationBudget {
    uint64_t maxHostBytes=UINT64_MAX,maxAllocatedBytes=UINT64_MAX;
    uint32_t maxBuffers=UINT32_MAX,reserved=0;
};
static_assert(sizeof(PointSplitCreateDesc)==32 && sizeof(PointSplitRequestDesc)==32 && sizeof(PointSplitPreparationBudget)==24);
static_assert(offsetof(PointSplitRequestDesc,translation)==16 && std::is_trivially_copyable_v<PointSplitRequestDesc>);
NVRHI_IID(IGVDBPointSplitTicket,"a2f22cc5-aaca-486c-b3a7-d12c8fb7e743")
struct IGVDBPointSplitTicket:nvrhi::IRHIObject {
    NVRHI_DECLARE_UUID_TRAITS(IGVDBPointSplitTicket)
    virtual nvrhi::IDevice* getDevice() const noexcept=0;
    virtual uint64_t getCount() const noexcept=0;
    // Axis 0/1/2: borrowed raw float range available after record. Caller synchronizes.
    // Empty is {nullptr,0,0}; invalid axis/early call preserves output.
    virtual nvrhi::FRESULT getAxisBuffer(uint32_t axis,BorrowedBufferBinding&) const noexcept=0;
    // Exactly once AFTER caller submission/wait; no implicit submission/readiness check.
    // Actual GPU values and bounds survive source/operator release until ticket release.
    virtual nvrhi::FRESULT finishAfterCallerCompletion() noexcept=0;
    virtual nvrhi::FRESULT getAxisData(uint32_t axis,BorrowedBytes&) const noexcept=0;
    // Exact XYZ extrema, W=0. Empty bounds are both zero. Failure preserves outputs.
    virtual nvrhi::FRESULT getBounds(Float4& minimum,Float4& maximum) const noexcept=0;
};
NVRHI_IID(IGVDBPointSplit,"a4cd9924-8e63-4814-91ef-9258879f1cce")
struct IGVDBPointSplit:nvrhi::IRHIObject {
    NVRHI_DECLARE_UUID_TRAITS(IGVDBPointSplit)
    virtual nvrhi::IDevice* getDevice() const noexcept=0;
    // Position/Velocity only; finite canonical XYZ plus translation. Missing velocity
    // returns FE_NOT_FOUND, including an empty initial upload without optional attributes.
    // Retained host bytes=12*N; new GPU/readback bytes=256+24*N in 3 buffers for N>0.
    // Empty needs no allocations. Existing input/object/native alignment costs excluded.
    virtual nvrhi::FRESULT prepare(const PointSplitRequestDesc&,const PointSplitPreparationBudget&,
        IGVDBPointSplitTicket** output) noexcept=0;
    virtual nvrhi::FRESULT record(RecordingContext,IGVDBPointSplitTicket*) noexcept=0;
};
using PointSplitHandle=nvrhi::AutoPtr<IGVDBPointSplit>;
using PointSplitTicketHandle=nvrhi::AutoPtr<IGVDBPointSplitTicket>;
GVDB_NVRHI_C_API nvrhi::FRESULT gvdbCreateNvrhiPointSplit(uint32_t apiVersion,const PointSplitCreateDesc*,
    IGVDBPointSplit** output) noexcept;
inline nvrhi::FRESULT createPointSplit(const PointSplitCreateDesc& d,IGVDBPointSplit** out) noexcept {
    return gvdbCreateNvrhiPointSplit(c_GVDBNvrhiApiVersion,&d,out);
}
} // namespace gvdb::portable
