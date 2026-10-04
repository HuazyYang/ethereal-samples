#pragma once

// Helpers shared by the space-FX render passes (src/fx).
//
// Constant-buffer layouts come from the reconstructed shader headers (asteroids/shaders/demo/include,
// on the asteroids_core include path). The 2018 LightConstants / ShadowConstants live in namespace
// light2018 (light_cb.h), distinct from donut main's structs of the same names.
//
// Like donut's passes, an fx .cpp includes the shader headers after `using namespace donut::math;`
// (e.g. `#include <space_cb.h>`); this header only forward-declares the structs.

#include <donut/core/math/math.h>
#include <nvrhi/nvrhi.h>

#include <cstdint>

namespace light2018
{
    struct LightConstants;
    struct ShadowConstants;
}
class SceneLight;

namespace donut::engine
{
    class IShadowMap;
}

namespace fx
{
    // Fills the 2018 LightConstants of a light (2018: Light vtable slot 1, FillLightConstants).
    void FillLightConstants2018(const SceneLight& light, light2018::LightConstants& out);

    // Fills light.shadowCascades and shadows[] from the light's shadow map, as the 2018 passes do:
    // cascade i (while i < maxShadows) goes to shadows[i] and shadowCascades[i] = i.
    // perObjectShadows are appended after the cascades when 'includePerObject' is set (lens flare).
    // Returns the number of ShadowConstants written.
    uint32_t FillShadowConstants2018(const donut::engine::IShadowMap* shadowMap, light2018::LightConstants& light,
        light2018::ShadowConstants* shadows, uint32_t maxShadows, bool includePerObject);

    // A volatile constant buffer description in the donut style (2018 helper 0x14000C200
    // took {byteSize, debugName, isVolatile}).
    nvrhi::BufferDesc ConstantBufferDesc(uint32_t byteSize, const char* debugName, bool isVolatile = true);

    // Premultiplied-alpha blending (2018 BlendState helper 0x14000BD90 with (One, InvSrcAlpha)):
    // color = src + dst * (1 - srcAlpha), alpha = src.
    nvrhi::BlendState::RenderTarget BlendStateRT(nvrhi::BlendFactor srcBlend, nvrhi::BlendFactor destBlend);
}
