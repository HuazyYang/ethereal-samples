// Original spray shading modes on the portable volume representation.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "SpraySimulation.h"
namespace gvdb::samples::spray {
enum class Shading : uint32_t {
    Off,
    Voxel,
    Surface,
    Section,
    Volume
};
std::vector<slang::Float4> sprayTransfer(Shading);
slang::RaycastOutput renderSpray(ethereal::slang::ComputeRuntime&, SprayVolume&, slang::RaycastParameters,
                                 Shading, const std::vector<slang::Float4>&);
}  // namespace gvdb::samples::spray
