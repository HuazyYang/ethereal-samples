#pragma once

// C++ view of the meshlet shader constant layouts (asteroids/shaders/demo/include/meshlet_cb.h).
// meshlet_cb.h follows the donut convention (bare HLSL type names), so it is included inside a namespace that
// imports donut::math. Include meshlet_cb.h through this header only, so the types always live in
// meshlet_shader:: (the header has an include guard).

#include <cstddef>
#include <donut/core/math/math.h>

namespace meshlet_shader
{
    using namespace donut::math;
#include "meshlet_cb.h"

    // Byte size the 2018 code uploads for ObjectConstants (everything before 'padding'); the cbuffer reports 404.
    constexpr size_t c_ObjectConstantsUploadSize = offsetof(ObjectConstants, padding);
    static_assert(c_ObjectConstantsUploadSize == 384, "2018 ObjectConstants upload size");
}
