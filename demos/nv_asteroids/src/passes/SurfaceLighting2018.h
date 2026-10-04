#pragma once

// Light / shadow / light probe constant filling shared by ForwardShadingPass2018 and DeferredLightingPass2018.
// Asteroids.exe: inlined in ForwardShadingPass::Render (0x140085A60) and DeferredLightingPass::Render (0x14007CA30)
// of the 2018 framework; both use identical loops.

#include <donut/core/math/math.h>
#include <nvrhi/nvrhi.h>

#include <memory>
#include <vector>

class SceneLight;

namespace donut::engine
{
    struct LightProbe;
}

namespace light2018   // 2018 layouts (shaders/demo/include/light_cb.h), not donut's
{
    struct LightConstants;
    struct ShadowConstants;
    struct LightProbeConstants;
}

namespace surface_lighting
{
    using LightList = std::vector<std::shared_ptr<SceneLight>>;
    using LightProbeList = std::vector<std::shared_ptr<donut::engine::LightProbe>>;

    struct ShadowMapInfo
    {
        nvrhi::ITexture* texture = nullptr;
        dm::int2 size = 0;
    };

    // The shadow map texture of the first light that has a shadow map. The 2018 code does not check that the
    // other lights use the same texture (the shaders bind only one array).
    ShadowMapInfo FindShadowMap(const LightList& lights);

    // Fills up to 16 lights and up to 16 shadow constants (cascades first, then per-object shadows of each light,
    // in light order) and returns the number of lights. 'lights' / 'shadows' point to the 16-entry arrays.
    uint32_t FillLights(const LightList& lights, light2018::LightConstants* lightConstants, light2018::ShadowConstants* shadowConstants);

    struct LightProbeTextures
    {
        nvrhi::ITexture* diffuse = nullptr;
        nvrhi::ITexture* specular = nullptr;
        nvrhi::ITexture* environmentBrdf = nullptr;
    };

    // Collects the textures of the enabled light probes. Returns false (after logging the 2018 error
    // "All lights probe submitted to <passName>::Render(...) must use the same set of textures") when two enabled
    // probes use different textures; the 2018 Render functions then return without drawing.
    bool CollectLightProbeTextures(const LightProbeList& probes, const char* passName, LightProbeTextures& textures);

    // Fills up to 16 active probes and returns their number.
    uint32_t FillLightProbes(const LightProbeList& probes, light2018::LightProbeConstants* probeConstants);
}
