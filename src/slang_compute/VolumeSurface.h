// Opt-in level-set surface rendering with sparse background and packed color.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ComputeRuntime.h"
#include "../gvdb_samples/gvdb/GVDBSlangVolume.h"
namespace gvdb::sample {
// Scalar samples use trilinear interpolation across leaf interiors. Missing
// voxels use scalarBackground, independently of stored apron contents. Normals
// follow the positive scalar gradient (positive-outside level-set convention).
// Optional RGBA8 color uses point sampling at the hit, matching point-fusion.
// Its raw buffer must cover the scalar atlas in exactly the same slot layout.
// A null color atlas, or a hit in missing space, uses the transfer color.
slang::RaycastOutput renderVolumeSurface(ethereal::slang::ComputeRuntime& runtime,
    slang::SlangVolume& volume,const slang::RaycastParameters& parameters,
    const std::vector<slang::Float4>& transfer,nvrhi::IBuffer* packedColorAtlas,float scalarBackground);
} // namespace gvdb::sample
