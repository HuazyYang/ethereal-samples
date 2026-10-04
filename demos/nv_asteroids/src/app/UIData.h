#pragma once

// Asteroids.exe: UIData (no RTTI; constructed on WinMain's stack by 0x1400246A0, 384-byte buffer).
// Shared between FeatureDemo (pointer at FeatureDemo+1200) and UIRenderer (pointer at UIRenderer+440).
//
// The layout below reproduces the 2018 offsets (checked by the static_asserts at the end) so that the
// offsets quoted in the decompiled code can be matched directly. Names come from the UI labels of
// UIRenderer::buildUI (0x14003A430), the keyboard handler (0x14002EC20) and the render code that reads
// each field. Fields whose purpose could not be pinned down keep a neutral name and an "unresolved" note.
//
// Provenance (offset: readers/writers):
//   +0 showGui ('`' key, SceneLoaded), +1 confirm exit (Esc), +2 benchmark result popup (Animate), +3 pause key,
//   +4/+8/+12 "Environment" sliders and the SunRotation animation track, +16 ambient color (0x140033CA0),
//   +29 "HBAO+", +32 HBAO+ parameters (0x140097230), +52 tone mapping parameters (0x140094A50), +88 TAA parameters,
//   +100 aaMode ("Temporal AA", RenderScene 0x1400342A0), +104/+123/+127/+128/+184/+216/+367 renderer settings
//   copied into the asteroid renderers (0x140037370), +108 sharpening, +112 "VSync (V)", +114/+116 "Bloom",
//   +120 "Shadows", +121 "Wireframe", +122 "Dynamic LOD", +124 "Hi-Z Culling", +125 culling readback,
//   +132/+136 "Camera/Shadow LOD Bias", +140 LOD scale, +144 tone mapping on/off, +148..+156 light probe pass,
//   +148 nebula brightness, +160/+164 real stars, +168 light-probe ambient, +172/+176 sun light scales (0x1400377C0), +180 static LOD ("0-6"),
//   +188 "Visualize LODs", +189 debug overlay, +190/+192 probe debug view (0x1400321F0), +196..+208 shadow cascades (0x140034F70),
//   +212 "Vertical FOV", +220 camera mode ('T'), +224 "Fog", +225 "Particles", +226/+228 particle settings,
//   +232 "Reset Particles", +233 "LensFlare", +236 fx::FogParameters (0x14001AE60, 0x1400384F0),
//   +276 free camera speed exponent (Animate), +280 picked material (RenderScene), +288 planet light scale,
//   +292..+360 per-frame statistics written by RenderScene and shown by the keynote counters,
//   +364 counters ('F'), +365 ship collisions, +366 "Freeze Ship Position", +368 planet option,
//   +369 forward pass, +370 "Shield", +371 "Mute".

#include "app/AppGlobals.h"
#include "fx/FogPass.h"

#include <donut/core/math/math.h>
#include <donut/render/ToneMappingPasses.h>

#include <cstddef>
#include <cstdint>

struct SceneMaterial;

// Anti-aliasing / resolve mode (UIData+100). The UI checkbox "Temporal AA" toggles between 0 and 1;
// RenderScene treats 1..3 as "temporal" paths and 3 as render-at-1/1.5-resolution plus upscale.
enum class AntiAliasingMode : int32_t
{
    None = 0,
    TemporalAA = 1,
    Accumulation = 2,   // unresolved: no UI or key selects it; RenderScene routes it through the accumulation pass
    TemporalUpscale = 3
};

// HBAO+ settings handed to the HBAO+ wrapper (UIData+32). The five values mirror donut's SsaoParameters
// order (amount, background view depth, world radius, surface bias, power exponent).
// unresolved: exact mapping onto GFSDK_SSAO_Parameters is done inside the HbaoPlus module.
struct HbaoParameters
{
    float amount = 2.f;                 // +32
    float backgroundViewDepth = 150.f;  // +36
    float radiusWorld = 25.f;           // +40
    float surfaceBias = 0.1f;           // +44
    float powerExponent = 3.9f;         // +48
};

struct UIData
{
    // --- UI state ---------------------------------------------------------------------------------
    bool showGui = false;                   // +0   '`' key; SceneLoaded sets it to !benchmark
    bool showConfirmExit = false;           // +1   Escape key -> "Confirm exit" popup
    bool showBenchmarkResult = false;       // +2   set by Animate when a benchmark replay finishes
    bool paused = false;                    // +3   Pause key: freezes the demo time

    // --- Environment ------------------------------------------------------------------------------
    float solarAxisElevation = 40.f;        // +4   "Solar Axis Elevation" [-90, 90]
    float solarAxisAzimuth = 45.f;          // +8   "Solar Axis Azimuth" [-360, 360]
    float sunRotation = 40.f;               // +12  "Sun Rotation Around Axis" [5, 175], animated by "SunRotation"
    dm::float3 ambientColor = dm::float3(0.074f, 0.136f, 0.242f); // +16 in percent of the sun intensity

    bool unknown28 = false;                 // +28  unresolved: never read by the recovered code
    bool enableHbao = true;                 // +29  "HBAO+" (-disableSSAO)
    HbaoParameters hbao;                    // +32

    donut::render::ToneMappingParameters toneMapping; // +52 (same field order/defaults as the 2018 struct)
    // +88: TemporalAntiAliasingParameters handed to the TAA resolve (0x14002B2A0 passes UIData+88).
    float taaNewFrameWeight = 0.05f;        // +88
    float taaClampingFactor = 1.f;          // +92
    bool taaEnableHistoryClamping = true;   // +96

    // --- Rendering --------------------------------------------------------------------------------
    AntiAliasingMode aaMode = AntiAliasingMode::TemporalAA; // +100 (constructor sets (-aa == 1))
    int32_t alphaPreset = 0;                // +104 -> meshlet renderers alphaPreset (+260)
    float sharpness = 0.125f;               // +108 final blit sharpening when > 0 and aaMode != None
    bool enableVsync = false;               // +112 "VSync (V)"
    bool reloadShaders = false;             // +113 view setup (0x140031EF0) clears the shader cache and
                                            //      recreates the passes when set; nothing in the binary sets it
    bool enableBloom = true;                // +114 "Bloom"
    float bloomSigma = 32.f;                // +116 at 1080 lines, scaled by the render height
    bool enableShadows = true;              // +120 "Shadows" (-disableShadows)
    bool wireframe = false;                 // +121 "Wireframe" / 'K'
    bool dynamicLod = true;                 // +122 "Dynamic LOD" / 'L'
    bool asteroidsCulling = true;           // +123 -> meshlet renderers enableAsteroidsCulling (+255)
    bool hiZCulling = true;                 // +124 "Hi-Z Culling" -> gbuffer renderer (+276)
    bool readbackCullingStats = false;      // +125 reads back numCulled and prints "numCulled = %d"
    bool showShipBoundingBoxes = false;     // +126 ship overlay renderer (+312): showBBoxes, drawn in wireframe
    bool distanceLod = true;                // +127 -> meshlet renderers enableDistanceLod (+254)
    int32_t zCullSectorThreshold = 3;       // +128 -> meshlet renderers (+272): sectors drawn before Hi-Z
    float cameraLodBias = 0.85f;            // +132 "Camera LOD Bias"
    float shadowLodBias = -2.3f;            // +136 "Shadow LOD Bias"
    float lodScale = 1.25f;                 // +140 -> renderers (+288); (1 - lodScale) * 9 is added to both biases
    bool enableToneMapping = true;          // +144 (-disableToneMapping)
    bool unknown145 = true;                 // +145
    float nebulaBrightness = 0.13f;         // +148 textured starfield (fx::EnvironmentMapPass) linear brightness
    float starLinearBrightness = 0.f;       // +152 fx::EnvironmentMapPass star layers (light probe capture only)
    float starSquareBrightness = 0.6f;      // +156
    bool enableRealStars = true;            // +160 real star field pass
    float realStarBrightness = 1.75f;       // +164 fx::RealStarFieldPass brightness
    bool useLightProbeAmbient = true;       // +168 when set the constant ambient term is zero
    bool useProbeHeightBands = true;        // +169 three probes with vertical bands, else probe 0 everywhere
    float sunDiffuseScale = 1.f;            // +172 multiplied by the sun intensity (0x1400377C0)
    float sunSpecularScale = 1.5f;          // +176
    int32_t staticLodIndex = 0;             // +180 keys 0-6, "Select asteroid LOD"
    float lodTransitionRange = 0.1f;        // +184 -> meshlet renderers transitionRange (+280)
    bool visualizeLods = false;             // +188 "Visualize LODs"
    bool drawDebugOverlay = false;          // +189 overlay pass (0x1400321F0)
    bool showLightProbe = false;            // +190 probe cubemap debug view (fx::ShowCubemapPass)
    int32_t lightProbeIndex = 0;            // +192 probe shown by the debug view
    float shadowDistance = 20000.f;         // +196 shadow cascades and fog
    float shadowDepthRange = 30000.f;       // +200
    float cascadeExponent = 3.f;            // +204
    bool shadowFitToView = true;            // +208
    float verticalFov = 70.f;               // +212 "Vertical FOV" [20, 110]
    float minAsteroidScreenSize = 5.f;      // +216 -> G-buffer renderer (+368), pixels
    int32_t cameraMode = 1;                 // +220 1 = third-person ship camera, 0 = free camera ('T')
    bool enableFog = true;                  // +224 "Fog"
    bool enableParticles = true;            // +225 "Particles"
    bool particlesParam226 = true;          // +226
    int32_t particlesParam228 = 64;         // +228
    bool resetParticles = false;            // +232 "Reset Particles" button
    bool enableLensFlare = true;            // +233 "LensFlare"
    fx::FogParameters fog;                  // +236 fog layer (fx::FogPass); [1],[2],[4] also feed 0x1400384F0
    uint32_t unknown272 = 0;                // +272
    float cameraSpeedExponent = 8.f;        // +276 free camera speed = 2^x
    SceneMaterial* pickedMaterial = nullptr;// +280 material whose id was read back (picking, dead code)
    float planetLightScale = 1.f;           // +288

    // --- Statistics (written every frame by RenderScene) ---------------------------------------------
    uint32_t statsMeshletCount = 0;         // +292 unresolved: value of 0x140041220
    uint64_t asteroidsDrawnWithShadows = 0; // +296 "Total asteroids" with includeShadows
    uint64_t asteroidsDrawn = 0;            // +304 "Total asteroids"
    float frameTimeMs = 0.f;                // +312 CPU frame time, exponential average (0.95)
    float shadowTimeMs = 0.f;               // +316 shadow rendering time, exponential average
    float gbufferTimeMs = 0.f;              // +320 G-buffer pass time, exponential average (0x140032630)
    uint64_t maxLodTrianglesWithShadows = 0;// +328 "Max LOD triangles" with includeShadows
    uint64_t maxLodTriangles = 0;           // +336 "Max LOD triangles"
    uint64_t drawnTriangles = 0;            // +344 "Drawn triangles" (pipeline statistics, + particles)
    uint64_t unknown352 = 0;                // +352
    uint32_t statsRendererValue = 0;        // +360 copied from the gbuffer renderer (+304)

    // --- Toggles ---------------------------------------------------------------------------------
    bool showCounters = false;              // +364 'F', keynote counters overlay
    bool shipCollisions = true;             // +365 ship movement with collision against asteroids
    bool freezeShipPosition = false;        // +366 "Freeze Ship Position"
    bool drawAsteroids = true;              // +367 -> G-buffer and ship renderers drawAsteroids (+336)
    bool planetFlag368 = false;             // +368 planet / light probe passes
    bool drawSpaceObjects = true;           // +369 -> drawPlayerShip/drawOtherObjects (+337/+338), forward pass
    bool enableShield = true;               // +370 "Shield"
    bool mute = false;                      // +371 "Mute"

    // Asteroids.exe: 0x1400246A0. Three options come from the command line.
    UIData()
    {
        enableHbao = demo::g_Options.enableSsao;
        enableShadows = demo::g_Options.enableShadows;
        enableToneMapping = demo::g_Options.enableToneMapping;
    }
};

// Layout checks against the 2018 offsets.
static_assert(offsetof(UIData, solarAxisElevation) == 4);
static_assert(offsetof(UIData, ambientColor) == 16);
static_assert(offsetof(UIData, enableHbao) == 29);
static_assert(offsetof(UIData, hbao) == 32);
static_assert(offsetof(UIData, toneMapping) == 52);
static_assert(offsetof(UIData, taaNewFrameWeight) == 88);
static_assert(offsetof(UIData, aaMode) == 100);
static_assert(offsetof(UIData, sharpness) == 108);
static_assert(offsetof(UIData, enableVsync) == 112);
static_assert(offsetof(UIData, enableShadows) == 120);
static_assert(offsetof(UIData, zCullSectorThreshold) == 128);
static_assert(offsetof(UIData, enableToneMapping) == 144);
static_assert(offsetof(UIData, staticLodIndex) == 180);
static_assert(offsetof(UIData, visualizeLods) == 188);
static_assert(offsetof(UIData, shadowDistance) == 196);
static_assert(offsetof(UIData, verticalFov) == 212);
static_assert(offsetof(UIData, cameraMode) == 220);
static_assert(offsetof(UIData, enableParticles) == 225);
static_assert(offsetof(UIData, enableLensFlare) == 233);
static_assert(offsetof(UIData, fog) == 236);
static_assert(offsetof(UIData, unknown272) == 272);
static_assert(offsetof(UIData, cameraSpeedExponent) == 276);
static_assert(offsetof(UIData, pickedMaterial) == 280);
static_assert(offsetof(UIData, planetLightScale) == 288);
static_assert(offsetof(UIData, statsMeshletCount) == 292);
static_assert(offsetof(UIData, asteroidsDrawnWithShadows) == 296);
static_assert(offsetof(UIData, frameTimeMs) == 312);
static_assert(offsetof(UIData, maxLodTrianglesWithShadows) == 328);
static_assert(offsetof(UIData, drawnTriangles) == 344);
static_assert(offsetof(UIData, statsRendererValue) == 360);
static_assert(offsetof(UIData, showCounters) == 364);
static_assert(offsetof(UIData, mute) == 371);
