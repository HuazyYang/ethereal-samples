// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "GVDBNvrhiApi.h"
namespace gvdb::portable {
struct ResampleCreateDesc {
    uint32_t structSize = sizeof(ResampleCreateDesc), reserved = 0;
    IGVDBVolume* volume = nullptr;
    const void* artifact = nullptr;
    uint64_t artifactBytes = 0;
};
struct ResampleRequestDesc {
    uint32_t structSize = sizeof(ResampleRequestDesc), reserved = 0;
    uint32_t channel = 0, preserveOutside = 1; // Boolean: exactly 0 or 1.
    const void* sourceData = nullptr; // Dense X-fast Float32, copied during preparation.
    uint64_t sourceBytes = 0;
    UInt3 sourceResolution{};
    uint32_t reserved2 = 0;
    Float4 transformRows[3]{{1,0,0,0},{0,1,0,0},{0,0,1,0}};
    Float4 remap{0,1,0,1}; // Input min/max, output min/max.
    Float4 background{}; // Uses X; all components must be finite.
};
struct ResamplePreparationBudget {
    uint64_t maxAllocatedBytes = UINT64_MAX;
    uint32_t maxBuffers = UINT32_MAX, reserved = 0;
};
static_assert(sizeof(ResampleCreateDesc)==32 && sizeof(ResampleRequestDesc)==128 && sizeof(ResamplePreparationBudget)==16);
static_assert(offsetof(ResampleRequestDesc,transformRows)==48 && offsetof(ResampleRequestDesc,background)==112);
static_assert(std::is_standard_layout_v<ResampleRequestDesc> && std::is_trivially_copyable_v<ResampleRequestDesc>);
NVRHI_IID(IGVDBResample,"72d06b6b-d161-4d1e-9ee7-c67c3751a4dd")
struct IGVDBResample : nvrhi::IRHIObject {
    NVRHI_DECLARE_UUID_TRAITS(IGVDBResample)
    virtual IGVDBVolume* getVolume() const noexcept = 0; // Borrowed; operator retains volume.
    // R32_FLOAT channel of current finished publication. Every active interior/apron
    // receives its transformed integer sample; unused slots are zero. Coordinates
    // truncate toward zero, including (-1,0)->0. Outside source or nonfinite/out-of-int
    // transformed coordinates preserve exact old bits, or use background.x when
    // preserveOutside=0. Input values may be nonfinite and follow remap arithmetic.
    // Failure clears output and leaves owner unchanged. Caller records the returned
    // existing generation ticket through volume->recordAndPublish, then submits/waits.
    // No commands are recorded here; source bytes may be destroyed after preparation.
    // Empty atlas returns an authenticated one-use no-op after descriptor validation.
    virtual nvrhi::FRESULT prepare(const ResampleRequestDesc&,const ResamplePreparationBudget&,
        IGVDBGenerationTicket** output) noexcept = 0;
};
using ResampleHandle = nvrhi::AutoPtr<IGVDBResample>;
GVDB_NVRHI_C_API nvrhi::FRESULT gvdbCreateNvrhiResample(uint32_t apiVersion,
    const ResampleCreateDesc*,IGVDBResample** output) noexcept;
inline nvrhi::FRESULT createResample(const ResampleCreateDesc& desc,IGVDBResample** output) noexcept {
    return gvdbCreateNvrhiResample(c_GVDBNvrhiApiVersion,&desc,output);
}
} // namespace gvdb::portable
