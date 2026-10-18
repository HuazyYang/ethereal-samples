// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "GVDBNvrhiPointApi.h"
namespace gvdb::portable {
enum class GatherKind : uint32_t { Density = 0, LevelSet = 1 };
struct GatherCreateDesc {
    uint32_t structSize = sizeof(GatherCreateDesc), reserved = 0;
    IGVDBVolume* volume = nullptr;
    const void* artifact = nullptr;
    uint64_t artifactBytes = 0;
};
struct GatherRequestDesc {
    uint32_t structSize = sizeof(GatherRequestDesc), reserved = 0;
    GatherKind kind = GatherKind::Density;
    uint32_t channel = 0;
    IGVDBPointSet* points = nullptr;
    uint32_t colorChannel = UINT32_MAX, subcellWidth = 2;
    float radius = 1;
    uint32_t accumulate = 0; // Exactly 0 or 1.
    float supportRadius = -1; // -1 follows radius; otherwise finite >=0 for bin coverage only.
    uint32_t reserved2 = 0;
    Float4 translation{}; // All finite; XYZ added in binary32, W unused.
};
struct GatherPreparationBudget {
    uint64_t maxCells = UINT64_MAX; // All leaf-pool cell slots, including inactive leaves.
    uint64_t maxCandidateVisits = UINT64_MAX; // Clipped support visits before sparse lookup.
    uint64_t maxPointPairs = UINT64_MAX; // Actual cell-to-point associations.
    uint64_t maxAllocatedBytes = UINT64_MAX; // New GPU storage, uploads and constants.
    uint32_t maxBuffers = UINT32_MAX, reserved = 0;
};
static_assert(sizeof(GatherCreateDesc)==32 && sizeof(GatherRequestDesc)==64 && sizeof(GatherPreparationBudget)==40);
static_assert(offsetof(GatherRequestDesc,points)==16 && offsetof(GatherRequestDesc,supportRadius)==40 &&
    offsetof(GatherRequestDesc,reserved2)==44 && offsetof(GatherRequestDesc,translation)==48);
static_assert(std::is_standard_layout_v<GatherRequestDesc> && std::is_trivially_copyable_v<GatherRequestDesc>);
NVRHI_IID(IGVDBGather,"6d7e5ab4-8855-4181-90ad-fd6ca77169a0")
struct IGVDBGather : nvrhi::IRHIObject {
    NVRHI_DECLARE_UUID_TRAITS(IGVDBGather)
    virtual IGVDBVolume* getVolume() const noexcept = 0; // Borrowed; retained by operator.
    // Authenticated same-device PointSet and a current finished publication required.
    // Scalar channel R32_FLOAT; optional distinct RGBA8_UINT channel needs point colors.
    // Width 1/2/4/8 must divide leaf width. Stable input order, signed phase-relative
    // cells, and binary32 translated points define coverage; velocity is unused.
    // supportRadius controls bins independently from scalar distance radius.
    // Fresh interiors reset to density0/levelset3 and color0 even with no point pairs.
    // Accumulation preserves exact seed bits in empty cells. Both preserve active
    // aprons exactly and zero unused slots; neither performs an implicit apron update.
    // Empty atlas is an authenticated one-use no-op after descriptor validation.
    // Clears output on failure. Does not record/submit/wait or grow topology; publish
    // the returned existing ticket through volume->recordAndPublish explicitly.
    virtual nvrhi::FRESULT prepare(const GatherRequestDesc&,const GatherPreparationBudget&,
        IGVDBGenerationTicket** output) noexcept = 0;
};
using GatherHandle = nvrhi::AutoPtr<IGVDBGather>;
GVDB_NVRHI_C_API nvrhi::FRESULT gvdbCreateNvrhiGather(uint32_t apiVersion,
    const GatherCreateDesc*,IGVDBGather** output) noexcept;
inline nvrhi::FRESULT createGather(const GatherCreateDesc& desc,IGVDBGather** output) noexcept {
    return gvdbCreateNvrhiGather(c_GVDBNvrhiApiVersion,&desc,output);
}
} // namespace gvdb::portable
