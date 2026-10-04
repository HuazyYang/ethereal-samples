// Asteroids.exe: FeatureDemo per-frame rendering.
//   RenderScene 0x1400342A0, RenderSplashScreen 0x1400354A0, SceneUnloading 0x140037020,
//   SetupViewsAndTargets 0x140031EF0, UpdateViews 0x140037C50, UpdateLights 0x140031980,
//   CollectLightProbes 0x1400377C0, UpdateScene 0x140038380, UpdateViewDistanceMap 0x1400384F0,
//   UpdateAsteroidRendererSettings 0x140037370, RenderShadows 0x140034F70, RenderGBuffer 0x140032630,
//   RenderLightingAndEffects 0x140033CA0, ResolveOrAccumulate 0x14002B2A0, PostProcess 0x140033960,
//   RenderOverlay 0x1400321F0.
//
// Rendering is camera-relative, as in 2018: the views are built from the camera's translated-world matrix and
// m_ViewOrigin (FeatureDemo+640) = -cameraPosition is the pre-view translation applied to the scene.

#include "app/AppGlobals.h"
#include "app/GpuProfiler.h"
#include "passes/SharpenBlitPass2018.h"
#include "passes/TemporalAAPass2018.h"
#include "app/DemoLightProbe.h"
#include "app/FeatureDemo.h"
#include "app/RenderHandedness.h"
#include "app/RenderTargets.h"
#include "app/ThirdPersonCamera.h"
#include "app/UIData.h"

#include "fx/EnvironmentMapPass.h"
#include "fx/FogPass.h"
#include "fx/HqBlitPass.h"
#include "fx/LensFlarePass.h"
#include "fx/ParticleSystem.h"
#include "fx/PlanetSet.h"
#include "fx/RealStarField.h"
#include "fx/ShieldPass.h"
#include "fx/ShowCubemapPass.h"
#include "fx/SunDisk.h"
#include "meshlets/MeshletDrawStrategy.h"
#include "meshlets/PipelineStatisticsQuery.h"
#include "passes/BloomPass2018.h"
#include "passes/DeferredLightingPass2018.h"
#include "passes/ForwardShadingPass2018.h"
#include "passes/GBufferFillPass2018.h"
#include "passes/LightProbeProcessingPass2018.h"
#include "passes/ToneMappingPass2018.h"
#include "passes/HbaoPlusPass.h"
#include "scene/AsteroidLibrary.h"
#include "scene/CargoShip.h"
#include "scene/Lights.h"
#include "scene/PlayerShip.h"
#include "scene/SpaceObject.h"
#include "scene/SpaceScene.h"

#include <donut/core/log.h>
#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/FramebufferFactory.h>
#include <donut/engine/SceneGraph.h>
#include <donut/engine/SceneTypes.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/TextureCache.h>
#include <donut/engine/View.h>
#include <donut/render/CascadedShadowMap.h>
#include <donut/render/TemporalAntiAliasingPass.h>
#include <donut/shaders/light_types.h>

#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

using namespace donut;
using namespace donut::math;

namespace
{
    int64_t NowNanoseconds()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    float ElapsedMs(int64_t start)
    {
        return float(double(NowNanoseconds() - start) * 1e-6);
    }

    // Exponential average used for every timing statistic (0.05 new, 0.95 old).
    void Accumulate(float& average, float sample)
    {
        average = sample * 0.05f + average * 0.95f;
    }

    // 0x1401EDC30: direction from azimuth / elevation in degrees.
    float3 SphericalDirection(float azimuthDegrees, float elevationDegrees)
    {
        const float azimuth = radians(azimuthDegrees);
        const float elevation = radians(elevationDegrees);
        return float3(cosf(azimuth) * cosf(elevation), sinf(elevation), sinf(azimuth) * cosf(elevation));
    }

    // 0x1401EE160 applied to a row vector: rotation of 'v' about 'axis' by 'angle' radians.
    float3 RotateAboutAxis(const float3& v, const float3& axis, float angle)
    {
        const float s = sinf(angle);
        const float c = cosf(angle);
        const float t = 1.f - c;
        const float x = axis.x, y = axis.y, z = axis.z;
        const float3 r0(x * x * t + c, x * y * t + z * s, x * z * t - y * s);
        const float3 r1(x * y * t - z * s, y * y * t + c, y * z * t + x * s);
        const float3 r2(x * z * t + y * s, y * z * t - x * s, z * z * t + c);
        return r0 * v.x + r1 * v.y + r2 * v.z;
    }

    // 0x14003C720: Abramowitz-Stegun approximation of erf.
    float ApproximateErf(float x)
    {
        const float a = fabsf(x);
        const float p = 1.f + a * 0.278393f + a * a * 0.230389f + a * a * a * 0.000972f + a * a * a * a * 0.078108f;
        const float result = 1.f - 1.f / ((p * p) * (p * p));
        return x < 0.f ? -result : result;
    }

    // uniform_real_distribution<float>(0, 1) over the global engine (0x1400226F0).
    float RandomUnit()
    {
        return std::uniform_real_distribution<float>(0.f, 1.f)(demo::g_RandomEngine);
    }

    nvrhi::RenderState MakeShadowRenderState()
    {
        nvrhi::RenderState state;
        state.depthStencilState.depthTestEnable = true;
        state.depthStencilState.depthWriteEnable = true;
        state.depthStencilState.depthFunc = nvrhi::ComparisonFunc::LessOrEqual;
        state.depthStencilState.stencilEnable = false;
        state.rasterState.cullMode = nvrhi::RasterCullMode::Back;
        state.rasterState.frontCounterClockwise = true;
        return state;
    }
}

// =====================================================================================================
// RenderSplashScreen / SceneUnloading
// =====================================================================================================

void FeatureDemo::RenderSplashScreen(nvrhi::IFramebuffer* framebuffer)
{
    nvrhi::ITexture* backBuffer = framebuffer->getDesc().colorAttachments[0].texture;

    m_CommandList->open();
    // deviation: the 2018 nvrhi needed beginTrackingTextureState(backBuffer, RenderTarget) here; donut's
    // nvrhi tracks the swap chain textures itself.
    m_CommandList->clearTextureFloat(backBuffer, nvrhi::AllSubresources, nvrhi::Color(0.f));

    if (m_CommonPasses)
    {
        if (!m_SplashTexture)
        {
            // Synchronous load and upload on the splash command list (sRGB).
            m_SplashTexture = m_TextureCache->LoadTextureFromFile(m_MediaPath / "splash.jpg",
                engine::TextureLoadOptions{ engine::SRGBModeFromBool(true) }, m_CommonPasses.Get(), m_CommandList);

            if (!m_SplashTexture || !m_SplashTexture->texture)
                log::error("Error reading splashscreen");
        }

        if (m_SplashTexture && m_SplashTexture->texture)
        {
            const nvrhi::TextureDesc& textureDesc = m_SplashTexture->texture->getDesc();

            int windowWidth = 0, windowHeight = 0;
            GetDeviceManager()->GetWindowDimensions(windowWidth, windowHeight);

            // Fit the image into the window, keeping its aspect ratio, centered.
            const float imageWidth = float(textureDesc.width);
            const float imageHeight = float(textureDesc.height);
            const float scale = std::min(float(windowHeight) / imageHeight, float(windowWidth) / imageWidth);
            const float halfWidth = imageWidth * scale * 0.5f;
            const float halfHeight = imageHeight * scale * 0.5f;
            const float centerX = float(windowWidth) * 0.5f;
            const float centerY = float(windowHeight) * 0.5f;

            engine::BlitParameters blit;
            blit.targetFramebuffer = framebuffer;
            blit.targetViewport = nvrhi::Viewport(centerX - halfWidth, centerX + halfWidth,
                centerY - halfHeight, centerY + halfHeight, 0.f, 1.f);
            blit.sourceTexture = m_SplashTexture->texture;
            m_CommonPasses->BlitTexture(m_CommandList, blit);
        }
    }

    m_CommandList->close();
    GetDevice()->executeCommandList(m_CommandList);

    GetDeviceManager()->SetVsyncEnabled(true);
}

void FeatureDemo::SceneUnloading()
{
    if (m_ForwardPass)
        m_ForwardPass->ResetBindingCache();
    if (m_DeferredLightingPass)
        m_DeferredLightingPass->ResetBindingCache();
    if (m_LightProbePass)
        m_LightProbePass->ResetCaches();

    m_SunLight.reset();

    // deviation: the binary cleared the chase camera's input maps and history in place and dropped the
    // (leaked) pointer; here the owned camera is destroyed.
    m_ShipCamera.reset();
    m_ActiveCamera = &m_FirstPersonCamera;
    m_UI->cameraMode = 0;

    m_OpaqueDrawStrategy = nullptr;
    m_TransparentDrawStrategy = nullptr;
    m_GBufferRenderer = nullptr;
    m_DepthRenderer = nullptr;
    m_MaterialIdRenderer = nullptr;
    m_ForwardRenderer = nullptr;
    m_ShipForwardRenderer = nullptr;

    m_UI->pickedMaterial = nullptr;
}

// =====================================================================================================
// Views and targets
// =====================================================================================================

void FeatureDemo::SetupViewsAndTargets(uint32_t width, uint32_t height, nvrhi::IFramebuffer* framebuffer,
    bool& exposureResetRequired)
{
    nvrhi::IDevice* device = GetDevice();
    bool targetsRecreated = false;

    if (!m_RenderTargets || m_RenderTargets->IsUpdateRequired(uint2(width, height)))
    {
        // deviation: the 2018 CommonRenderPasses cached blit binding sets and was reset here; donut's has no
        // cache of its own.

        // The cached meshlet binding sets reference the old depth and Hi-Z textures (0x14005C260).
        AsteroidLibrary& library = m_Scene->GetAsteroidLibrary();
        for (uint32_t typeId = 0; typeId < library.GetNumTypes(); typeId++)
        {
            if (auto type = library.GetAsteroidType(typeId))
                type->GetMeshletBindingSet() = nullptr;
        }
        // deviation: the binary only reset the asteroid types; the space-object sets bind the Hi-Z texture too.
        for (const auto& object : m_Scene->GetObjects())
        {
            if (object)
                object->GetMeshletBindingSet() = nullptr;
        }

        if (m_SharpenBlitPass)
            m_SharpenBlitPass->ResetCaches();   // 2018: commonPasses->ResetBindingCache() (0x14007B6A0)

        m_RenderTargets.reset();
        m_RenderTargets = std::make_unique<RenderTargets>(device, uint2(width, height));
        m_PreviousViewsValid = false;
        targetsRecreated = true;
    }

    bool recreatePasses = UpdateViews() || targetsRecreated;

    if (m_UI->reloadShaders)
    {
        m_ShaderFactory->ClearCache();
        for (const auto& renderer : { m_GBufferRenderer, m_ShipForwardRenderer, m_DepthRenderer, m_MaterialIdRenderer })
        {
            if (renderer)
                renderer->CreateShaders();
        }
        m_UI->reloadShaders = false;
        recreatePasses = true;
    }

    if (recreatePasses)
    {
        CreateRenderPasses(exposureResetRequired, framebuffer);
        device->waitForIdle();
        device->runGarbageCollection();
    }
}

bool FeatureDemo::UpdateViews()
{
    // The camera mode selects the active camera (0 = free camera, 1 = chase camera).
    if (m_UI->cameraMode == 0)
        m_ActiveCamera = &m_FirstPersonCamera;
    else if (m_UI->cameraMode == 1 && m_ShipCamera)
        m_ActiveCamera = m_ShipCamera.get();

    float2 pixelOffset = 0.f;
    if (!m_UI->wireframe && m_UI->aaMode == AntiAliasingMode::TemporalAA && m_TaaPass2018)
        pixelOffset = m_TaaPass2018->GetCurrentPixelOffset();

    const uint2 renderSize = m_RenderTargets->GetSize();
    int windowWidth = 0, windowHeight = 0;
    GetDeviceManager()->GetWindowDimensions(windowWidth, windowHeight);

    bool created = false;
    if (!m_View)
    {
        m_View = MAKE_RC_OBJ_PTR(engine::PlanarView);
        m_ViewPrevious = MAKE_RC_OBJ_PTR(engine::PlanarView);
        created = true;
    }

    // 0x1401EDDD0: D3D-style perspective, near 1, far g_SceneFarDistance, aspect of the window.
    // The view is Donut's left-handed camera matrix as is; the 2018 content's handedness is
    // reconciled in the projection (see app/RenderHandedness.h).
    const float4x4 projection = demo::MirrorProjectionX(perspProjD3DStyle(radians(m_UI->verticalFov),
        float(windowWidth) / float(std::max(windowHeight, 1)), 1.f, demo::g_SceneFarDistance));

    m_View->SetViewport(nvrhi::Viewport(float(renderSize.x), float(renderSize.y)));
    m_View->SetPixelOffset(pixelOffset);
    m_View->SetMatrices(m_ActiveCamera->GetTranslatedWorldToViewMatrix(), projection);
    m_View->UpdateCache();

    m_ViewOrigin = -m_ActiveCamera->GetPosition();
    if (created)
    {
        m_ViewOriginPrevious = m_ViewOrigin;
        // deviation: the binary left the new previous view uninitialized for the first frame.
        // PlanarView is reference counted and not copy-assignable, so the three pieces of
        // state set above are copied explicitly.
        m_ViewPrevious->SetViewport(m_View->GetViewport());
        m_ViewPrevious->SetPixelOffset(m_View->GetPixelOffset());
        m_ViewPrevious->SetMatrices(m_View->GetViewMatrix(), m_View->GetProjectionMatrix(false));
        m_ViewPrevious->UpdateCache();
    }

    return created;
}

// =====================================================================================================
// Lights
// =====================================================================================================

void FeatureDemo::UpdateSun()
{
    // Inlined in RenderScene: the sun turns about the solar axis (Environment sliders).
    const float3 axis = SphericalDirection(m_UI->solarAxisAzimuth, m_UI->solarAxisElevation);
    const float3 start = SphericalDirection(m_UI->solarAxisAzimuth - 90.f, 0.f);
    const float3 direction = RotateAboutAxis(start, axis, radians(m_UI->sunRotation));

    m_SunLight->direction = -direction;
    if (m_SunLight->direction.y > -0.00001f)
        m_SunLight->direction.y = -0.00001f;

    // Planets in front of the sun dim it.
    const float visibility = m_Planets ? m_Planets->GetSunVisibility(-m_SunLight->direction, m_SunLight->angularSize) : 1.f;
    m_SunLight->irradiance = visibility * m_UI->planetLightScale;
}

void FeatureDemo::UpdateLights()
{
    // Point and spot lights are copied and moved into translated (camera-relative) space; directional lights
    // are shared.
    m_Lights.clear();
    for (const auto& light : m_Scene->GetLights())
    {
        if (!light)
            continue;

        if (light->GetLightType() == LightType_Point)
        {
            auto copy = std::make_shared<ScenePointLight>(*std::static_pointer_cast<ScenePointLight>(light));
            copy->position += m_ViewOrigin;
            m_Lights.push_back(copy);
        }
        else if (light->GetLightType() == LightType_Spot)
        {
            auto copy = std::make_shared<SceneSpotLight>(*std::static_pointer_cast<SceneSpotLight>(light));
            copy->position += m_ViewOrigin;
            m_Lights.push_back(copy);
        }
        else
        {
            m_Lights.push_back(light);
        }
    }
}

void FeatureDemo::CollectLightProbes(std::vector<nvrhi::AutoPtr<engine::LightProbe>>& probes)
{
    probes.clear();
    if (!m_UI->useLightProbeAmbient || m_LightProbes.empty())
        return;

    const float sunIrradiance = m_SunLight ? m_SunLight->irradiance : 1.f;

    if (m_UI->useProbeHeightBands)
    {
        const float extent = m_Scene->GetWorldDiagonal();
        const float originY = m_ViewOrigin.y;

        for (const auto& probe : m_LightProbes)
        {
            probe->diffuseScale = sunIrradiance * m_UI->sunDiffuseScale;
            probe->specularScale = sunIrradiance * m_UI->sunSpecularScale;

            // Horizontal planes at the band limits (translated space), softened over ~2000 units.
            const box3 band(float3(-extent, probe->minHeight + originY, -extent),
                float3(extent, probe->maxHeight + 2000.f + originY, extent));
            frustum bounds = frustum::fromBox(band);
            for (int planeIndex : { int(frustum::TOP_PLANE), int(frustum::BOTTOM_PLANE) })
            {
                bounds.planes[planeIndex].normal *= 0.0005f;
                bounds.planes[planeIndex].distance *= 0.0005f;
            }
            probe->bounds = bounds;
            probes.push_back(probe);
        }
    }
    else
    {
        const auto& probe = m_LightProbes.front();
        probe->bounds = frustum::infinite();
        probe->diffuseScale = sunIrradiance * m_UI->sunDiffuseScale;
        probe->specularScale = sunIrradiance * m_UI->sunSpecularScale;
        probes.push_back(probe);
    }
}

// =====================================================================================================
// Scene state
// =====================================================================================================

void FeatureDemo::UpdateScene(nvrhi::ICommandList* commandList)
{
    for (const auto& object : m_Scene->GetObjects())
    {
        if (object)
            object->UpdateBounds();
    }

    if (!m_Scene->AreRenderingResourcesCreated())
        m_Scene->CreateResources(GetDevice());

    m_Scene->Update(commandList);
    m_Scene->GetAsteroidLibrary().UpdateObjectConstants(commandList);
    m_Scene->UpdateBounds();
}

void FeatureDemo::UpdateViewDistanceMap(nvrhi::ICommandList* commandList, float cameraHeight, float farDistance)
{
    // For each view direction (y from -1 to 1) the distance at which the fog transmittance of the height layer
    // drops below the visibility threshold; the asteroid shaders fade out beyond it.
    const fx::FogParameters& fog = m_UI->fog;
    const uint32_t count = m_ViewDistanceMap->getDesc().width;
    std::vector<float> distances(count);

    for (uint32_t index = 0; index < count; index++)
    {
        const float t = float(index) / float(count - 1) * 2.f - 1.f;
        const float inverse = 1.f / t;
        const float layerDistance = (fog.meanHeight - cameraHeight) * inverse;
        const float layerThickness = fog.thickness * inverse;
        const float k = t / fog.thickness;
        const float scale = fog.densityScale * -1.2533141f * layerThickness;

        const float start = ApproximateErf((0.f - layerDistance) * 0.70710677f * k);
        auto transmittance = [&](float distance)
        {
            return expf((ApproximateErf((distance - layerDistance) * 0.70710677f * k) - start) * scale);
        };

        float value = farDistance;
        if (transmittance(farDistance) < fog.visibilityThreshold)
        {
            float lo = 0.f;
            float hi = farDistance;
            for (int iteration = 0; iteration < 12; iteration++)
            {
                value = (lo + hi) * 0.5f;
                if (transmittance(value) >= fog.visibilityThreshold)
                    lo = value;
                else
                    hi = value;
            }
        }
        distances[index] = value;
    }

    commandList->writeTexture(m_ViewDistanceMap, 0, 0, distances.data(), count * sizeof(float));
}

void FeatureDemo::UpdateAsteroidRendererSettings()
{
    const float lodAdjustment = (1.f - m_UI->lodScale) * 9.f;

    for (const auto& renderer : { m_GBufferRenderer, m_DepthRenderer, m_MaterialIdRenderer, m_ForwardRenderer, m_ShipForwardRenderer })
    {
        if (!renderer)
            continue;
        renderer->enableLod = m_UI->dynamicLod;
        renderer->enableAsteroidsCulling = m_UI->asteroidsCulling;
        renderer->enableDistanceLod = m_UI->distanceLod;
        renderer->alphaPreset = uint32_t(m_UI->alphaPreset);
        renderer->forcedLod = uint32_t(m_UI->staticLodIndex);
        renderer->zCullSectorThreshold = uint32_t(m_UI->zCullSectorThreshold);
        renderer->transitionRange = m_UI->lodTransitionRange;
        renderer->lodBias = lodAdjustment + m_UI->cameraLodBias;
        renderer->lodSlope = m_UI->lodScale;
        renderer->visualizeLods = m_UI->visualizeLods ? 1u : 0u;
        renderer->lodView = m_View.Get();
        renderer->preViewTranslation = m_ViewOrigin;
        renderer->preViewTranslationPrevious = m_ViewOriginPrevious;
        renderer->time = m_CurrentTime;
    }

    if (m_GBufferRenderer)
    {
        m_GBufferRenderer->enableZCull = m_UI->hiZCulling;
        m_GBufferRenderer->enableZCullStats = m_UI->readbackCullingStats;
        m_GBufferRenderer->minAsteroidScreenSize = m_UI->minAsteroidScreenSize;
        m_GBufferRenderer->wireframe = m_UI->wireframe;
    }
    if (m_MaterialIdRenderer)
    {
        m_MaterialIdRenderer->wireframe = m_UI->wireframe;
        m_MaterialIdRenderer->transitionRange = 0.f;
    }
    if (m_DepthRenderer)
    {
        m_DepthRenderer->transitionRange = 0.f;
        m_DepthRenderer->lodBias = lodAdjustment + m_UI->shadowLodBias;
    }
    if (m_ShipForwardRenderer)
    {
        m_ShipForwardRenderer->wireframe = true;
        m_ShipForwardRenderer->showBBoxes = m_UI->showShipBoundingBoxes;
    }
    for (const auto& renderer : { m_GBufferRenderer, m_ShipForwardRenderer })
    {
        if (!renderer)
            continue;
        renderer->drawAsteroids = m_UI->drawAsteroids;
        renderer->drawPlayerShip = m_UI->drawSpaceObjects;
        renderer->drawOtherObjects = m_UI->drawSpaceObjects;
    }
}

// =====================================================================================================
// Passes
// =====================================================================================================

void FeatureDemo::RenderShadowView(const engine::IView& view)
{
    MeshletPassState state;
    state.framebuffer = m_ShadowFramebuffer->GetFramebuffer(view);
    state.viewport = view.GetViewportState();
    state.renderState = MakeShadowRenderState();

    m_DepthRenderer->Render(m_CommandList, state, view, [](SceneMaterial*, MeshletPassState&) { return true; });
}

void FeatureDemo::RenderShadows()
{
    if (!m_ShadowLightGraph)
    {
        m_ShadowLightGraph = MAKE_RC_OBJ_PTR(engine::SceneGraph);
        auto node = MAKE_RC_OBJ_PTR(engine::SceneGraphNode);
        m_ShadowLightGraph->SetRootNode(node);
        m_ShadowLight = MAKE_RC_OBJ_PTR(engine::DirectionalLight);
        node->SetLeaf(m_ShadowLight);
    }
    if (!m_ShadowFramebuffer || m_ShadowFramebuffer->DepthTarget != m_ShadowMap->GetTexture())
    {
        m_ShadowFramebuffer = MAKE_RC_OBJ_PTR(engine::FramebufferFactory, GetDevice());
        m_ShadowFramebuffer->DepthTarget = m_ShadowMap->GetTexture();
    }

    m_SunLight->shadowMap = m_ShadowMap;

    // 2018 (0x1400ADB00): the cascade light basis is lookatZ(direction, up = (0,0,1)) (up = (1,0,0) for a
    // near-vertical light). donut main's Light::SetDirection uses the one-argument lookatZ, whose orthogonal()
    // picks a different roll whenever |dir.x| <= |dir.z|, which rotates the shadow texel grid relative to the
    // original. Set the node rotation to the 2018 basis directly (the node translation is ignored by donut).
    {
        const double3 lightDir = normalize(double3(m_SunLight->direction));
        const double3 lightUp = (abs(lightDir.x) < 1e-6 && abs(lightDir.y) < 1e-6) ? double3(1.0, 0.0, 0.0)
                                                                                   : double3(0.0, 0.0, 1.0);
        const daffine3 localToParent = inverse(dm::lookatZ(lightDir, lightUp));
        dquat rotation;
        double3 scaling;
        decomposeAffine<double>(localToParent, nullptr, &rotation, &scaling);
        m_ShadowLight->GetNode()->SetTransform(nullptr, &rotation, &scaling);
    }
    m_ShadowLight->angularSize = m_SunLight->angularSize;
    m_ShadowLightGraph->Refresh(GetDeviceManager()->GetFrameIndex());

    const float3 direction = normalize(m_SunLight->direction);
    const float depthRange = m_UI->shadowDepthRange;
    const float lightSpaceZUp = (2.f - direction.y) * depthRange;

    if (m_UI->shadowFitToView)
    {
        m_ShadowMap->SetupForPlanarViewStable(*m_ShadowLight, m_View->GetProjectionFrustum(),
            m_View->GetInverseViewMatrix(), m_UI->shadowDistance, lightSpaceZUp, depthRange, m_UI->cascadeExponent,
            m_ViewOrigin);
    }
    else
    {
        m_ShadowMap->SetupForPlanarView(*m_ShadowLight, m_View->GetViewFrustum(), m_UI->shadowDistance,
            lightSpaceZUp, depthRange, m_UI->cascadeExponent, m_ViewOrigin);
    }

    // Per-object shadow of the cargo ship (translated bounds).
    if (CargoShip* cargoShip = m_Scene->GetCargoShip())
    {
        if (SpaceObject* shipObject = cargoShip->GetSpaceObject())
        {
            const box3& bounds = shipObject->GetBounds();
            m_ShadowMap->SetupPerObjectShadow(*m_ShadowLight, 0, box3(bounds.m_mins + m_ViewOrigin, bounds.m_maxs + m_ViewOrigin));

            // deviation (donut workaround): 2018 (0x1400AE290) fitted the ortho projection to the object's light-space
            // depth range with the right-handed 0x1401EDCE0, so the object spans depth [0, 1]. donut's
            // SetupWholeSceneDirectionalLightView passes the same (-maxZ, -minZ) to the left-handed orthoProjD3DStyle,
            // which puts the whole object in front of the near plane (all depths clamp to 0). Remap
            // z' = (z + maxZ) / (maxZ - minZ) to (z - minZ) / (maxZ - minZ), the direction the cascades use.
            if (m_ShadowMap->GetNumberOfPerObjectShadows() > 0)
            {
                auto objectView = m_ShadowMap->GetPerObjectView(0);
                float4x4 projection = objectView->GetProjectionMatrix(false);
                projection[3].z = 1.f - projection[3].z;
                objectView->SetMatrices(objectView->GetViewMatrix(), projection);
                objectView->UpdateCache();
            }
        }
    }

    // 0x1400814F0(true) and 0x140081440(0.5f): per-cascade lit-out-of-bounds flag (+49) and falloff distance (+76).
    m_ShadowMap->SetLitOutOfBounds(true);
    m_ShadowMap->SetFalloffDistance(0.5f);

    const int64_t start = NowNanoseconds();
    m_PipelineStatsQuery->Begin(m_CommandList, 0);
    demo::ProfBegin(m_CommandList, "Shadows");

    if (m_DepthRenderer)
    {
        // Cascades: asteroids and the other objects.
        m_DepthRenderer->drawAsteroids = true;
        m_DepthRenderer->drawPlayerShip = false;
        m_DepthRenderer->drawOtherObjects = true;
        m_ShadowMap->Clear(m_CommandList);
        for (uint32_t cascade = 0; cascade < m_ShadowMap->GetNumberOfCascades(); cascade++)
            RenderShadowView(*m_ShadowMap->GetCascadeView(cascade));

        // Per-object shadow: the player ship only.
        m_DepthRenderer->drawAsteroids = false;
        m_DepthRenderer->drawPlayerShip = true;
        m_DepthRenderer->drawOtherObjects = false;
        if (m_ShadowMap->GetNumberOfPerObjectShadows() > 0)
        {
            auto objectView = m_ShadowMap->GetPerObjectView(0);
            m_CommandList->clearDepthStencilTexture(m_ShadowMap->GetTexture(), objectView->GetSubresources(),
                true, 1.f, false, 0);
            RenderShadowView(*objectView);
        }
    }

    demo::ProfEnd(m_CommandList);
    m_PipelineStatsQuery->End(m_CommandList, 0);
    Accumulate(m_UI->shadowTimeMs, ElapsedMs(start));
}

void FeatureDemo::RenderGBuffer()
{
    const int64_t start = NowNanoseconds();
    m_PipelineStatsQuery->Begin(m_CommandList, 1);
    m_GBufferPass->Render(m_CommandList, *m_GBufferRenderer, *m_View, m_ViewPrevious.Get());
    m_PipelineStatsQuery->End(m_CommandList, 1);
    Accumulate(m_UI->gbufferTimeMs, ElapsedMs(start));
}

void FeatureDemo::RenderLightingAndEffects()
{
    std::vector<nvrhi::AutoPtr<engine::LightProbe>> probes;
    CollectLightProbes(probes);

    float3 ambient = 0.f;
    if (!m_UI->useLightProbeAmbient)
        ambient = m_UI->ambientColor * m_SunLight->irradiance * 0.01f;

    RandomUnit();
    const float randomY = RandomUnit() * 32767.f;
    const float randomX = RandomUnit() * 32767.f;
    m_DeferredLightingPass->Render(m_CommandList, *m_View, m_Lights, m_GBufferBindingSet, float2(randomX, randomY),
        ambient, ambient, probes, nullptr);

    if (m_EnvironmentMapPass)
        m_EnvironmentMapPass->Render(m_CommandList, *m_View, m_UI->nebulaBrightness, 0.f, 0.f);

    if (m_UI->enableRealStars && m_RealStarFieldPass)
        m_RealStarFieldPass->Render(m_CommandList, *m_View, m_UI->realStarBrightness);

    m_SunDisk->Render(m_CommandList, *m_View, *m_SunLight, demo::g_SceneFarDistance, 1.f);
    m_Planets->Render(m_CommandList, *m_View, *m_SunLight, demo::g_SceneFarDistance, 1.f, m_UI->planetFlag368);

    if (m_UI->enableHbao && m_HbaoPlus)
    {
        m_HbaoPlus->Render(m_CommandList, m_UI->hbao, *m_View, m_RenderTargets->DepthBuffer, m_RenderTargets->GBuffer2,
            m_RenderTargets->HdrColor);
    }

    if (m_UI->enableFog)
    {
        const float r0 = RandomUnit() * 32767.f;
        const float r1 = RandomUnit() * 32767.f;
        const float r2 = RandomUnit() * 32767.f;
        m_FogPass->Render(m_CommandList, *m_View, m_ViewOrigin, *m_SunLight, m_UI->fog, float3(r2, r1, r0),
            m_UI->shadowDistance);
    }

    if (m_UI->enableParticles)
    {
        if (m_UI->resetParticles)
        {
            m_Particles->Reset(m_CommandList);
            m_UI->resetParticles = false;
        }
        m_Particles->Update(m_CommandList, m_CurrentTime, m_ElapsedTime);
    }

    if (m_UI->drawSpaceObjects && m_ForwardRenderer)
        m_ForwardPass->Render(m_CommandList, *m_ForwardRenderer, *m_View, m_Lights, ambient, ambient, probes);

    if (m_UI->showShipBoundingBoxes && m_ShipForwardRenderer)
        m_ForwardPass->Render(m_CommandList, *m_ShipForwardRenderer, *m_View, m_Lights, ambient, ambient, probes);

    if (m_UI->enableShield)
    {
        PlayerShip* ship = m_Scene->GetPlayerShip();
        const float3 shipPosition = ship->GetPosition() + m_ViewOrigin;
        m_ShieldPass->Render(m_CommandList, *m_View, shipPosition, m_CollisionNormal, 30.f, m_CollisionIntensity,
            m_CurrentTime);
    }
}

void FeatureDemo::ResolveOrAccumulate(const nvrhi::Viewport& viewport)
{
    // 2018 ResolveOrAccumulate (0x14002B2A0): with history the motion vectors are rendered and the frame is
    // resolved into the TAA output texture (ResolvedColor1/2 swap every frame); without history the HDR image is
    // copied into the output instead.
    TemporalAAPass2018::Parameters params;
    params.newFrameWeight = m_UI->taaNewFrameWeight;
    params.clampingFactor = m_UI->taaClampingFactor;
    params.enableHistoryClamping = m_UI->taaEnableHistoryClamping;

    if (m_PreviousViewsValid)
    {
        // deviation: donut's pass renders the motion vectors. Camera-relative views:
        // translated(prev) = translated(current) - (origin - originPrevious).
        m_TemporalAntiAliasingPass->RenderMotionVectors(m_CommandList, *m_View, *m_ViewPrevious,
            m_ViewOrigin - m_ViewOriginPrevious);
    }
    m_TaaPass2018->Resolve(m_CommandList, params, m_PreviousViewsValid, *m_View, *m_View);

    // The post-processing chain works in BloomColor.
    engine::BlitParameters blit;
    blit.targetFramebuffer = m_RenderTargets->BloomFramebuffer->GetFramebuffer(nvrhi::AllSubresources);
    blit.targetViewport = viewport;
    blit.sourceTexture = m_TaaPass2018->GetOutput();
    m_CommonPasses->BlitTexture(m_CommandList, blit);
}

nvrhi::ITexture* FeatureDemo::PostProcess(const nvrhi::AutoPtr<engine::FramebufferFactory>& framebufferWithDepth,
    const nvrhi::AutoPtr<engine::FramebufferFactory>& framebuffer, nvrhi::ITexture* source)
{
    if (m_UI->enableParticles)
    {
        m_PipelineStatsQuery->Begin(m_CommandList, 2);
        m_Particles->Render(m_CommandList, *m_View, framebufferWithDepth, *m_SunLight, m_ViewOrigin, 5000.f,
            uint32_t(m_UI->particlesParam228), m_UI->particlesParam226);
        m_PipelineStatsQuery->End(m_CommandList, 2);
    }

    if (m_UI->enableBloom)
    {
        const float sigma = float(m_RenderTargets->GetSize().y) / 1080.f * m_UI->bloomSigma;
        // 0x140090420; the composite weight (blend factor 0.05) is part of the 2018 pass.
        m_BloomPass->Render(m_CommandList, framebuffer, *m_View, source, sigma);
    }

    if (m_UI->enableLensFlare)
        m_LensFlarePass->Render(m_CommandList, *m_View, framebuffer, *m_SunLight, m_ViewOrigin);

    nvrhi::ITexture* result = source;
    if (m_UI->enableToneMapping)
    {
        // 0x140094A50: histogram, exposure adaptation and the tone mapping draw.
        m_ToneMappingPass->SimpleRender(m_CommandList, m_UI->toneMapping, *m_View, source);
        result = m_RenderTargets->LdrColor;
    }
    return result;
}

void FeatureDemo::RenderOverlay(const nvrhi::Viewport& windowViewport, nvrhi::IFramebuffer* framebuffer)
{
    // +1264 material picking (+904 / +912) is never enabled in the binary.

    // Shadow map slices along the bottom of the window.
    if (m_UI->drawDebugOverlay)
    {
        const uint32_t numSlices = m_ShadowMap->GetNumberOfCascades() + m_ShadowMap->GetNumberOfPerObjectShadows();
        for (uint32_t slice = 0; slice < numSlices; slice++)
        {
            engine::BlitParameters blit;
            blit.targetFramebuffer = framebuffer;
            blit.targetViewport = nvrhi::Viewport(float(slice) * 266.f + 10.f, float(slice + 1) * 266.f,
                windowViewport.maxY - 266.f, windowViewport.maxY - 10.f, 0.f, 1.f);
            blit.sourceTexture = m_ShadowMap->GetTexture();
            blit.sourceArraySlice = slice;
            m_CommonPasses->BlitTexture(m_CommandList, blit);
        }
    }

    // Specular cubemap of the selected light probe.
    if (m_UI->showLightProbe && m_UI->lightProbeIndex >= 0 && size_t(m_UI->lightProbeIndex) < m_LightProbes.size())
    {
        const auto& probe = m_LightProbes[size_t(m_UI->lightProbeIndex)];
        if (probe->specularMap)
        {
            if (!m_ShowCubemapPass)
                m_ShowCubemapPass = std::make_shared<fx::ShowCubemapPass>(GetDevice(), m_ShaderFactory, m_CommonPasses,
                    framebuffer, probe->specularMap);
            m_ShowCubemapPass->Render(m_CommandList, framebuffer, nvrhi::Viewport(10.f, 266.f,
                windowViewport.maxY - 266.f, windowViewport.maxY - 10.f, 0.f, 1.f), probe->specularArrayIndex, 0);
        }
    }
}

// =====================================================================================================
// RenderScene
// =====================================================================================================

void FeatureDemo::RenderScene(nvrhi::IFramebuffer* framebuffer)
{
    nvrhi::IDevice* device = GetDevice();
    ++demo::g_ReplayFrameIndex;

    int windowWidth = 0, windowHeight = 0;
    GetDeviceManager()->GetWindowDimensions(windowWidth, windowHeight);
    const nvrhi::Viewport windowViewport{ float(windowWidth), float(windowHeight) };

    uint32_t renderWidth = uint32_t(windowWidth);
    uint32_t renderHeight = uint32_t(windowHeight);
    if (m_UI->aaMode == AntiAliasingMode::TemporalUpscale)
    {
        // Render at 1/1.5 of the window size, aligned up to 8 pixels.
        renderWidth = (uint32_t(windowWidth * 2 / 3) + 7) & ~7u;
        renderHeight = (uint32_t(windowHeight * 2 / 3) + 7) & ~7u;
    }
    const nvrhi::Viewport renderViewport{ float(renderWidth), float(renderHeight) };

    bool exposureResetRequired = false;
    SetupViewsAndTargets(renderWidth, renderHeight, framebuffer, exposureResetRequired);

    const int64_t frameStart = NowNanoseconds();
    m_PipelineStatsQuery->ReadResults();

    demo::GpuProfiler::Get().BeginFrame();
    m_CommandList->open();
    demo::ProfBegin(m_CommandList, "Frame");
    m_Scene->Animate(m_CommandList);

    nvrhi::ITexture* backBuffer = framebuffer->getDesc().colorAttachments[0].texture;
    m_CommandList->clearTextureFloat(backBuffer, nvrhi::AllSubresources, nvrhi::Color(0.f));

    UpdateSun();
    UpdateLights();

    if (exposureResetRequired)
        m_ToneMappingPass->ResetExposure(m_CommandList, 0.05f);

    UpdateScene(m_CommandList);
    UpdateViewDistanceMap(m_CommandList, m_View->GetViewOrigin().y - m_ViewOrigin.y, demo::g_SceneFarDistance);

    MeshletDrawStrategy::ResetFrameStatistics();
    UpdateAsteroidRendererSettings();

    if (m_UI->enableShadows)
    {
        RenderShadows();
    }
    else
    {
        m_SunLight->shadowMap = nullptr;
        m_UI->shadowTimeMs = 0.f;
    }

    m_RenderTargets->Clear(m_CommandList);
    RenderGBuffer();

    // Statistics for the keynote counters.
    m_UI->statsMeshletCount = MeshletDrawStrategy::GetNumMeshletDraws();
    m_UI->asteroidsDrawnWithShadows = MeshletDrawStrategy::GetNumAsteroidInstances();
    m_UI->maxLodTrianglesWithShadows = MeshletDrawStrategy::GetNumAsteroidPrimitives();
    m_UI->statsRendererValue = m_GBufferRenderer->numSectorsDrawn;
    m_UI->asteroidsDrawn = m_GBufferRenderer->numAsteroidInstances;
    m_UI->maxLodTriangles = m_GBufferRenderer->numAsteroidPrimitives;
    {
        const auto& results = m_PipelineStatsQuery->GetResults();
        uint64_t triangles = results.size() > 1 ? results[1].CInvocations : 0;
        if (m_UI->enableParticles && results.size() > 2)
            triangles += results[2].CInvocations;
        m_UI->drawnTriangles = triangles;
    }

    RenderLightingAndEffects();

    nvrhi::AutoPtr<engine::FramebufferFactory> postFramebuffer;
    nvrhi::AutoPtr<engine::FramebufferFactory> postFramebufferWithDepth;
    nvrhi::ITexture* postSource = nullptr;

    if (m_UI->wireframe || m_UI->aaMode == AntiAliasingMode::None || int(m_UI->aaMode) > 3)
    {
        postSource = m_RenderTargets->HdrColor;
        postFramebuffer = m_RenderTargets->HdrFramebufferNoDepth;
        postFramebufferWithDepth = m_RenderTargets->HdrFramebuffer;
        m_PreviousViewsValid = false;
    }
    else
    {
        // deviation: aaMode 2 (accumulation) is not selectable and takes the temporal path here.
        ResolveOrAccumulate(renderViewport);
        postSource = m_RenderTargets->BloomColor;
        postFramebuffer = m_RenderTargets->BloomFramebuffer;
        postFramebufferWithDepth = m_RenderTargets->BloomFramebufferWithDepth;
        m_PreviousViewsValid = true;
    }

    nvrhi::ITexture* result = PostProcess(postFramebufferWithDepth, postFramebuffer, postSource);

    if (m_UI->aaMode == AntiAliasingMode::TemporalUpscale)
    {
        m_HqBlitPass->Render(m_CommandList, framebuffer, windowViewport, result);
    }
    else if (m_UI->sharpness > 0.f && m_UI->aaMode != AntiAliasingMode::None && m_SharpenBlitPass)
    {
        // 2018: commonPasses->BlitWithSharpening(cmd, framebuffer, viewport, sharpness, result, 0, 0) (0x14007AAB0)
        m_SharpenBlitPass->Render(m_CommandList, framebuffer, windowViewport, result, m_UI->sharpness);
    }
    else
    {
        // 2018: commonPasses->BlitTexture (0x14007B0A0)
        engine::BlitParameters blit;
        blit.targetFramebuffer = framebuffer;
        blit.targetViewport = windowViewport;
        blit.sourceTexture = result;
        m_CommonPasses->BlitTexture(m_CommandList, blit);
    }

    RenderOverlay(windowViewport, framebuffer);

    demo::ProfEnd(m_CommandList);
    m_PipelineStatsQuery->Resolve(m_CommandList);
    m_CommandList->close();
    device->executeCommandList(m_CommandList);

    Accumulate(m_UI->frameTimeMs, ElapsedMs(frameStart));

    if (m_UI->readbackCullingStats && m_GBufferRenderer)
    {
        const std::vector<uint32_t>& stats = m_GBufferRenderer->ReadStatsReadback();
        if (!stats.empty())
        {
            char buffer[64];
            snprintf(buffer, sizeof(buffer), "numCulled = %d\n", int(stats[0]));
            OutputDebugStringA(buffer);
        }
    }

    if (m_TemporalAntiAliasingPass)
        m_TemporalAntiAliasingPass->AdvanceFrame();
    if (m_TaaPass2018)
        m_TaaPass2018->AdvanceFrame();

    std::swap(m_View, m_ViewPrevious);
    m_ViewOriginPrevious = m_ViewOrigin;

    GetDeviceManager()->SetVsyncEnabled(m_UI->enableVsync);

    if (demo::g_PrintLoadingTime)
    {
        demo::g_PrintLoadingTime = false;
        m_FirstFrameTimestamp = NowNanoseconds();

        char buffer[256];
        snprintf(buffer, sizeof(buffer), "Loading time: %.3f s\n",
            double(m_FirstFrameTimestamp - m_ConstructionTimestamp) * 1e-9);
        OutputDebugStringA(buffer);
        log::info("%s", buffer);
    }

    ++m_RenderedFrames;
    SaveScreenshotIfRequested(framebuffer);

    // Developer option (deviation): "-perfLog" prints the average frame time every 1000 rendered frames.
    if ((demo::g_Options.perfLog || demo::g_Options.gpuProfile) && m_RenderedFrames % 1000 == 0)
    {
        const auto now = std::chrono::steady_clock::now();
        if (m_RenderedFrames > 1000)
        {
            const double ms = std::chrono::duration<double, std::milli>(now - m_PerfLogStart).count() / 1000.0;
            log::info("perf: frames %u, %.2f ms/frame (%.1f fps)", m_RenderedFrames, ms, 1000.0 / ms);
        }
        // "-gpuProfile": GPU milliseconds per pass over the last 1000 frames (see app/GpuProfiler.h).
        const std::string report = demo::GpuProfiler::Get().TakeReport();
        if (!report.empty())
            log::info("gpu passes (avg ms/frame, frames up to %u):\n%s", m_RenderedFrames, report.c_str());
        m_PerfLogStart = now;
    }
}

void FeatureDemo::SaveScreenshotIfRequested(nvrhi::IFramebuffer* framebuffer)
{
    // Developer option (deviation, not in the binary): "-screenshot <file.png> <frame>".
    if (m_ScreenshotPath.empty() || m_RenderedFrames < m_ScreenshotFrame
        || m_RenderedFrames >= m_ScreenshotFrame + m_ScreenshotCount)
        return;

    nvrhi::ITexture* backBuffer = framebuffer->getDesc().colorAttachments[0].texture;
    // "-screenshotCount N" (N > 1): the frames are saved as <stem>_<frame><extension>.
    std::filesystem::path outputPath = m_ScreenshotPath;
    if (m_ScreenshotCount > 1)
        outputPath = m_ScreenshotPath.parent_path() / (m_ScreenshotPath.stem().string() + "_" + std::to_string(m_RenderedFrames)
            + m_ScreenshotPath.extension().string());
    const std::string fileName = outputPath.string();
    if (engine::SaveTextureToFile(GetDevice(), m_CommonPasses.Get(), backBuffer, nvrhi::ResourceStates::Present,
        fileName.c_str(), false))
        log::info("Saved screenshot to %s (frame %u)", fileName.c_str(), m_RenderedFrames);
    else
        log::warning("Failed to save screenshot to %s", fileName.c_str());

    // Developer option (deviation): "-dumpGBuffer <prefix>" also saves the G-buffer targets of the same frame
    // (<prefix>_gb0/_gb1/_gb2.png) to compare shader variants.
    if (!demo::g_Options.dumpGBufferPrefix.empty() && m_RenderTargets)
    {
        const std::string prefix(demo::g_Options.dumpGBufferPrefix.begin(), demo::g_Options.dumpGBufferPrefix.end());
        nvrhi::ITexture* targets[] = { m_RenderTargets->GBuffer0, m_RenderTargets->GBuffer1, m_RenderTargets->GBuffer2 };
        for (int i = 0; i < 3; ++i)
        {
            const std::string path = prefix + "_gb" + std::to_string(i) + ".png";
            if (targets[i])
                engine::SaveTextureToFile(GetDevice(), m_CommonPasses.Get(), targets[i],
                    nvrhi::ResourceStates::ShaderResource, path.c_str(), false);
        }
    }

    if (m_RenderedFrames + 1 < m_ScreenshotFrame + m_ScreenshotCount)
        return;     // more frames to save

    m_ScreenshotPath.clear();
    glfwSetWindowShouldClose(GetDeviceManager()->GetWindow(), GLFW_TRUE);
}
