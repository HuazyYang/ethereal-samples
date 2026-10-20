// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "GVDBNvrhiApi.h"
namespace gvdb::portable {
// Explicit legacy ScnRay byte layout, including the formerly implicit tail padding.
// On hit, replace hit/normal. On miss, replace hit with 1e10 and preserve normal.
// Origin, unnormalized direction, color, node/index and padding are always preserved.
struct alignas(16) RayRecord {
    float hit[3]{}, normal[3]{}, origin[3]{}, direction[3]{};
    uint32_t color = 0, pointNode = 0, pointIndex = 0, padding = 0;
};
struct RayQueryCreateDesc {
    uint32_t structSize = sizeof(RayQueryCreateDesc), reserved = 0;
    nvrhi::IDevice* device = nullptr;
    const void* artifact = nullptr;
    uint64_t artifactBytes = 0;
};
struct RayQueryRequestDesc {
    uint32_t structSize = sizeof(RayQueryRequestDesc), channel = 0;
    IGVDBSnapshot* snapshot = nullptr;
    const RayRecord* rays = nullptr;  // Copied during prepare, never modified.
    uint64_t count = 0;
    float step = .25f, threshold = .3f, epsilon = .01f, bias = -.0001f;
    uint32_t maxLeafSteps = 256, maxLeafVisits = 65536, reserved = 0, reserved2 = 0;
};
struct RayQueryPreparationBudget {
    uint64_t maxHostBytes = UINT64_MAX;  // Copied records plus completed records/hits/normals: 96+64 bytes/ray.
    uint64_t maxAllocatedBytes = UINT64_MAX;  // Nonempty: 384 + 256*count logical bytes, eight buffers.
    uint32_t maxBuffers = UINT32_MAX, reserved = 0;
};
enum class RayQueryOutput : uint32_t {
    Records,
    Hits,
    Normals
};
static_assert(sizeof(RayRecord) == 64 && offsetof(RayRecord, normal) == 12 &&
              offsetof(RayRecord, origin) == 24 && offsetof(RayRecord, direction) == 36 &&
              offsetof(RayRecord, color) == 48 && offsetof(RayRecord, padding) == 60);
static_assert(sizeof(RayQueryCreateDesc) == 32 && sizeof(RayQueryRequestDesc) == 64 &&
              sizeof(RayQueryPreparationBudget) == 24 && std::is_trivially_copyable_v<RayRecord>);
NVRHI_IID(IGVDBRayQueryTicket, "f32d38da-2901-4c4b-88d7-5426a58b3987")
struct IGVDBRayQueryTicket : nvrhi::IRHIObject {
    NVRHI_DECLARE_UUID_TRAITS(IGVDBRayQueryTicket)
    virtual nvrhi::IDevice* getDevice() const noexcept = 0;
    virtual uint64_t getCount() const noexcept = 0;
    // Records are raw 64-byte elements; hits/normals are structured Float4.
    // Hits.w is 1 hit, 0 miss/invalid ray, -1 exhausted budget. Miss xyz is 1e10.
    // Normals on miss/limit are zero here, while the record retains its input normal.
    // Available after recording; caller orders GPU use. Empty returns null/zero.
    virtual nvrhi::FRESULT getOutputBuffer(RayQueryOutput, BorrowedBufferBinding&) const noexcept = 0;
    // Exactly once after caller submission/completion; no hidden submit/wait.
    virtual nvrhi::FRESULT finishAfterCallerCompletion() noexcept = 0;
    virtual nvrhi::FRESULT getValues(RayQueryOutput, BorrowedBytes&) const noexcept = 0;
};
NVRHI_IID(IGVDBRayQuery, "a4f172cc-92d8-4ca5-b4ca-4a0507ee6c69")
struct IGVDBRayQuery : nvrhi::IRHIObject {
    NVRHI_DECLARE_UUID_TRAITS(IGVDBRayQuery)
    virtual nvrhi::IDevice* getDevice() const noexcept = 0;
    // Authenticated same-device snapshot, selected R32 channel, stored aprons and
    // clamped atlas sampling. Retains the publication, copies rays, allocates only.
    // Nonfinite/zero directions are misses. Directions are not normalized;
    // step/epsilon/bias use their parameter units. Clears ticket output on failure.
    virtual nvrhi::FRESULT prepare(const RayQueryRequestDesc&, const RayQueryPreparationBudget&,
                                   IGVDBRayQueryTicket**) noexcept = 0;
    virtual nvrhi::FRESULT record(RecordingContext, IGVDBRayQueryTicket*) noexcept = 0;
};
using RayQueryHandle = nvrhi::AutoPtr<IGVDBRayQuery>;
using RayQueryTicketHandle = nvrhi::AutoPtr<IGVDBRayQueryTicket>;
GVDB_NVRHI_C_API nvrhi::FRESULT gvdbCreateNvrhiRayQuery(uint32_t, const RayQueryCreateDesc*,
                                                        IGVDBRayQuery**) noexcept;
inline nvrhi::FRESULT createRayQuery(const RayQueryCreateDesc& d, IGVDBRayQuery** out) noexcept {
    return gvdbCreateNvrhiRayQuery(c_GVDBNvrhiApiVersion, &d, out);
}
}  // namespace gvdb::portable
