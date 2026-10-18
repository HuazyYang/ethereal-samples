// Explicit NVRHI resource orchestration for the portable SPH kernels.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "slang/GVDBPortable.h"
#include <nvrhi/nvrhi.h>
#include <vector>
namespace ethereal::slang { class ComputeRuntime; }
namespace gvdb::slang {
class SlangSphSimulation {
    ethereal::slang::ComputeRuntime* m_runtime;
    SphParameters m_parameters;
    uint32_t m_count, m_cellCount;
    nvrhi::BufferHandle m_positions,m_velocities,m_evaluated;
    nvrhi::BufferHandle m_nextPositions,m_nextVelocities,m_nextEvaluated;
    nvrhi::BufferHandle m_densities,m_forces,m_pointCells,m_pointOffsets;
    nvrhi::BufferHandle m_cellCounts,m_cellOffsets,m_indices;
public:
    // Runtime lifetime must cover this simulation. Positions and velocities are
    // float4 streams; .w of positions is preserved as caller-owned particle data.
    SlangSphSimulation(ethereal::slang::ComputeRuntime& runtime,SphParameters parameters,
        const std::vector<Float4>& positions,const std::vector<Float4>& velocities = {});
    const SphParameters& parameters() const { return m_parameters; }
    nvrhi::IBuffer* positions() const { return m_positions; }
    nvrhi::IBuffer* velocities() const { return m_velocities; }
    nvrhi::IBuffer* densities() const { return m_densities; }
    nvrhi::IBuffer* forces() const { return m_forces; }
    // Enqueues complete steps. Flush the shared runtime before external GPU use.
    void step(uint32_t count = 1);
    std::vector<Float4> readPositions();
    std::vector<Float4> readVelocities();
};
} // namespace gvdb::slang
