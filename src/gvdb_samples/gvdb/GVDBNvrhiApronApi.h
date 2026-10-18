// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "GVDBNvrhiApi.h"
namespace gvdb::portable {
struct ApronCreateDesc {
    uint32_t structSize = sizeof(ApronCreateDesc), reserved = 0;
    IGVDBVolume* volume = nullptr;
    const void* artifact = nullptr; // Existing GVDBAtlasMutation.slang / mutateAtlas artifact.
    uint64_t artifactBytes = 0;
};
struct ApronBoundaryOverride {
    uint32_t channel = 0, reserved[3]{};
    Float4 value{}; // Same format/range rules as ChannelDesc::voidValue.
};
struct ApronRequestDesc {
    uint32_t structSize = sizeof(ApronRequestDesc), reserved = 0;
    uint32_t channelMask = 0, overrideCount = 0;
    const ApronBoundaryOverride* overrides = nullptr;
    uint64_t reserved2 = 0;
};
struct ApronPreparationBudget {
    uint64_t maxAllocatedBytes = UINT64_MAX;
    uint32_t maxBuffers = UINT32_MAX, reserved = 0;
};
static_assert(sizeof(ApronCreateDesc)==32 && sizeof(ApronBoundaryOverride)==32 && sizeof(ApronRequestDesc)==32 && sizeof(ApronPreparationBudget)==16);
static_assert(offsetof(ApronBoundaryOverride,value)==16 && offsetof(ApronRequestDesc,overrides)==16);
static_assert(std::is_standard_layout_v<ApronRequestDesc> && std::is_trivially_copyable_v<ApronRequestDesc>);
NVRHI_IID(IGVDBApronOps,"1c8d04c7-a048-498b-bb0e-f52608def285")
struct IGVDBApronOps : nvrhi::IRHIObject {
    NVRHI_DECLARE_UUID_TRAITS(IGVDBApronOps)
    virtual IGVDBVolume* getVolume() const noexcept = 0; // Borrowed; retained by operator.
    // Rebuilds selected faces/edges/corners from active neighbor interiors. Missing
    // neighbors use channel voidValue unless overridden for this one preparation.
    // Overrides are unique, selected channels, count<=32; every reserved field is0.
    // Values follow existing ChannelDesc validation (integer storage units, no truncation).
    // All descriptors validate even on empty/zero-mask/apron0 no-ops. No-op tickets
    // are authenticated/one-use and preserve every resource/revision on publication.
    // Interior bits and channel descriptors remain unchanged; unused slots/tail zero.
    // Clears output on failure. Copies values before return; no record/submit/wait.
    // Publish the existing ticket with volume->recordAndPublish explicitly.
    virtual nvrhi::FRESULT prepare(const ApronRequestDesc&,const ApronPreparationBudget&,
        IGVDBGenerationTicket** output) noexcept = 0;
};
using ApronOpsHandle = nvrhi::AutoPtr<IGVDBApronOps>;
GVDB_NVRHI_C_API nvrhi::FRESULT gvdbCreateNvrhiApronOps(uint32_t apiVersion,
    const ApronCreateDesc*,IGVDBApronOps** output) noexcept;
inline nvrhi::FRESULT createApronOps(const ApronCreateDesc& desc,IGVDBApronOps** output) noexcept {
    return gvdbCreateNvrhiApronOps(c_GVDBNvrhiApiVersion,&desc,output);
}
} // namespace gvdb::portable
