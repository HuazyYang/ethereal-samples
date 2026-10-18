// SPDX-License-Identifier: Apache-2.0
#pragma once
// M5 Step C: SDK-free C++ ownership API. No queue, submission, or completion wrapper.
#include "GVDBHostVolume.h"
#include <memory>
#include <vector>

namespace gvdb::portable {
struct VolumeOwnerDesc {
    nvrhi::IDevice* device = nullptr;
    LevelConfig configuration;
    HostVolumeLimits hostLimits;
    const void* remapArtifact = nullptr;
    size_t remapArtifactBytes = 0;
};
struct AtlasPlacement {
    UInt3 minimumBrickCount{16, 16, 1};
    uint32_t apron = 1;
    // Preserve by signed leaf origin when leaf width and channel incarnation match.
    // Changing apron/grid is supported. New/removed-then-added channels initialize anew.
    bool preserve = true;
};
struct PreparationBudget {
    uint64_t maxAllocatedBytes = UINT64_MAX;
    uint32_t maxBuffers = UINT32_MAX;
};
struct ChannelUpload {
    uint32_t channel = 0;
    const void* data = nullptr;
    uint64_t byteCount = 0;
    // Exactly the destination layout's logical bytes. Copied during preparation.
    // Only active brick interiors are authoritative; aprons are regenerated.
};
struct GenerationRequest {
    AtlasPlacement placement;
    const ChannelUpload* uploads = nullptr;
    size_t uploadCount = 0;
    PreparationBudget budget;
};
class NvrhiVolume;
namespace detail { class VolumeMutationBuilder; class VolumeAdoptionBuilder; }
class PreparedGeneration {
 public:
    ~PreparedGeneration();
    PreparedGeneration(const PreparedGeneration&) = delete;
    PreparedGeneration& operator=(const PreparedGeneration&) = delete;
 private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    explicit PreparedGeneration(std::unique_ptr<Impl>) noexcept;
    friend class NvrhiVolume;
    friend class detail::VolumeMutationBuilder;
    friend class detail::VolumeAdoptionBuilder;
};
// Authenticated retained generation. Bindings and buffers survive the publishing owner.
// The binding description is immutable; its GPU contents obey caller queue ordering.
class VolumeGeneration {
 public:
    ~VolumeGeneration();
    const SnapshotBindings& bindings() const noexcept;
    VolumeGeneration(const VolumeGeneration&) = delete;
    VolumeGeneration& operator=(const VolumeGeneration&) = delete;
 private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    explicit VolumeGeneration(std::unique_ptr<Impl>) noexcept;
    friend class NvrhiVolume;
    friend class detail::VolumeMutationBuilder;
    friend class detail::VolumeAdoptionBuilder;
};
class PreparedReadback {
 public:
    ~PreparedReadback();
    PreparedReadback(const PreparedReadback&) = delete;
    PreparedReadback& operator=(const PreparedReadback&) = delete;
 private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    explicit PreparedReadback(std::unique_ptr<Impl>) noexcept;
    friend class NvrhiVolume;
};
struct ChannelReadback {
    uint32_t channelMask = 0;
    std::array<std::vector<uint8_t>, c_MaxChannels> channels;
};
class NvrhiVolume {
 public:
    // Copies the local artifact; runtime consumers need neither a compiler nor a VFS.
    // Failure preserves an existing output owner. This C++ API does not clear it.
    static nvrhi::FRESULT create(const VolumeOwnerDesc&, std::unique_ptr<NvrhiVolume>& output) noexcept;
    ~NvrhiVolume();
    NvrhiVolume(const NvrhiVolume&) = delete;
    NvrhiVolume& operator=(const NvrhiVolume&) = delete;
    // The HostVolume object address is stable. A transactional topology adoption
    // invalidates every borrowed pool/node pointer. Bare NodeId has no epoch bits;
    // callers must pair cached IDs with host().revision().epoch and reacquire after adoption.
    HostVolume& host() noexcept;
    const HostVolume& host() const noexcept;
    // Every successful set starts a fresh incarnation, even with an identical descriptor.
    nvrhi::FRESULT setChannel(uint32_t channel, const ChannelDesc&) noexcept;
    nvrhi::FRESULT removeChannel(uint32_t channel) noexcept;
    // Requires finishTopology. No GPU allocation. Lets callers size full atlas uploads.
    // GPU publication requires node endpoints and leaf aprons inside [-8388607,8388607];
    // the CPU HostVolume deliberately supports a broader signed32 coordinate domain.
    nvrhi::FRESULT describePlacement(const AtlasPlacement&, VolumeInfo& output) const noexcept;
    // All validation, allocation, input copies, and bindings precede recording.
    // Failed preparation leaves output and the published generation unchanged.
    nvrhi::FRESULT prepareGeneration(const GenerationRequest&, std::unique_ptr<PreparedGeneration>& output) noexcept;
    // Open same-device graphics/compute list required. Never opens/closes/submits/waits.
    // Replaces compute state: callers must rebind before their next dispatch.
    // Tickets are single-use and reject owner/configuration/host/base-generation changes.
    // Publication means commands were recorded, not completed. Caller must submit in
    // generation order, synchronize cross-queue use, and not discard published work.
    // Validation/allocation failure is atomic; driver/device loss or exceptions during
    // void NVRHI recording calls cannot roll back already recorded commands.
    nvrhi::FRESULT recordAndPublish(RecordingContext, PreparedGeneration&) noexcept;
    std::shared_ptr<const VolumeGeneration> snapshot() const noexcept;
    // Only generations from this owner are accepted, including retained old generations.
    nvrhi::FRESULT prepareReadback(std::shared_ptr<const VolumeGeneration>, uint32_t channelMask,
                                  std::unique_ptr<PreparedReadback>& output) noexcept;
    nvrhi::FRESULT recordReadback(RecordingContext, PreparedReadback&) noexcept;
    // Caller has submitted AND waited using NVRHI. This function does not infer readiness
    // from a revision, does not submit/wait, and returns logical bytes without padding.
    static nvrhi::FRESULT finishReadbackAfterCompletion(PreparedReadback&, ChannelReadback& output) noexcept;
 private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    explicit NvrhiVolume(std::unique_ptr<Impl>) noexcept;
    nvrhi::FRESULT prepareGenerationForHost(const HostVolume&, const GenerationRequest&,
                                           std::unique_ptr<PreparedGeneration>& output) noexcept;
    friend class detail::VolumeMutationBuilder;
    friend class detail::VolumeAdoptionBuilder;
};
} // namespace gvdb::portable
