#include "fx/PlanetSet.h"
#include "app/GpuProfiler.h"
#include "fx/FxCommon.h"
#include "fx/RectPass.h"
#include "scene/Lights.h"

#include <donut/core/json.h>
#include <donut/core/log.h>
#include <donut/core/vfs/VFS.h>
#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/FramebufferFactory.h>
#include <donut/engine/SceneTypes.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/TextureCache.h>
#include <donut/engine/View.h>

#include <json/json.h>

#include <algorithm>
#include <cmath>
#include <cstddef>

using namespace donut::math;
#include <space_cb.h>

using namespace donut::engine;

namespace fx
{
    namespace
    {
        // 2018 helper 0x1401EDC30 (out, azimuth, elevation, radius), angles in degrees.
        float3 SphericalToCartesianDegrees(float azimuth, float elevation, float radius)
        {
            const float az = radians(azimuth);
            const float el = radians(elevation);
            return float3(cosf(az) * cosf(el), sinf(el), sinf(az) * cosf(el)) * radius;
        }

        nvrhi::ITexture* TextureOrFallback(const std::shared_ptr<LoadedTexture>& texture, nvrhi::ITexture* fallback)
        {
            return (texture && texture->texture) ? texture->texture.Get() : fallback;
        }
    }

    PlanetSet::PlanetSet() = default;
    PlanetSet::~PlanetSet() = default;

    void PlanetSet::LoadPlanet(Planet& planet, const Json::Value& node, TextureCache& textureCache,
        const std::filesystem::path& mediaPath, ThreadPool* threadPool)
    {
        planet.azimuth = node["Azimuth"].asFloat();
        planet.elevation = node["Elevation"].asFloat();
        planet.angularSize = node["AngularSize"].asFloat();
        planet.radiusRatio = node["RadiusRatio"].asFloat();
        planet.atmosphericAlpha = node["AtmosphericAlpha"].asFloat();
        planet.litBrightness = node["LitBrightness"].asFloat();
        planet.ambientBrightness = node["AmbientBrightness"].asFloat();
        planet.atmosphereBrightness = node["AtmosphereBrightness"].asFloat();
        planet.rotation = node["Rotation"].asFloat();
        planet.stretchAmount = node["StretchAmount"].asFloat();
        planet.innerRingRadius = node["InnerRingRadius"].asFloat();
        planet.atmosphereRingsRatio = node["AtmosphereRingsRatio"].asFloat();
        planet.ringsElevation = node["RingsElevation"].asFloat();
        planet.ringsAzimuth = node["RingsAzimuth"].asFloat();

        if (planet.angularSize == 0.f)
            planet.angularSize = 30.f;

        auto load = [&](const char* key, bool sRGB) -> std::shared_ptr<LoadedTexture>
        {
            const std::filesystem::path path = mediaPath / node[key].asString();
            const TextureLoadOptions options{ SRGBModeFromBool(sRGB) };
            // deviation: see PlanetSet::Load (task group -> ThreadPool).
            if (threadPool)
                return textureCache.LoadTextureFromFileAsync(path, options, *threadPool);
            return textureCache.LoadTextureFromFileDeferred(path, options);
        };

        planet.surfaceMap = load("SurfaceMap", true);
        planet.normalMap = load("NormalMap", false);
        planet.ringsPattern = load("RingsPattern", false);
    }

    bool PlanetSet::Load(
        donut::vfs::IFileSystem& fs,
        const std::filesystem::path& jsonFileName,
        TextureCache& textureCache,
        const std::filesystem::path& mediaPath,
        ThreadPool* threadPool)
    {
        Json::Value root;
        if (!donut::json::LoadFromFile(fs, jsonFileName, root))
            return false;

        try
        {
            m_Planets.resize(root.size());

            size_t index = 0;
            for (const Json::Value& node : root)
                LoadPlanet(m_Planets[index++], node, textureCache, mediaPath, threadPool);
        }
        catch (const Json::Exception&)
        {
            return false;
        }

        return true;
    }

    void PlanetSet::CreateRenderPasses(
        nvrhi::IDevice* device,
        const std::shared_ptr<ShaderFactory>& shaderFactory,
        const std::shared_ptr<FramebufferFactory>& framebufferFactory,
        const ICompositeView& compositeView,
        const std::shared_ptr<CommonRenderPasses>& commonPasses)
    {
        m_PlanetConstants = device->createBuffer(ConstantBufferDesc(sizeof(PlanetConstants), "PlanetConstants"));

        // b0 PlanetConstants, t0 Surface, t1 Normals, t2 Rings, s0 SurfaceSampler
        nvrhi::BindingLayoutDesc pixelLayoutDesc;
        pixelLayoutDesc.bindings = {
            nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
            nvrhi::BindingLayoutItem::Texture_SRV(0),
            nvrhi::BindingLayoutItem::Texture_SRV(1),
            nvrhi::BindingLayoutItem::Texture_SRV(2),
            nvrhi::BindingLayoutItem::Sampler(0)
        };

        // Straight alpha blending over the sky (the shader outputs the atmosphere opacity in alpha).
        const nvrhi::BlendState::RenderTarget blendState = BlendStateRT(nvrhi::BlendFactor::SrcAlpha, nvrhi::BlendFactor::InvSrcAlpha);

        m_RectPass = std::make_unique<RectPass>(device, shaderFactory, "demo/PlanetFx_on_ps.hlsl", pixelLayoutDesc,
            blendState, framebufferFactory, compositeView);
        m_RingsRectPass = std::make_unique<RectPass>(device, shaderFactory, "demo/PlanetWithRingsFx_on_ps.hlsl", pixelLayoutDesc,
            blendState, framebufferFactory, compositeView);

        nvrhi::ITexture* fallback = commonPasses->m_BlackTexture;

        for (Planet& planet : m_Planets)
        {
            nvrhi::BindingSetDesc bindingSetDesc;
            bindingSetDesc.bindings = {
                nvrhi::BindingSetItem::ConstantBuffer(0, m_PlanetConstants),
                nvrhi::BindingSetItem::Texture_SRV(0, TextureOrFallback(planet.surfaceMap, fallback)),
                nvrhi::BindingSetItem::Texture_SRV(1, TextureOrFallback(planet.normalMap, fallback)),
                nvrhi::BindingSetItem::Texture_SRV(2, TextureOrFallback(planet.ringsPattern, fallback)),
                // unresolved: the 2018 sampler is CommonRenderPasses+304; by the member order of the
                // 2018 class (black 2D array texture at +272) this is the linear wrap sampler.
                nvrhi::BindingSetItem::Sampler(0, commonPasses->m_LinearWrapSampler)
            };

            planet.bindingSet = device->createBindingSet(bindingSetDesc, m_RectPass->GetPixelBindingLayout());
            planet.ringsBindingSet = device->createBindingSet(bindingSetDesc, m_RingsRectPass->GetPixelBindingLayout());
        }
    }

    void PlanetSet::Render(
        nvrhi::ICommandList* commandList,
        const ICompositeView& compositeView,
        const SceneDirectionalLight& sun,
        float distance,
        float brightness,
        bool withRings) const
    {
        demo::ProfBegin(commandList, "PlanetFx");

        const float3 directionToSun = -normalize(sun.direction);
        const float3 sunColor = sun.color * sun.irradiance;

        for (const Planet& planet : m_Planets)
        {
            for (uint32_t viewIndex = 0; viewIndex < compositeView.GetNumChildViews(ViewType::PLANAR); viewIndex++)
            {
                const IView* view = compositeView.GetChildView(ViewType::PLANAR, viewIndex);

                const float4x4 projection = view->GetProjectionMatrix(false);
                const nvrhi::Rect extent = view->GetViewExtent();
                const int width = extent.maxX - extent.minX;
                const int height = extent.maxY - extent.minY;

                const float3 planetDirection = SphericalToCartesianDegrees(planet.azimuth, planet.elevation, 1.f);

                // Texture LOD from the on-screen diameter of the planet in pixels.
                const float halfAngle = radians(std::min(30.f, planet.angularSize * 0.5f));
                const float tanTerm = tanf(halfAngle) * 2.f;
                const float pixelSize = std::max(
                    fabsf(projection[0][0]) * tanTerm * float(width),
                    fabsf(projection[1][1]) * tanTerm * float(height));

                float textureLOD = 0.f;
                if (planet.surfaceMap && planet.surfaceMap->texture)
                {
                    const nvrhi::TextureDesc& desc = planet.surfaceMap->texture->getDesc();
                    textureLOD = std::min(std::max(0.f, std::log2(float(desc.width) / pixelSize)), float(desc.mipLevels) - 1.f);
                }

                // The atmosphere brightens as the planet approaches the sun.
                const float sunAngle = std::max(0.f, acosf(dot(planetDirection, directionToSun)) - radians(planet.angularSize));

                const float planetRadius = sinf(halfAngle);

                PlanetConstants constants{};
                constants.directionToSun = directionToSun;
                constants.radiusRatio = planet.radiusRatio;
                constants.planetPosition = planetDirection;
                constants.atmosphericRadius = planetRadius;
                constants.atmosphericAlpha = planet.atmosphericAlpha;
                constants.rotation = planet.rotation;
                constants.textureLOD = textureLOD;
                constants.stretchAmount = planet.stretchAmount;
                constants.sunColor = sunColor;
                constants.litBrightness = brightness * planet.litBrightness;
                constants.ambientBrightness = brightness * planet.ambientBrightness;
                constants.atmosphereBrightness = (expf(sunAngle * -4.f) * 9.f + 1.f) * (brightness * planet.atmosphereBrightness);

                if (withRings)
                {
                    // Outer ring radius r: tan(asin(r)) = planetRadius / AtmosphereRingsRatio.
                    const float ringHalfAngle = atanf(planetRadius / planet.atmosphereRingsRatio);
                    const float outerRadius = sinf(ringHalfAngle);
                    const float innerRadius = (outerRadius - planetRadius) * planet.innerRingRadius + planetRadius;
                    constants.outerRingRadiusSquared = outerRadius * outerRadius;
                    constants.innerRingRadiusSquared = innerRadius * innerRadius;

                    const float3 ringNormal = SphericalToCartesianDegrees(planet.ringsAzimuth, planet.ringsElevation, 1.f);
                    constants.ringsPlane = float4(ringNormal, -dot(ringNormal, planetDirection));

                    commandList->writeBuffer(m_PlanetConstants, &constants, sizeof(constants));

                    // The quad must cover the rings: twice the ring half-angle.
                    m_RingsRectPass->Render(commandList, *view, planetDirection, degrees(ringHalfAngle) * 2.f, distance, planet.ringsBindingSet);
                }
                else
                {
                    commandList->writeBuffer(m_PlanetConstants, &constants, sizeof(constants));

                    m_RectPass->Render(commandList, *view, planetDirection, planet.angularSize, distance, planet.bindingSet);
                }
            }
        }

        demo::ProfEnd(commandList);
    }

    float PlanetSet::GetSunVisibility(const float3& directionToSun, float sunAngularSize) const
    {
        float visibility = 1.f;

        for (const Planet& planet : m_Planets)
        {
            const float3 planetDirection = SphericalToCartesianDegrees(planet.azimuth, planet.elevation, 1.f);
            const float angle = degrees(acosf(dot(planetDirection, directionToSun)));

            // 0 when the sun center is inside the planet disk shrunk by the sun radius, 1 when the
            // sun is a full sun diameter further out.
            const float t = std::min(std::max(0.f, (angle - (planet.angularSize - sunAngularSize) * 0.5f) / sunAngularSize), 1.f);
            visibility *= (1.f - cosf(t * PI_f)) * 0.5f;
        }

        return visibility;
    }
}
