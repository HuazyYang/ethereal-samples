// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "GVDBNvrhiVolume.h"
namespace gvdb::portable {
struct VoxelOpsDesc {
    // Borrowed stable owner; it must outlive the operator and every preparation call.
    // Prepared tickets retain their own resources. The queryable facade retains its volume.
    NvrhiVolume* volume = nullptr;
    const void* artifact = nullptr;
    size_t artifactBytes = 0;
};
class NvrhiVoxelOps {
 public:
    static nvrhi::FRESULT create(const VoxelOpsDesc&, std::unique_ptr<NvrhiVoxelOps>& output) noexcept;
    ~NvrhiVoxelOps();
    NvrhiVoxelOps(const NvrhiVoxelOps&) = delete;
    NvrhiVoxelOps& operator=(const NvrhiVoxelOps&) = delete;
    // All methods prepare only: record using the owner's existing recordAndPublish.
    // Failed preparation preserves output/current generation. Current publication and
    // a matching finished host/channel configuration are required at each call.
    // Fill preserves the legacy value conventions: all-zero value clears; R8 uses
    // x in [0,255] (truncated); RGBA8 uses normalized xyz in [0,1], alpha 255;
    // RGB32 uses xyz; RGBA32 uses xyz, alpha 1. R32 uses x, or the original atlas-space
    // sine pattern when z<0. All four input components must be finite.
    // Active brick interiors AND aprons are filled; unused slots/padding stay zero.
    // Unlike the retained CUDA launch, coordinate-zero planes are not accidentally skipped.
    nvrhi::FRESULT prepareFill(uint32_t channel, Float4 value, const PreparationBudget&,
                                std::unique_ptr<PreparedGeneration>& output) noexcept;
    // Clear writes zero bits, including active aprons; it does not apply voidValue.
    nvrhi::FRESULT prepareClear(uint32_t channelMask, const PreparationBudget&,
                                 std::unique_ptr<PreparedGeneration>& output) noexcept;
    // Same-owner, same-format channel copy including existing apron bytes. No conversion.
    // Destination metadata/void value remain unchanged. Equal source/destination is a no-op.
    nvrhi::FRESULT prepareCopy(uint32_t sourceChannel, uint32_t destinationChannel,
                                const PreparationBudget&, std::unique_ptr<PreparedGeneration>& output) noexcept;
    // Preserves every interior bit and reconstructs faces/edges/corners from current
    // active neighbors. Missing sparse cells use each channel's complete void value.
    nvrhi::FRESULT prepareApron(uint32_t channelMask, const PreparationBudget&,
                                 std::unique_ptr<PreparedGeneration>& output) noexcept;
    // Per-call missing-neighbor boundary values. values is logically 32 entries;
    // only overrideMask bits are read. Those bits must be selected in channelMask.
    // All supplied values obey ChannelDesc::voidValue validation, even on no-ops.
    // The channel descriptors and retained snapshots are never modified.
    nvrhi::FRESULT prepareApron(uint32_t channelMask, const Float4* values, uint32_t overrideMask,
        const PreparationBudget&, std::unique_ptr<PreparedGeneration>& output) noexcept;
    // Zero mask, empty atlas, or apron update with apron=0 returns a valid unconsumed
    // no-op ticket. Recording validates and consumes it once, without GPU allocation,
    // dispatch, publication serial or channel/topology revision changes.
 private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    explicit NvrhiVoxelOps(std::unique_ptr<Impl>) noexcept;
    nvrhi::FRESULT prepare(uint32_t operation, uint32_t mask, uint32_t source, Float4 value,
                            const PreparationBudget&, std::unique_ptr<PreparedGeneration>& output,
                            const Float4* boundaryValues = nullptr, uint32_t overrideMask = 0) noexcept;
};
} // namespace gvdb::portable
