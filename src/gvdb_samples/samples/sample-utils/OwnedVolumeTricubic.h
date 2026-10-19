// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <gvdb/GVDBNvrhiApi.h>
#include <gvdb/GVDBSlangVolume.h>
namespace gvdb::sample {
struct TricubicOptions {
    float fineStep = .1f, epsilon = .01f, shadowAmount = .8f, shadowBias = 1;
    uint32_t maxLeafSteps = 4096, maxLeafVisits = 65536;
    bool borderZero = true;  // VBX loader uses zero border; false selects atlas-wide clamp.
};
// Immediate sample renderer of retained raw channels. Flush prior producers first.
// Density >= isovalue, white or point RGBA8 color, ambient .1/diffuse .9, trilinear
// shadows. Uses stored atlas bytes, including aprons and neighboring packed slots.
// march.x is direct step; march.y/z clip ray distance. direction00.w must be zero.
// Negative output depth signals traversal exhaustion; positive infinity is a miss.
// No color channel: UINT32_MAX. Empty publications return background without reads.
slang::RaycastOutput renderOwnedTricubic(ethereal::slang::ComputeRuntime&, portable::SnapshotHandle,
                                         uint32_t scalarChannel, uint32_t colorChannel,
                                         slang::RaycastParameters, TricubicOptions = {});
}  // namespace gvdb::sample
