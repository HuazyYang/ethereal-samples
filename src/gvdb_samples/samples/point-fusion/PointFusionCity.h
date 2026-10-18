// Procedural city and scanner pose of the original point-fusion example.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "PointFusionScanner.h"
namespace gvdb::samples {
struct PointFusionCity {
    std::vector<ScanBox> boxes;
    float blockPitch = 122.2f, indexScale = 5;
    slang::Float4 minimum{0, 0, 0, 0}, maximum{6110, 0, 6110, 0};
    slang::Float4 initialEye{1222, 20, 1222, 0};
};
PointFusionCity makePointFusionCity();
// Angles retain the legacy orbit convention: yaw 180/pitch 0 looks along +Z.
ScanParameters cityScanParameters(const PointFusionCity&, slang::Float4 eye, float yawDegrees,
                                  float pitchDegrees, uint32_t width = 240, uint32_t height = 180,
                                  bool jitter = true);
}  // namespace gvdb::samples
