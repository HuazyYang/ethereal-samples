// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "GVDBNvrhiVolume.h"
namespace gvdb::portable {
struct ReductionResult {
    float sum = 0;
    uint32_t reserved = 0;
    uint64_t voxelCount = 0;
};
static_assert(sizeof(ReductionResult) == 16);
struct ReductionDesc {
    NvrhiVolume* volume = nullptr; // Borrowed for creation/preparation.
    const void* artifact = nullptr;
    size_t artifactBytes = 0;
};
class PreparedReduction {
 public:
    ~PreparedReduction();
    PreparedReduction(const PreparedReduction&) = delete;
    PreparedReduction& operator=(const PreparedReduction&) = delete;
 private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    explicit PreparedReduction(std::unique_ptr<Impl>) noexcept;
    friend class NvrhiReduction;
};
class NvrhiReduction {
 public:
    ~NvrhiReduction();
    NvrhiReduction(const NvrhiReduction&) = delete;
    NvrhiReduction& operator=(const NvrhiReduction&) = delete;
    static nvrhi::FRESULT create(const ReductionDesc&, std::unique_ptr<NvrhiReduction>& output) noexcept;
    // Authenticates an old or current snapshot from the borrowed owner. R32_FLOAT only.
    // Sums active brick interiors, excluding aprons and every unused atlas slot.
    // Preparation never records or waits; failed preparation preserves output.
    nvrhi::FRESULT prepare(std::shared_ptr<const VolumeGeneration>, uint32_t channel,
        const PreparationBudget&, std::unique_ptr<PreparedReduction>& output) noexcept;
    // Same-device open graphics/compute list. One record per ticket, even for empty.
    // Replaces compute state. Does not consult the live owner or current publication.
    nvrhi::FRESULT record(RecordingContext, PreparedReduction&) noexcept;
    // Caller submitted and waited. One successful finish; allocation/map failures can
    // retry. Retained resources permit completion after operator/volume destruction.
    // Float GPU column sums, then double CPU accumulation and final float conversion.
    static nvrhi::FRESULT finishAfterCallerCompletion(PreparedReduction&, ReductionResult& output) noexcept;
 private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    explicit NvrhiReduction(std::unique_ptr<Impl>) noexcept;
};
} // namespace gvdb::portable
