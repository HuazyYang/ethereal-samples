// Portable host material values shared by scene parsers and the Slang RT renderer.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <donut/core/math/math.h>
namespace gvdb::sample {
// Defaults and field meanings match the retained OptixMaterialParams contract.
// GPU uploads use an explicit packed representation; do not memcpy this struct.
struct SceneMaterial {
    float lightWidth = 0;
    donut::math::float3 ambColor = donut::math::float3::zero();
    donut::math::float3 envColor = donut::math::float3::zero();
    donut::math::float3 diffColor{.6f, .7f, .7f};
    donut::math::float3 specColor{3, 3, 3};
    float specPower = 400;
    float shadowWidth = 0, shadowBias = 0;
    float reflWidth = 0;
    donut::math::float3 reflColor{1, 1, 1};
    float reflBias = 0, refrWidth = 0, refrIor = 1.2f;
    donut::math::float3 refrColor{.35f, .4f, .4f};
    float refrAmount = 10, refrOffset = 15, refrBias = 0;
};
}  // namespace gvdb::sample
