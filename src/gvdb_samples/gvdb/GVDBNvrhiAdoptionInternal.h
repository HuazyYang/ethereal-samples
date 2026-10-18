// SPDX-License-Identifier: Apache-2.0
#pragma once
// Private transactional topology/atlas import seam. Not installed or externally exported.
#include "GVDBNvrhiVolume.h"
#include "slang/GVDBPortableData.h"
namespace gvdb::portable::detail {
struct ImportedLeafSlot { uint32_t leafIndex = 0, slot = 0; };
struct ImportedPlacement {
    UInt3 brickCount{};
    uint32_t apron = 0;
    const ImportedLeafSlot* leaves = nullptr;
    size_t leafCount = 0;
};
struct ImportedChannel {
    uint32_t channel = 0;
    ChannelDesc desc;
    const void* data = nullptr;
    uint64_t byteCount = 0;
};
struct VolumeImportRequest {
    // Deep-copied during preparation. The candidate itself and its revisions remain unchanged.
    const HostVolume* candidate = nullptr;
    ImportedPlacement placement;
    const ImportedChannel* channels = nullptr;
    size_t channelCount = 0;
    PreparationBudget budget;
    // Immutable serializer-owned extension; the generic owner does not inspect its schema.
    nvrhi::AutoPtr<nvrhi::IRHIObject> attachment;
};
class VolumeAdoptionBuilder {
 public:
    // Replaces host configuration/topology and the entire channel set, preserving exact
    // logical imported bytes (including aprons/unused slots). Only physical padding is zero.
    // All graph/placement/resource preparation happens before returning the existing ticket.
    // Output/target host/current generation remain untouched on failure. The base target host
    // must be finished; later host/config/publication changes make the ticket stale.
    static nvrhi::FRESULT prepareImport(NvrhiVolume&, const VolumeImportRequest&,
                                        std::unique_ptr<PreparedGeneration>& output) noexcept;
    // Dense rebuild transaction: deep-copies a finished candidate, keeps
    // configured channels/incarnations, and uses normal signed-origin remapping.
    // The candidate determines the complete resulting active leaf set, even if it
    // only removes leaves or becomes empty. Uploads override selected interiors;
    // all aprons/unused slots follow the normal generation policy. New host epoch.
    // Metadata retention is explicit because a rebuild can keep the coordinate
    // system despite replacing every node ID. No live-host mutation or hidden wait.
    static nvrhi::FRESULT prepareTopology(NvrhiVolume&, const HostVolume& candidate,
        const GenerationRequest&, bool preserveAttachment,
        std::unique_ptr<PreparedGeneration>& output) noexcept;
    static nvrhi::FRESULT describeTopologyPlacement(const HostVolume&, const AtlasPlacement&,
                                                   VolumeInfo& output) noexcept;
    // Copies frozen, placed CPU metadata from an authenticated retained generation. No
    // live HostVolume, GPU readback or wait is consulted. Failure preserves output.
    static nvrhi::FRESULT exportTopology(const VolumeGeneration&, slang::PackedVolume& output) noexcept;
    static nvrhi::FRESULT validateGeneration(const NvrhiVolume&, const VolumeGeneration&) noexcept;
    static nvrhi::IRHIObject* attachment(const VolumeGeneration&) noexcept; // Borrowed from generation.
 private:
    static nvrhi::FRESULT cloneHostForOwner(NvrhiVolume&, const HostVolume&,
                                           std::unique_ptr<HostVolume>& output) noexcept;
};
} // namespace gvdb::portable::detail
