// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "GVDBNvrhiApi.h"
namespace gvdb::portable {
struct VbxVector3 { float x = 0, y = 0, z = 0; };
struct VbxMetadata {
    VbxVector3 preTranslation, angles, scale{1,1,1}, translation, voxelSize{1,1,1};
    uint8_t gridName[256]{}; // Exact file bytes; no terminator or text encoding is assumed.
};
struct SerializationBudget {
    uint64_t maxBlobBytes = UINT64_MAX; // Input bytes on load; exact serialized bytes on save.
    uint64_t maxAllocatedBytes = UINT64_MAX; // GPU uploads/readback buffers, including padding.
    uint32_t maxBuffers = UINT32_MAX;
    uint32_t reserved = 0;
};
struct SerializerCreateDesc {
    uint32_t structSize = sizeof(SerializerCreateDesc);
    uint32_t reserved = 0;
    IGVDBVolume* volume = nullptr;
};
static_assert(sizeof(VbxVector3) == 12 && sizeof(VbxMetadata) == 316);
static_assert(sizeof(SerializationBudget) == 24 && sizeof(SerializerCreateDesc) == 16);
static_assert(std::is_standard_layout_v<VbxMetadata> && std::is_trivially_copyable_v<VbxMetadata>);

NVRHI_IID(IGVDBSaveTicket, "a9565707-5c5e-4591-ac70-061637e0190a")
struct IGVDBSaveTicket : nvrhi::IRHIObject {
    NVRHI_DECLARE_UUID_TRAITS(IGVDBSaveTicket)
    virtual nvrhi::IDevice* getDevice() const noexcept = 0; // Borrowed; retained by ticket.
    // Caller must submit and wait first. Never submits or waits. Works after owner and
    // serializer release. One successful completion consumes the ticket. CPU allocation
    // failure is retryable. Output/optional UTF-8 diagnostic are cleared before failure.
    virtual nvrhi::FRESULT finishAfterCallerCompletion(nvrhi::IDataBlob** output,
        nvrhi::IDataBlob** diagnostic = nullptr) noexcept = 0;
};
NVRHI_IID(IGVDBSerializer, "32232709-1855-40bc-9110-bc3ca53fd890")
struct IGVDBSerializer : nvrhi::IRHIObject {
    NVRHI_DECLARE_UUID_TRAITS(IGVDBSerializer)
    virtual IGVDBVolume* getVolume() const noexcept = 0; // Borrowed; serializer retains owner.
    // Accepts retained snapshots of this owner. Unattached generations use default
    // metadata. Failure resets output to defaults. Metadata is immutable per snapshot.
    virtual nvrhi::FRESULT getMetadata(IGVDBSnapshot*, VbxMetadata& output) const noexcept = 0;
    // Copies all input bytes before returning; never changes the owner during preparation.
    // Success returns an existing generation ticket for volume->recordAndPublish.
    // Imports preserve exact logical channel bytes, including aprons and unused slots.
    virtual nvrhi::FRESULT prepareLoadVBX(nvrhi::IDataBlob*, const SerializationBudget&,
        IGVDBGenerationTicket** output, nvrhi::IDataBlob** diagnostic = nullptr) noexcept = 0;
    // Freezes placed topology, channel order and metadata from this snapshot. An optional
    // override is copied into the ticket and does not modify snapshot metadata. Only dense
    // channel masks 0..N-1 and nonempty volumes are supported. No recording or GPU wait.
    virtual nvrhi::FRESULT prepareSaveVBX(IGVDBSnapshot*, const VbxMetadata* optionalOverride,
        const SerializationBudget&, IGVDBSaveTicket** output,
        nvrhi::IDataBlob** diagnostic = nullptr) noexcept = 0;
    // Open same-device graphics/compute list; caller owns submission/completion. Each
    // ticket records once. Validation failure leaves it available for a corrected retry.
    virtual nvrhi::FRESULT recordSaveReadback(RecordingContext, IGVDBSaveTicket*) noexcept = 0;
};
using SerializerHandle = nvrhi::AutoPtr<IGVDBSerializer>;
using SaveTicketHandle = nvrhi::AutoPtr<IGVDBSaveTicket>;
GVDB_NVRHI_C_API nvrhi::FRESULT gvdbCreateNvrhiSerializer(uint32_t apiVersion,
    const SerializerCreateDesc*, IGVDBSerializer** output) noexcept;
inline nvrhi::FRESULT createSerializer(const SerializerCreateDesc& desc, IGVDBSerializer** output) noexcept {
    return gvdbCreateNvrhiSerializer(c_GVDBNvrhiApiVersion,&desc,output);
}
} // namespace gvdb::portable
