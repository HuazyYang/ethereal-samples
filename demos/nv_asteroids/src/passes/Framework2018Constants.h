#pragma once

// C++ view of the constant buffers of the 2018 framework post-processing / probe passes. The structs are defined
// once, in shaders/framework/framework_passes_cb.h (HLSL + C++, namespace framework2018); the byte layouts were
// also checked against the Asteroids.exe writeBuffer call sites:
//   LightProbeConstants   16 bytes  0x14008BA80 (zeros), 0x14008C7F0 (diffuse), 0x14008D460 (specular)
//   ToneMappingConstants  80 bytes  0x140093DC0 (histogram), 0x140094A50 (exposure), 0x1400942C0 (tone mapping)
//   BloomConstants        32 bytes  0x140090420 ("BloomConstantsH" / "BloomConstantsV"); numSamples at offset 16

#include <donut/core/math/math.h>

// The shared header uses the HLSL type names (uint, uint2, float2, float3) unqualified inside namespace
// framework2018; the using-directive below applies to that namespace only.
namespace framework2018
{
    using namespace donut::math;
}

#include "../../shaders/framework/framework_passes_cb.h"
