// Asteroids.exe: FeatureDemo scene loading and render-pass creation.
//   LoadScene 0x140031280, SceneLoaded 0x140036540, CreateRenderPasses 0x14002BD60,
//   light probe textures 0x14002B590, light probe objects 0x14002B920, light probe rendering 0x1400327F0.

#include "app/AppGlobals.h"
#include "passes/SharpenBlitPass2018.h"
#include "passes/TemporalAAPass2018.h"
#include "app/DemoLightProbe.h"
#include "app/FeatureDemo.h"
#include "app/RenderTargets.h"
#include "app/ThirdPersonCamera.h"
#include "app/UIData.h"

#include "audio/SoundEngine.h"
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
#include "meshlets/MeshletRenderResources.h"
#include "passes/BloomPass2018.h"
#include "passes/DeferredLightingPass2018.h"
#include "passes/ForwardShadingPass2018.h"
#include "passes/GBufferFillPass2018.h"
#include "passes/LightProbeProcessingPass2018.h"
#include "passes/ToneMappingPass2018.h"
#include "scene/CargoShip.h"
#include "scene/Lights.h"
#include "scene/PlayerShip.h"
#include "scene/SpaceObject.h"
#include "scene/SpaceScene.h"

#include <donut/core/log.h>
#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/FramebufferFactory.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/TextureCache.h>
#include <donut/engine/ThreadPool.h>
#include <donut/engine/View.h>
#include <donut/render/DrawStrategy.h>
#include <donut/render/GeometryPasses.h> // DrawItem (complete type for the draw strategies)
#include <donut/render/TemporalAntiAliasingPass.h>

#include <cmath>

using namespace donut;
using namespace donut::math;

namespace
{
    // Default sun when the scene has none (SceneLoaded): normalize(112, 127, 162), angular size 4.25 degrees.
    const float3 c_DefaultSunColor = normalize(float3(112.f, 127.f, 162.f));
    constexpr float c_DefaultSunAngularSize = 4.25f;

    // Capture resolution and formats of the light probe render (0x1400327F0).
    constexpr uint32_t c_ProbeCaptureSize = 1024;
    constexpr uint32_t c_ProbeCaptureMips = 8;
    constexpr uint32_t c_ProbeDiffuseSize = 128;
    constexpr uint32_t c_ProbeSpecularSize = 256;
    constexpr uint32_t c_ProbeSpecularMips = 8;
    constexpr float c_ProbeCaptureFarDistance = 10000.f;
}

// =====================================================================================================
// LoadScene (loading thread)
// =====================================================================================================

bool FeatureDemo::LoadScene(vfs::IFileSystem* fs, const std::filesystem::path& sceneFileName)
{
    // deviation: a donut ThreadPool replaces the 2018 concurrency::task_group.
    engine::ThreadPool threadPool;

    m_Planets = std::make_shared<fx::PlanetSet>();
    m_Planets->Load(*m_RootFs, m_MediaPath / "PlanetFx/planets.json", *m_TextureCache, m_MediaPath, &threadPool);

    const engine::TextureLoadOptions linear{ engine::SRGBModeFromBool(false) };
    m_SkyTexture = m_TextureCache->LoadTextureFromFileAsync(
        m_MediaPath / "SkyAndStars/loc00180_8_Space_Sky_2k-ExposureCorrected-16bit.dds", linear, threadPool);
    m_RandomsTexture = m_TextureCache->LoadTextureFromFileAsync(m_MediaPath / "randoms_texture.dds", linear, threadPool);

    if (!demo::g_Options.renderLightProbes)
        LoadLightProbes(threadPool);

    // The binary loads the scene through the root file system (+136), not the 'fs' argument.
    (void)fs;
    m_Scene = SpaceScene::Load(m_RootFs, sceneFileName, m_TextureCache.Get(), &threadPool);

    threadPool.WaitForTasks();
    return m_Scene != nullptr;
}

// =====================================================================================================
// SceneLoaded (main thread)
// =====================================================================================================

void FeatureDemo::SceneLoaded()
{
    nvrhi::IDevice* device = GetDevice();

    if (!m_Scene->GetColorLut().empty())
    {
        m_ColorLutTexture = m_TextureCache->LoadTextureFromFileDeferred(m_MediaPath / m_Scene->GetColorLut(),
            engine::TextureLoadOptions{ engine::SRGBModeFromBool(false) });
    }
    else
    {
        m_ColorLutTexture = MAKE_RC_OBJ_PTR(engine::LoadedTexture);
    }

    // Unused by the meshlet renderer but created by the binary (+200, +216).
    m_OpaqueDrawStrategy = MAKE_RC_OBJ_PTR(render::InstancedOpaqueDrawStrategy);
    m_TransparentDrawStrategy = MAKE_RC_OBJ_PTR(render::TransparentDrawStrategy);

    // 0x140072F60: finish the deferred texture uploads; donut's ApplicationBase::SceneLoaded does exactly that
    // (ProcessRenderingThreadCommands + LoadingFinished) and marks the scene as loaded.
    ApplicationBase::SceneLoaded();

    m_Scene->CreateRenderingResources(device, m_CommonPasses, true);

    demo::g_SceneFarDistance = std::max(m_Scene->GetWorldDiagonal(), demo::g_SceneFarDistance);
    m_PreviousViewsValid = false;

    // The sun is the scene's first directional light, or a default one added to the scene.
    m_SunLight.reset();
    for (const auto& light : m_Scene->GetLights())
    {
        if (light && light->GetLightType() == LightType_Directional)
        {
            m_SunLight = std::static_pointer_cast<SceneDirectionalLight>(light);
            break;
        }
    }
    if (!m_SunLight)
    {
        m_SunLight = std::make_shared<SceneDirectionalLight>();
        m_SunLight->color = c_DefaultSunColor;
        m_SunLight->angularSize = c_DefaultSunAngularSize;
        m_Scene->GetLights().push_back(m_SunLight);
    }

    PlayerShip* ship = m_Scene->GetPlayerShip();
    m_ShipCamera = std::make_unique<ThirdPersonCamera>(ship, 50.f, 0.f, 0.4f);
    m_ActiveCamera = m_ShipCamera.get();

    if (!m_MeshletResources)
        m_MeshletResources = std::make_shared<MeshletRenderResources>(device);

    // Light probes: three probes stacked vertically, captured at runtime with -renderLightProbes.
    if (demo::g_Options.renderLightProbes)
        CreateLightProbeTextures(3);
    CreateLightProbeObjects(3);

    if (m_LightProbes.size() >= 3)
    {
        m_LightProbes[0]->capturePosition = float3(10000.f, 5000.f, 10000.f);
        m_LightProbes[0]->minHeight = 1000.f;
        m_LightProbes[0]->maxHeight = 50000.f;
        m_LightProbes[0]->debugColor = float4(1.f, 0.f, 0.f, 0.f);

        m_LightProbes[1]->capturePosition = float3(10000.f, -1000.f, 10000.f);
        m_LightProbes[1]->minHeight = -5000.f;
        m_LightProbes[1]->maxHeight = 1000.f;
        m_LightProbes[1]->debugColor = float4(0.f, 0.f, 1.f, 0.f);

        m_LightProbes[2]->capturePosition = float3(10000.f, -9000.f, 10000.f);
        m_LightProbes[2]->minHeight = -50000.f;
        m_LightProbes[2]->maxHeight = -5000.f;
        m_LightProbes[2]->debugColor = float4(0.f, 1.f, 0.f, 0.f);
    }

    if (demo::g_Options.renderLightProbes)
    {
        RenderLightProbes();
        SaveLightProbes();
    }

    if (demo::g_Options.viewIndex < 0)
    {
        m_UI->showGui = !demo::g_Options.benchmark;
        m_UI->showCounters = true;

        // Benchmarks start with the short version of the opening sequence.
        m_ActiveSequence = demo::g_Options.benchmark ? &m_OpeningSequenceFast : &m_OpeningSequence;
        m_ActiveSequence->Restart();

        if (!m_UI->mute)
        {
            if (audio::Sound* music = m_Scene->GetAmbienceMusic())
                music->Play();
        }
    }
}

// =====================================================================================================
// Light probes
// =====================================================================================================

void FeatureDemo::CreateLightProbeTextures(uint32_t numProbes)
{
    nvrhi::IDevice* device = GetDevice();

    nvrhi::TextureDesc desc;
    desc.dimension = nvrhi::TextureDimension::TextureCubeArray;
    desc.format = nvrhi::Format::RGBA16_FLOAT;      // 2018 format 34
    desc.isRenderTarget = true;
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;

    desc.width = desc.height = c_ProbeDiffuseSize;
    desc.arraySize = 6 * numProbes;
    desc.mipLevels = 1;
    desc.debugName = "LightProbeDiffuse";
    m_LightProbeDiffuse = MAKE_RC_OBJ_PTR(engine::LoadedTexture);
    device->createTexture(desc, &m_LightProbeDiffuse->texture);

    desc.width = desc.height = c_ProbeSpecularSize;
    desc.mipLevels = c_ProbeSpecularMips;
    desc.debugName = "LightProbeSpecular";
    m_LightProbeSpecular = MAKE_RC_OBJ_PTR(engine::LoadedTexture);
    device->createTexture(desc, &m_LightProbeSpecular->texture);

    // Filled by RenderLightProbes from the LightProbeProcessingPass.
    m_EnvironmentBrdf = MAKE_RC_OBJ_PTR(engine::LoadedTexture);
}

void FeatureDemo::CreateLightProbeObjects(uint32_t numProbes)
{
    m_LightProbes.clear();

    for (uint32_t index = 0; index < numProbes; index++)
    {
        auto probe = MAKE_RC_OBJ_PTR(DemoLightProbe);
        probe->name = std::to_string(index + 1);
        probe->diffuseMap = m_LightProbeDiffuse ? m_LightProbeDiffuse->texture : nullptr;
        probe->specularMap = m_LightProbeSpecular ? m_LightProbeSpecular->texture : nullptr;
        probe->environmentBrdf = m_EnvironmentBrdf ? m_EnvironmentBrdf->texture : nullptr;
        probe->diffuseArrayIndex = index;
        probe->specularArrayIndex = index;
        probe->bounds = frustum::infinite();
        probe->enabled = true;
        m_LightProbes.push_back(probe);
    }
}

void FeatureDemo::RenderLightProbes()
{
    nvrhi::IDevice* device = GetDevice();

    if (!m_LightProbePass)
        m_LightProbePass = std::make_shared<LightProbeProcessingPass2018>(device, m_ShaderFactory, m_CommonPasses,
            c_ProbeCaptureSize, nvrhi::Format::RGBA16_FLOAT);

    nvrhi::TextureDesc cubeDesc;
    cubeDesc.width = cubeDesc.height = c_ProbeCaptureSize;
    cubeDesc.arraySize = 6;
    cubeDesc.mipLevels = c_ProbeCaptureMips;
    cubeDesc.dimension = nvrhi::TextureDimension::TextureCube;
    cubeDesc.format = nvrhi::Format::RGBA16_FLOAT;
    cubeDesc.isRenderTarget = true;
    cubeDesc.useClearValue = true;
    cubeDesc.clearValue = nvrhi::Color(0.f);
    cubeDesc.initialState = nvrhi::ResourceStates::RenderTarget;
    cubeDesc.keepInitialState = true;
    cubeDesc.debugName = "LightProbeCapture";
    nvrhi::TextureHandle captureColor;
    device->createTexture(cubeDesc, &captureColor);

    cubeDesc.mipLevels = 1;
    cubeDesc.format = nvrhi::Format::D24S8;
    cubeDesc.isTypeless = true;
    cubeDesc.clearValue = nvrhi::Color(1.f);
    cubeDesc.initialState = nvrhi::ResourceStates::DepthWrite;
    cubeDesc.debugName = "LightProbeCaptureDepth";
    nvrhi::TextureHandle captureDepth;
    device->createTexture(cubeDesc, &captureDepth);

    auto captureFramebuffer = MAKE_RC_OBJ_PTR(engine::FramebufferFactory, device);
    captureFramebuffer->RenderTargets = { captureColor };
    captureFramebuffer->DepthTarget = captureDepth;

    // Camera-relative capture: the cube view sits at the origin, the scene is shifted by -capturePosition.
    engine::CubemapView cubeView;
    cubeView.SetArrayViewports(int(c_ProbeCaptureSize), 0);
    // The 2018 CubemapView::SetTransform (0x14007FED0) composes the given transform (identity here) with a z flip,
    // diag(1, 1, -1), before applying the per-face matrices, which are the same as donut main's. donut main's
    // CubemapView has no flip, so it is passed in explicitly; without it the captured faces are mirrored.
    cubeView.SetTransform(scaling(float3(1.f, 1.f, -1.f)), 1.f, c_ProbeCaptureFarDistance, false);
    cubeView.UpdateCache();

    std::shared_ptr<fx::EnvironmentMapPass> environmentPass;
    if (m_SkyTexture && m_SkyTexture->texture)
        environmentPass = std::make_shared<fx::EnvironmentMapPass>(device, m_ShaderFactory, m_CommonPasses,
            captureFramebuffer, cubeView, m_SkyTexture->texture, nullptr);

    // 0x1400840B0(..., singlePassStereo = 0, singlePassCubemap = 0, trackLiveness = 1): the forward pass draws the
    // six faces as separate planar views.
    auto forwardPass = std::make_shared<ForwardShadingPass2018>(device, m_ShaderFactory, m_CommonPasses,
        captureFramebuffer, cubeView, m_Scene->GetMaterialBindingLayout(), false, false, true);

    auto forwardStrategy = MAKE_RC_OBJ_PTR(MeshletDrawStrategy, device, m_ShaderFactory, m_Scene, m_MeshletResources,
        captureDepth, m_RenderTargets ? m_RenderTargets->HiZTexture.Get() : nullptr,
        m_RandomsTexture ? m_RandomsTexture->texture.Get() : nullptr, m_ViewDistanceMap,
        "forward_ps.hlsl", MeshletRenderRole::Forward);
    // Renderer fields set by RenderLightProbes (0x1400327F0, object offsets): +252 wireframe 0, +253 enableLod 1,
    // +254 enableDistanceLod 1, +255 enableAsteroidsCulling 1, +256 showBBoxes 0, +260 alphaPreset 0,
    // +268 visualizeLods 0, +280 transitionRange 0, +284 lodBias -2, +328 lodView = the cube view.
    forwardStrategy->wireframe = false;
    forwardStrategy->enableLod = true;
    forwardStrategy->enableDistanceLod = true;
    forwardStrategy->enableAsteroidsCulling = true;
    forwardStrategy->showBBoxes = false;
    forwardStrategy->alphaPreset = 0;
    forwardStrategy->visualizeLods = 0;
    forwardStrategy->transitionRange = 0.f;
    forwardStrategy->lodBias = -2.f;
    forwardStrategy->lodView = &cubeView;

    m_Planets->CreateRenderPasses(device, m_ShaderFactory, captureFramebuffer, cubeView, m_CommonPasses);

    // The capture uses a fixed sun (direction (-0.17, -0.54, 0.822), irradiance 1) as the only light.
    const float3 savedSunDirection = m_SunLight->direction;
    const float savedSunIrradiance = m_SunLight->irradiance;
    std::vector<std::shared_ptr<SceneLight>> captureLights = { m_SunLight };
    const float3 ambient = m_UI->ambientColor * 0.01f;

    for (const auto& probe : m_LightProbes)
    {
        m_CommandList->open();

        m_CommandList->clearTextureFloat(captureColor, nvrhi::AllSubresources,
            nvrhi::Color(probe->debugColor.x, probe->debugColor.y, probe->debugColor.z, probe->debugColor.w));
        m_CommandList->clearDepthStencilTexture(captureDepth, nvrhi::AllSubresources, true, 1.f, true, 0);

        forwardStrategy->preViewTranslation = -probe->capturePosition;
        forwardStrategy->preViewTranslationPrevious = -probe->capturePosition;

        UpdateScene(m_CommandList);
        UpdateViewDistanceMap(m_CommandList, probe->capturePosition.y, c_ProbeCaptureFarDistance);

        m_SunLight->direction = float3(-0.17f, -0.54f, 0.8221f);
        m_SunLight->irradiance = 1.f;

        if (environmentPass)
            environmentPass->Render(m_CommandList, cubeView, m_UI->nebulaBrightness,
                m_UI->starLinearBrightness, m_UI->starSquareBrightness);
        m_Planets->Render(m_CommandList, cubeView, *m_SunLight, 2.f, 1.f, m_UI->planetFlag368);
        forwardPass->Render(m_CommandList, *forwardStrategy, cubeView, captureLights, ambient, ambient, {});

        m_LightProbePass->GenerateCubemapMips(m_CommandList, captureColor, 0, 0, c_ProbeCaptureMips - 1);
        m_LightProbePass->RenderDiffuseMap(m_CommandList, captureColor, nvrhi::AllSubresources,
            probe->diffuseMap, 6 * probe->diffuseArrayIndex, 0);

        const uint32_t specularMips = probe->specularMap->getDesc().mipLevels;
        for (uint32_t mip = 0; mip < specularMips; mip++)
        {
            const float roughness = powf(float(mip) / float(std::max(int(specularMips) - 1, 1)), 2.f);
            m_LightProbePass->RenderSpecularMap(m_CommandList, roughness, captureColor, nvrhi::AllSubresources,
                probe->specularMap, 6 * probe->specularArrayIndex, mip);
        }

        m_LightProbePass->RenderEnvironmentBrdfTexture(m_CommandList);
        m_EnvironmentBrdf->texture = m_LightProbePass->GetEnvironmentBrdfTexture();

        m_CommandList->close();
        device->executeCommandList(m_CommandList);
        device->waitForIdle();
        device->runGarbageCollection();

        probe->environmentBrdf = m_EnvironmentBrdf->texture;
        probe->enabled = true;
    }

    m_SunLight->direction = savedSunDirection;
    m_SunLight->irradiance = savedSunIrradiance;

    // Restore the planet passes for the main view and drop the probe debug view.
    if (m_RenderTargets && m_View)
        m_Planets->CreateRenderPasses(device, m_ShaderFactory, m_RenderTargets->HdrFramebuffer, *m_View, m_CommonPasses);
    m_ShowCubemapPass.reset();
}

// =====================================================================================================
// CreateRenderPasses
// =====================================================================================================

void FeatureDemo::CreateRenderPasses(bool& exposureResetRequired, nvrhi::IFramebuffer* framebuffer)
{
    nvrhi::IDevice* device = GetDevice();
    const engine::PlanarView& view = *m_View;
    RenderTargets& targets = *m_RenderTargets;
    nvrhi::IBindingLayout* materialLayout = m_Scene->GetMaterialBindingLayout();

    // +776 forward shading (2018: no stereo, no cubemap, liveness tracking on)
    m_ForwardPass = std::make_shared<ForwardShadingPass2018>(device, m_ShaderFactory, m_CommonPasses,
        targets.HdrFramebuffer, view, materialLayout, false, false, true);

    // +664 / +672
    const std::vector<engine::ShaderMacro> macros = { { "IS_SHIP", "0" }, { "_ASTEROIDS", "0" } };
    m_GBufferPixelShader = m_ShaderFactory->CreateShader("demo/gbuffer_ps.hlsl", "main", &macros, nvrhi::ShaderType::Pixel);
    m_DeferredLightingPixelShader = m_ShaderFactory->CreateShader("demo/deferred_lighting_ps.hlsl", "main", nullptr,
        nvrhi::ShaderType::Pixel);

    // +848
    GBufferFillPass2018::CreateParameters gbufferParams;
    // deviation: the binary also handed m_GBufferPixelShader to the pass, which then built its (never used)
    // input-assembler pipelines from gbuffer_vs + material layout. Under D3D12 root signature validation those
    // pipelines are invalid (c_GBuffer at b0 with VS|GS visibility overlaps the material constants at b0), so
    // the shader is not passed and only the meshlet path, which the demo uses, is set up.
    gbufferParams.materialPixelShader = nullptr;
    gbufferParams.enableDepthWrite = true;
    gbufferParams.enableMotionVectors = true;
    gbufferParams.trackLiveness = false;
    gbufferParams.stencilWriteMask = 1;
    m_GBufferPass = std::make_shared<GBufferFillPass2018>(device, m_ShaderFactory, m_CommonPasses,
        targets.GBufferFramebuffer, view, materialLayout, gbufferParams);

    // +680 / +688
    m_GBufferBindingLayout = DeferredLightingPass2018::CreateGBufferBindingLayout(device);
    m_GBufferBindingSet = DeferredLightingPass2018::CreateGBufferBindingSet(device, m_GBufferBindingLayout,
        targets.DepthBuffer, targets.GBuffer0, targets.GBuffer1, targets.GBuffer2);

    // +840
    m_DeferredLightingPass = std::make_shared<DeferredLightingPass2018>(device, m_ShaderFactory, m_CommonPasses,
        targets.HdrFramebufferNoDepth, view, m_DeferredLightingPixelShader, m_GBufferBindingLayout);

    // +1160 / +1168
    m_Planets->CreateRenderPasses(device, m_ShaderFactory, targets.HdrFramebuffer, view, m_CommonPasses);
    m_SunDisk = std::make_shared<fx::SunDisk>(device, m_ShaderFactory, targets.HdrFramebuffer, view);

    // +800 textured starfield, only once the sky texture exists
    if (m_SkyTexture && m_SkyTexture->texture)
    {
        m_EnvironmentMapPass = std::make_shared<fx::EnvironmentMapPass>(device, m_ShaderFactory, m_CommonPasses,
            targets.HdrFramebuffer, view, m_SkyTexture->texture, nullptr);
    }

    // +824
    m_RealStarFieldPass = std::make_shared<fx::RealStarFieldPass>(device, m_ShaderFactory, m_CommonPasses,
        targets.HdrFramebuffer, view, m_RealStarFieldResources);

    // +880 TAA with motion vectors (stencil mask 1: the G-buffer marks the geometry it drew)
    {
        render::TemporalAntiAliasingPass::CreateParameters taaParams;
        taaParams.sourceDepth = targets.DepthBuffer;
        taaParams.motionVectors = targets.MotionVectors;
        taaParams.unresolvedColor = targets.HdrColor;
        taaParams.resolvedColor = targets.ResolvedColor1;
        taaParams.feedback1 = targets.ResolvedColor2;
        taaParams.feedback2 = targets.TemporalFeedback;
        taaParams.motionVectorStencilMask = 1;
        taaParams.useCatmullRomFilter = true;
        m_TemporalAntiAliasingPass = MAKE_RC_OBJ_PTR(render::TemporalAntiAliasingPass, device, m_ShaderFactory,
            m_CommonPasses, view, taaParams);

        // The resolve (and the jitter) is the 2018 pass; donut's pass only renders the motion vectors.
        TemporalAAPass2018::CreateParameters taa2018;
        taa2018.unresolvedColor = targets.HdrColor;
        taa2018.motionVectors = targets.MotionVectors;
        taa2018.resolvedColor1 = targets.ResolvedColor1;
        taa2018.resolvedColor2 = targets.ResolvedColor2;
        taa2018.useCatmullRomFilter = true;
        m_TaaPass2018 = std::make_shared<TemporalAAPass2018>(device, m_ShaderFactory, taa2018);
    }

    // +856: the binary creates a pixel SSAO pass here that nothing ever renders (HBAO+ is used instead).
    // deviation: donut main's SsaoPass is a compute pass with different inputs; since it is never rendered it
    // is not created.
    m_SsaoPass = nullptr;

    // +920
    m_FogPass = std::make_shared<fx::FogPass>(device, m_ShaderFactory, m_CommonPasses, targets.HdrFramebufferNoDepth,
        targets.DepthBuffer, view);

    // +928, keeping the particle state of the previous instance
    {
        nvrhi::IBuffer* previousParticles = m_Particles ? m_Particles->GetParticleBuffer() : nullptr;
        m_Particles = std::make_shared<fx::ParticleSystem>(device, m_ShaderFactory, m_CommonPasses,
            targets.HdrFramebuffer, view, 250000, float3(5000.f, 2000.f, 5000.f), previousParticles);
    }

    // +960
    m_ShieldPass = std::make_shared<fx::ShieldPass>(device, m_ShaderFactory, m_CommonPasses,
        targets.HdrFramebufferNoDepth, view, targets.DepthBuffer);

    // The exposure buffer of the previous tone mapping pass is reused; without one the exposure is reset.
    nvrhi::BufferHandle exposureBuffer;
    if (m_ToneMappingPass)
        exposureBuffer = m_ToneMappingPass->GetExposureBuffer();
    else
        exposureResetRequired = true;

    // +936
    m_LensFlarePass = std::make_shared<fx::LensFlarePass>(device, m_ShaderFactory, m_CommonPasses,
        targets.HdrFramebufferNoDepth, view);

    // +944 "Upscale"
    m_HqBlitPass = std::make_shared<fx::HqBlitPass>(device, m_ShaderFactory, m_CommonPasses, framebuffer);
    if (!m_SharpenBlitPass)
        m_SharpenBlitPass = std::make_shared<SharpenBlitPass2018>(device, m_ShaderFactory, m_CommonPasses);

    // +736: created, never used by the shipped binary
    m_AccumulationPixelShader = m_ShaderFactory->CreateShader("demo/accumulation_ps.hlsl", "main", nullptr,
        nvrhi::ShaderType::Pixel);

    // +832
    {
        ToneMappingPass2018::CreateParameters toneMappingParams;
        toneMappingParams.histogramBins = 256;
        toneMappingParams.isTextureArray = false;
        toneMappingParams.exposureBufferOverride = exposureBuffer;
        toneMappingParams.colorLUT = m_ColorLutTexture ? m_ColorLutTexture->texture.Get() : nullptr;
        m_ToneMappingPass = std::make_shared<ToneMappingPass2018>(device, m_ShaderFactory, m_CommonPasses,
            targets.LdrFramebuffer, view, toneMappingParams);
    }

    // +888
    m_BloomPass = std::make_shared<BloomPass2018>(device, m_ShaderFactory, m_CommonPasses,
        targets.ResolvedFramebuffer1, view);

    m_PreviousViewsValid = false;

    // +248..+312: the five meshlet renderers
    nvrhi::ITexture* randoms = m_RandomsTexture ? m_RandomsTexture->texture.Get() : nullptr;
    auto makeRenderer = [&](const char* pixelShader, MeshletRenderRole role)
    {
        return MAKE_RC_OBJ_PTR(MeshletDrawStrategy, device, m_ShaderFactory, m_Scene, m_MeshletResources,
            targets.DepthBuffer, targets.HiZTexture, randoms, m_ViewDistanceMap, pixelShader, role);
    };
    m_GBufferRenderer = makeRenderer("gbuffer_ps.hlsl", MeshletRenderRole::GBuffer);
    m_DepthRenderer = makeRenderer("", MeshletRenderRole::Depth);
    m_MaterialIdRenderer = makeRenderer("material_id_ps.hlsl", MeshletRenderRole::MaterialId);
    m_ForwardRenderer = makeRenderer("forward_ps.hlsl", MeshletRenderRole::Forward);
    m_ForwardRenderer->transparentPass = true;
    m_ShipForwardRenderer = makeRenderer("forward_ps.hlsl", MeshletRenderRole::Forward);

    m_ShowCubemapPass.reset();
}
