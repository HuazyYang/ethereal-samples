#pragma once

#include <donut/core/math/math.h>
#include <nvrhi/nvrhi.h>

#include <filesystem>
#include <memory>
#include <vector>

class SceneDirectionalLight;

namespace Json
{
    class Value;
}

namespace donut::vfs
{
    class IFileSystem;
}

namespace donut::engine
{
    class ShaderFactory;
    class FramebufferFactory;
    class CommonRenderPasses;
    class ICompositeView;
    class TextureCache;
    class ThreadPool;
    struct LoadedTexture;
}

namespace fx
{
    class RectPass;

    // One entry of media/PlanetFx/planets.json (2018: 120-byte record, filled by 0x1400081F0).
    // Angles are in degrees. Keys that are absent read as 0 (jsoncpp null -> asFloat).
    struct Planet
    {
        nvrhi::AutoPtr<donut::engine::LoadedTexture> surfaceMap;     // +0   "SurfaceMap" (sRGB)
        nvrhi::AutoPtr<donut::engine::LoadedTexture> normalMap;      // +16  "NormalMap"
        nvrhi::AutoPtr<donut::engine::LoadedTexture> ringsPattern;   // +32  "RingsPattern"
        nvrhi::BindingSetHandle bindingSet;                           // +48  for PlanetFx_on_ps
        nvrhi::BindingSetHandle ringsBindingSet;                      // +56  for PlanetWithRingsFx_on_ps

        float azimuth = 0.f;                // +64  "Azimuth"      direction of the planet center
        float elevation = 0.f;              // +68  "Elevation"
        float angularSize = 30.f;           // +72  "AngularSize"  full angular diameter; 0 means 30
        float radiusRatio = 0.f;            // +76  "RadiusRatio"  planet radius / atmosphere radius (>= 1: no atmosphere)
        float atmosphericAlpha = 0.f;       // +80  "AtmosphericAlpha"
        float litBrightness = 0.f;          // +84  "LitBrightness"
        float ambientBrightness = 0.f;      // +88  "AmbientBrightness"
        float atmosphereBrightness = 0.f;   // +92  "AtmosphereBrightness"
        float rotation = 0.f;               // +96  "Rotation"     added to the longitude texcoord
        float stretchAmount = 0.f;          // +100 "StretchAmount"
        float innerRingRadius = 0.f;        // +104 "InnerRingRadius"  0 = rings start at the planet surface, 1 = at the outer radius
        float atmosphereRingsRatio = 1.f;   // +108 "AtmosphereRingsRatio"  tan(ring half-angle) = sin(planet half-angle) / ratio
        float ringsElevation = 0.f;         // +112 "RingsElevation"  ring plane normal
        float ringsAzimuth = 0.f;           // +116 "RingsAzimuth"
    };

    // Asteroids.exe: PlanetSet (ctor 0x140006C60, CreateRenderPasses 0x140007910, Load 0x140008740,
    // LoadPlanet 0x1400081F0, Render 0x140008870, GetSunVisibility 0x140008070; no vtable,
    // 48-byte object {PlanetConstants buffer, RectPass, vector<Planet>, RectPass (rings)}, FeatureDemo+1160)
    //
    // Background planets: each planet is drawn on a RectPass quad around its direction, with an
    // O'Neil atmosphere (demo/PlanetFx_on_ps.hlsl) and optional rings (demo/PlanetWithRingsFx_on_ps.hlsl).
    // The planets sit at unit distance from the (camera-relative) eye; their radius is
    // sin(angularSize / 2), so they keep their angular size at any camera position.
    class PlanetSet
    {
    public:
        PlanetSet();
        ~PlanetSet();

        // FeatureDemo::LoadScene: (root fs (+136), media/PlanetFx/planets.json, textureCache (+24),
        // media path (+152), the scene-load task group).
        // deviation: the 2018 code passed a concurrency task group to the deferred texture loads.
        // donut main loads asynchronously through a ThreadPool; with threadPool == nullptr the
        // textures are decoded synchronously and uploaded deferred (TextureCache::LoadTextureFromFileDeferred).
        bool Load(
            donut::vfs::IFileSystem& fs,
            const std::filesystem::path& jsonFileName,
            donut::engine::TextureCache& textureCache,
            const std::filesystem::path& mediaPath,
            donut::engine::ThreadPool* threadPool);

        // FeatureDemo::CreateRenderPasses: (device, shaderFactory (+120), render targets' framebuffer
        // factory (targets+120), view (+608), commonPasses (+48)).
        // Creates both pipelines and the per-planet binding sets (textures must have been loaded).
        void CreateRenderPasses(
            nvrhi::IDevice* device,
            const nvrhi::AutoPtr<donut::engine::ShaderFactory>& shaderFactory,
            const nvrhi::AutoPtr<donut::engine::FramebufferFactory>& framebufferFactory,
            const donut::engine::ICompositeView& compositeView,
            const nvrhi::AutoPtr<donut::engine::CommonRenderPasses>& commonPasses);

        // FeatureDemo::RenderLightingAndEffects: (commandList (+696), view (+608), sun (+984),
        // g_MaxSceneDistance, 1.0f, UIData+368 (draw rings)).
        void Render(
            nvrhi::ICommandList* commandList,
            const donut::engine::ICompositeView& compositeView,
            const SceneDirectionalLight& sun,
            float distance,
            float brightness,
            bool withRings) const;

        // Fraction of the sun disk that is not hidden by the planets (product over the planets of a
        // smooth cosine falloff over the sun's angular size). FeatureDemo::RenderScene sets
        // sun.irradiance = GetSunVisibility(-sun.direction, sun.angularSize) * UIData+288.
        [[nodiscard]] float GetSunVisibility(const donut::math::float3& directionToSun, float sunAngularSize) const;

        [[nodiscard]] const std::vector<Planet>& GetPlanets() const { return m_Planets; }

    private:
        void LoadPlanet(Planet& planet, const Json::Value& node, donut::engine::TextureCache& textureCache,
            const std::filesystem::path& mediaPath, donut::engine::ThreadPool* threadPool);

        nvrhi::BufferHandle m_PlanetConstants;
        std::unique_ptr<RectPass> m_RectPass;
        std::vector<Planet> m_Planets;
        std::unique_ptr<RectPass> m_RingsRectPass;
    };
}
