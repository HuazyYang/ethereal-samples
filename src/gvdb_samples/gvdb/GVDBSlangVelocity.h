// Sparse MAC velocity interpolation and PIC/FLIP transfer through NVRHI.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "GVDBSlangVolume.h"
#include <array>
namespace gvdb::slang {
struct VelocityTransfer {
    nvrhi::BufferHandle positions,velocities,sampledGrid;
};
// Grid channels are scalar structured buffers with the volume's atlas layout,
// and at least one updated apron voxel. Positions/velocities have float4 stride.
// Out-of-volume particles are preserved. This is grid-to-particle transfer and
// optional midpoint advection, not a pressure solver or particle-to-grid scatter.
VelocityTransfer transferGridVelocity(ethereal::slang::ComputeRuntime& runtime,const SlangVolume& volume,
    const std::array<nvrhi::IBuffer*,3>& current,const std::array<nvrhi::IBuffer*,3>& previous,
    nvrhi::IBuffer* positions,nvrhi::IBuffer* velocities,uint32_t count,float timeStep,float flipWeight,bool advect);
} // namespace gvdb::slang
