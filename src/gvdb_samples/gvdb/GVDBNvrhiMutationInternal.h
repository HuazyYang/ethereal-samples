// SPDX-License-Identifier: Apache-2.0
#pragma once
// Private shared preparation seam for trusted GVDB operators. Not an adoption API.
#include "GVDBNvrhiVolume.h"
namespace gvdb::portable::detail {
class VolumeMutationBuilder {
 public:
    // Requires a current publication and matching finished host/channel configuration.
    // Selected channels receive fresh owner-created raw allocations. A zero write mask
    // or an empty atlas prepares a no-op ticket without changing any revision.
    static nvrhi::FRESULT begin(NvrhiVolume&, uint32_t writeMask, const PreparationBudget&,
                                std::unique_ptr<VolumeMutationBuilder>& output) noexcept;
    static nvrhi::IDevice* device(const NvrhiVolume&) noexcept;
    ~VolumeMutationBuilder();
    VolumeMutationBuilder(const VolumeMutationBuilder&) = delete;
    VolumeMutationBuilder& operator=(const VolumeMutationBuilder&) = delete;
    const SnapshotBindings& sourceBindings() const noexcept;
    const SnapshotBindings& destinationBindings() const noexcept;
    bool hasWork() const noexcept;
    nvrhi::FRESULT allocate(uint64_t bytes, uint32_t stride, bool raw, bool constant,
                            nvrhi::BufferHandle& output) noexcept;
    nvrhi::FRESULT upload(const void* data, uint64_t bytes, uint32_t stride, bool raw,
                          bool constant, nvrhi::BufferHandle& output) noexcept;
    nvrhi::FRESULT appendDispatch(nvrhi::IComputePipeline*, nvrhi::IBindingSet*,
                                  uint32_t x, uint32_t y = 1, uint32_t z = 1) noexcept;
    // Moves an authenticated, unconsumed ticket out. No command recording occurs.
    nvrhi::FRESULT finish(std::unique_ptr<PreparedGeneration>& output) noexcept;
 private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    explicit VolumeMutationBuilder(std::unique_ptr<Impl>) noexcept;
};
} // namespace gvdb::portable::detail
