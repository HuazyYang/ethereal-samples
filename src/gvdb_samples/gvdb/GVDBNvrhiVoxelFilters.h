// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "GVDBNvrhiVolume.h"
namespace gvdb::portable {
enum class VoxelFilterOp : uint32_t { Smooth, Grow, Noise, Cut, ExpandByte, ExpandColor };
inline constexpr uint32_t c_MaxVoxelFilterIterations = 4096;
struct VoxelFilterDesc {
    VoxelFilterOp operation = VoxelFilterOp::Smooth;
    uint32_t channel = 0;
    uint32_t iterations = 1;
    Float4 parameters{1, 0, 0, 0};
    // Explicit boundary in storage units: byte channels use integer [0,255] components.
    // Full vector boundaries extend the retained scalar boundValue interface.
    Float4 boundaryValue{};
    bool updateApron = true;
    // Both settings filter interiors: retained kernels always use GVDB_VOXPACKED.
    // False only enlarged their launch grid; it did not filter apron cells.
    bool skipOverAprons = true;
};
struct VoxelFiltersDesc {
    // Borrowed stable owner, which must outlive the operator and preparation calls.
    // Prepared tickets independently retain every resource used by recording.
    NvrhiVolume* volume = nullptr;
    const void* artifact = nullptr;
    size_t artifactBytes = 0;
};
class NvrhiVoxelFilters {
 public:
    static nvrhi::FRESULT create(const VoxelFiltersDesc&, std::unique_ptr<NvrhiVoxelFilters>& output) noexcept;
    ~NvrhiVoxelFilters();
    NvrhiVoxelFilters(const NvrhiVoxelFilters&) = delete;
    NvrhiVoxelFilters& operator=(const NvrhiVoxelFilters&) = delete;
    // Prepare only; use the existing owner recordAndPublish. Failure preserves output
    // and publication. Only the selected channel receives new storage/revisions.
    // Each iteration reads a stable source, filters interiors, then optionally updates
    // all aprons. There is NO preupdate: the first pass observes stored apron values.
    // Smooth: float1/3/4 (vector extension), (p.x*center+six neighbors)/(p.x+6)+p.y.
    // Grow/Noise/Cut: R32 only. ExpandByte: R8. ExpandColor: RGBA8, output alpha byte 1.
    // Stencil operations require apron>=1 for nonempty, nonzero work. Apron widths>1
    // and vector boundaries are supported. Unused/inactive slots and padding become 0.
    // All parameter/boundary components must be finite. See the accompanying .md for
    // exact equations, conversion limits, NaN-data policy and legacy corrections.
    // >4096 iterations returns FE_UNSUPPORTED. Zero iterations or empty publication
    // returns a one-use no-op ticket, with no GPU allocation or revision changes.
    nvrhi::FRESULT prepare(const VoxelFilterDesc&, const PreparationBudget&,
                            std::unique_ptr<PreparedGeneration>& output) noexcept;
 private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    explicit NvrhiVoxelFilters(std::unique_ptr<Impl>) noexcept;
};
} // namespace gvdb::portable
