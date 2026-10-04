#include "passes/SurfaceLighting2018.h"

#include "scene/Lights.h"

#include <donut/core/log.h>
#include <donut/engine/SceneTypes.h>
#include <donut/engine/ShadowMap.h>

#include <algorithm>
#include <cstring>

using namespace donut::math;
#include <donut/shaders/light_cb.h>   // donut's ShadowConstants / LightProbeConstants (filled by donut objects)
#include "surface_cb.h"               // 2018 layouts (light2018::, surface2018::)

using namespace donut;

namespace surface_lighting
{
    // donut's IShadowMap / LightProbe fill donut's structs, which have exactly the 2018 layouts.
    static_assert(sizeof(::ShadowConstants) == sizeof(light2018::ShadowConstants), "ShadowConstants layout");
    static_assert(sizeof(::LightProbeConstants) == sizeof(light2018::LightProbeConstants), "LightProbeConstants layout");

    ShadowMapInfo FindShadowMap(const LightList& lights)
    {
        ShadowMapInfo info;
        for (const auto& light : lights)
        {
            if (light && light->shadowMap)
            {
                info.texture = light->shadowMap->GetTexture();
                info.size = light->shadowMap->GetTextureSize();
                break;
            }
        }
        return info;
    }

    uint32_t FillLights(const LightList& lights, light2018::LightConstants* lightConstants, light2018::ShadowConstants* shadowConstants)
    {
        uint32_t numLights = 0;
        uint32_t numShadows = 0;

        const size_t count = std::min<size_t>(lights.size(), SURFACE_MAX_LIGHTS);
        for (size_t index = 0; index < count; index++)
        {
            const SceneLight& light = *lights[index];
            light2018::LightConstants& constants = lightConstants[numLights];
            light.FillLightConstants(constants);   // 2018 Light vfunc1

            if (light.shadowMap)
            {
                const engine::IShadowMap& shadowMap = *light.shadowMap;

                for (uint32_t cascade = 0; cascade < shadowMap.GetNumberOfCascades(); cascade++)
                {
                    if (numShadows < SURFACE_MAX_SHADOWS)
                    {
                        ::ShadowConstants shadow = {};
                        shadowMap.GetCascade(cascade)->FillShadowConstants(shadow);
                        std::memcpy(&shadowConstants[numShadows], &shadow, sizeof(shadow));
                        constants.shadowCascades[cascade] = int(numShadows);
                        ++numShadows;
                    }
                }

                for (uint32_t object = 0; object < shadowMap.GetNumberOfPerObjectShadows(); object++)
                {
                    if (numShadows < SURFACE_MAX_SHADOWS)
                    {
                        ::ShadowConstants shadow = {};
                        shadowMap.GetPerObjectShadow(object)->FillShadowConstants(shadow);
                        std::memcpy(&shadowConstants[numShadows], &shadow, sizeof(shadow));
                        constants.perObjectShadows[object] = int(numShadows);
                        ++numShadows;
                    }
                }
            }

            ++numLights;
        }

        return numLights;
    }

    bool CollectLightProbeTextures(const LightProbeList& probes, const char* passName, LightProbeTextures& textures)
    {
        textures = LightProbeTextures();

        for (const auto& probe : probes)
        {
            if (!probe || !probe->enabled)
                continue;

            if (textures.diffuse && textures.specular && textures.environmentBrdf)
            {
                if (textures.diffuse != probe->diffuseMap || textures.specular != probe->specularMap
                    || textures.environmentBrdf != probe->environmentBrdf)
                {
                    log::error("All lights probe submitted to %s::Render(...) must use the same set of textures", passName);
                    return false;
                }
            }
            else
            {
                textures.diffuse = probe->diffuseMap;
                textures.specular = probe->specularMap;
                textures.environmentBrdf = probe->environmentBrdf;
            }
        }

        return true;
    }

    uint32_t FillLightProbes(const LightProbeList& probes, light2018::LightProbeConstants* probeConstants)
    {
        uint32_t numProbes = 0;
        for (const auto& probe : probes)
        {
            if (!probe || !probe->IsActive())
                continue;

            ::LightProbeConstants constants = {};
            probe->FillLightProbeConstants(constants);
            std::memcpy(&probeConstants[numProbes], &constants, sizeof(constants));
            if (++numProbes >= SURFACE_MAX_LIGHT_PROBES)
                break;
        }
        return numProbes;
    }
}
