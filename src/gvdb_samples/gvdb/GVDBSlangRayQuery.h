// Arbitrary index-space rays through a sparse scalar volume using NVRHI.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "GVDBSlangVolume.h"
namespace gvdb::slang {
struct RayQueryOptions {
    float step = .25f, threshold = .3f, epsilon = .01f, bias = -.0001f;
    uint32_t maxLeafSteps = 256, maxLeafVisits = 65536;
};
struct RayQueryOutput {
    // xyz: index-space point (1e10 on miss), w: 1 hit, 0 miss/invalid ray,
    // -1 iteration limit. Normals are the normalized negative scalar gradient.
    nvrhi::BufferHandle hits, normals;
    uint32_t count = 0;
};
// Inputs are StructuredBuffer<float4>. Directions are used without normalization,
// so the caller determines parameter-space step/bias units. Volume aprons must be current.
RayQueryOutput queryVolumeRays(ethereal::slang::ComputeRuntime&, const SlangVolume&, nvrhi::IBuffer* origins,
                               nvrhi::IBuffer* directions, uint32_t count, RayQueryOptions = {});
}  // namespace gvdb::slang
