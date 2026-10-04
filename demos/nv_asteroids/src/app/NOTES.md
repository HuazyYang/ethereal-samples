# src/app and src/ui: app layer of the Asteroids demo

This module reconstructs the entry point (`WinMain` 0x14003CC90), the startup dialog (`DialogFunc` 0x14002E0D0), the
shared options struct `UIData` (0x1400246A0), `UIRenderer` (vtable 0x140258420) and the app side of `FeatureDemo`
(vtable 0x140257F68), including its render side (pass creation, scene loading, the per-frame RenderScene and its
helpers, light-probe capture) on top of the scene / meshlets / passes / fx modules and donut main's render passes.

## Files

| File | Contents | Binary provenance |
|---|---|---|
| `main.cpp` | `WinMain`, `DialogFunc` (only in the executable, guarded by `ASTEROIDS_MAIN_EXE`) | 0x14003CC90, 0x14002E0D0 |
| `Asteroids.rc`, `resource.h`, `res/` | dialog 101, icon 106, bitmap 107, version info (extracted from the exe) | resource section |
| `AppGlobals.h/.cpp` | `demo::g_Options` (command line), replay frame counter, benchmark totals, RNG, far distance | 0x1402D10A0..0x1402D10C0, 0x1402D1100, 0x1402DF20D.. |
| `UIData.h` | the options/statistics struct, layout `static_assert`ed to the 2018 offsets | ctor 0x1400246A0 |
| `FeatureDemo.h` | the whole class with 2018 member offsets | 0x140257F68 |
| `FeatureDemo.cpp` | constructor, destructor, opening-sequence helpers | 0x1400230B0 |
| `FeatureDemo_Input.cpp` | Animate, keyboard, mouse, gamepad, frame limiter | 0x14002A4B0, 0x14002EC20, 0x140031900/60, 0x1400318E0, 0x14002EB90/00, 0x1400380D0, 0x1400381C0 |
| `FeatureDemo_Data.cpp` | replay, animations, camera presets, light probes | 0x1400310B0, 0x140030580, 0x14002AE20, 0x140030760, 0x140035810, 0x140030D40, 0x140035D40, 0x1400360F0 |
| `FeatureDemo_Scene.cpp` | LoadScene, SceneLoaded, CreateRenderPasses, light-probe textures / objects / capture | 0x140031280, 0x140036540, 0x14002BD60, 0x14002B590, 0x14002B920, 0x1400327F0 |
| `FeatureDemo_Render.cpp` | RenderScene and its helpers, RenderSplashScreen, SceneUnloading | 0x1400342A0 and the helpers listed below, 0x1400354A0, 0x140037020 |
| `DemoLightProbe.h` | donut LightProbe + capture position, height band, tint | RTTI DemoLightProbe, 0x14002B920 |
| `RenderTargets.h/.cpp` | the render target set and its framebuffer factories | 0x14004A2C0, 0x14004B3C0, 0x14004B270 |
| `ThirdPersonCamera.h/.cpp` | the chase camera that follows the player ship | vtable 0x14025B650, ctor 0x140062170 |
| `Animation.h/.cpp` | `Animation::Track<T>` and the track set (`Animation::Sequence`) | 0x14002F1B0..0x14002FF40, 0x14002E4D0, 0x14001C3B0 |
| `InputReplay.h/.cpp` | the recorded gamepad stream at FeatureDemo+1272 | 0x140016DE0, 0x140017C10, 0x140018430, 0x140018630 |
| `JsonFile.h/.cpp` | JSON loader with an optional silent mode | 0x140073C80 |
| `../ui/UIRenderer.h/.cpp` | the demo UI on donut's `ImGui_Renderer` | ctor 0x140024960, buildUI 0x14003A430, counters 0x140030A90 |

## UIData layout

`UIData` is 372 bytes, created on WinMain's stack. FeatureDemo keeps a pointer at +1200, and UIRenderer keeps one at +440.

| Offset | Field | Default | Used by |
|---|---|---|---|
| +0 | `showGui` | false | '`' key; SceneLoaded sets `!benchmark` |
| +1 | `showConfirmExit` | false | Escape key -> "Confirm exit" |
| +2 | `showBenchmarkResult` | false | Animate, when a benchmark replay ends |
| +3 | `paused` | false | Pause key: stops the demo clock |
| +4 / +8 / +12 | `solarAxisElevation` / `solarAxisAzimuth` / `sunRotation` | 40 / 45 / 40 | "Environment" sliders; "SunRotation" track |
| +16 | `ambientColor` (float3) | (0.074, 0.136, 0.242) | in percent of the sun irradiance (0x140033CA0) |
| +28 | `unknown28` | false | never read |
| +29 | `enableHbao` | !-disableSSAO | "HBAO+" |
| +32 | `hbao` (5 floats) | 2, 150, 25, 0.1, 3.9 | HBAO+ wrapper (0x140097230) |
| +52 | `toneMapping` (donut `ToneMappingParameters`) | donut defaults (0.8, 0.95, 1, 0.5, 0.02, 0.5, -0.5, 3, true) | tone mapping (0x140094A50) |
| +88 / +92 / +96 | `taaNewFrameWeight` / `taaClampingFactor` / `taaEnableHistoryClamping` | 0.05 / 1 / true | TAA resolve parameters (0x14002B2A0) |
| +100 | `aaMode` (`AntiAliasingMode`) | (-aa == 1) | "Temporal AA"; RenderScene (3 = upscale) |
| +104 | `alphaPreset` | 0 | asteroid renderers `alphaPreset` (+260) |
| +108 | `sharpness` | 0.125 | final sharpen blit |
| +112 | `enableVsync` | false | "VSync (V)", V key, RenderScene |
| +113 | `reloadShaders` | false | view setup: clear the shader cache and recreate the passes (never set) |
| +114 / +116 | `enableBloom` / `bloomSigma` | true / 32 | "Bloom"; sigma is scaled by height/1080 |
| +120 | `enableShadows` | !-disableShadows | "Shadows" |
| +121 | `wireframe` | false | "Wireframe", K, gamepad Up |
| +122 | `dynamicLod` | true | "Dynamic LOD", L |
| +123 | `asteroidsCulling` | true | renderers `enableAsteroidsCulling` (+255) |
| +124 | `hiZCulling` | true | "Hi-Z Culling" |
| +125 | `readbackCullingStats` | false | "numCulled = %d" readback |
| +126 | `showShipBoundingBoxes` | false | ship overlay renderer (+312): `showBBoxes`, wireframe |
| +127 / +128 | `distanceLod` / `zCullSectorThreshold` | true / 3 | renderers `enableDistanceLod` (+254) / `zCullSectorThreshold` (+272) |
| +132 / +136 | `cameraLodBias` / `shadowLodBias` | 0.85 / -2.3 | "Camera/Shadow LOD Bias" |
| +140 | `lodScale` | 1.25 | renderers +288, and (1 - x) * 9 added to the biases |
| +144 | `enableToneMapping` | !-disableToneMapping | post-processing |
| +145 | `unknown145` | true | |
| +148 | `nebulaBrightness` | 0.13 | fx::EnvironmentMapPass |
| +152 / +156 | `starLinearBrightness` / `starSquareBrightness` | 0 / 0.6 | EnvironmentMapPass star layers (probe capture 0x1400327F0) |
| +160 / +164 | `enableRealStars` / `realStarBrightness` | true / 1.75 | fx::RealStarFieldPass |
| +168 | `useLightProbeAmbient` | true | when set, the constant ambient term is zero |
| +169 | `useProbeHeightBands` | true | CollectLightProbes: three height-band probes, else probe 0 everywhere |
| +172 / +176 | `sunDiffuseScale` / `sunSpecularScale` | 1 / 1.5 | sun light constants (0x1400377C0) |
| +180 | `staticLodIndex` | 0 | keys 0-6 |
| +184 | `lodTransitionRange` | 0.1 | renderers `transitionRange` (+280) |
| +188 | `visualizeLods` | false | "Visualize LODs", gamepad Down |
| +189 | `drawDebugOverlay` | false | overlay (0x1400321F0) |
| +190 / +192 | `showLightProbe` / `lightProbeIndex` | false / 0 | fx::ShowCubemapPass |
| +196..+208 | `shadowDistance`, `shadowDepthRange`, `cascadeExponent`, `shadowFitToView` | 20000, 30000, 3, true | shadows (0x140034F70); +196 also feeds Fog |
| +212 | `verticalFov` | 70 | "Vertical FOV" [20, 110] |
| +216 | `minAsteroidScreenSize` | 5 | gbuffer renderer `minAsteroidScreenSize` (+368) |
| +220 | `cameraMode` | 1 | 1 = chase camera, 0 = free camera ('T') |
| +224 / +225 | `enableFog` / `enableParticles` | true / true | "Fog", "Particles" |
| +226 / +228 | `particlesParam226` / `particlesParam228` | true / 64 | fx::ParticleSystem (mesh shader, per batch) |
| +232 | `resetParticles` | false | "Reset Particles" |
| +233 | `enableLensFlare` | true | "LensFlare" |
| +236 | `fog` (`fx::FogParameters`, 36 B) | -3000, 4000, 0.0005, 1, 0.0001, (0.667, 0.742, 1), 1.5 | fx::FogPass |
| +272 | `unknown272` | 0 | |
| +276 | `cameraSpeedExponent` | 8 | free camera speed = 2^x |
| +280 | `pickedMaterial` | null | picking (dead code in the binary) |
| +288 | `planetLightScale` | 1 | sun visibility multiplier |
| +292 | `statsMeshletCount` | 0 | 0x140041220 |
| +296 / +304 | `asteroidsDrawnWithShadows` / `asteroidsDrawn` | 0 | "Total asteroids" |
| +312 / +316 / +320 | `frameTimeMs` / `shadowTimeMs` / `gbufferTimeMs` | 0 | exponential averages (0.95) |
| +328 / +336 | `maxLodTrianglesWithShadows` / `maxLodTriangles` | 0 | "Max LOD triangles" |
| +344 | `drawnTriangles` | 0 | "Drawn triangles" (pipeline statistics + particles) |
| +352 | `unknown352` | 0 | |
| +360 | `statsRendererValue` | 0 | gbuffer renderer +304 |
| +364 | `showCounters` | false | 'F', gamepad Left; SceneLoaded sets it |
| +365 | `shipCollisions` | true | collision-resolved ship movement |
| +366 | `freezeShipPosition` | false | "Freeze Ship Position" |
| +367 | `drawAsteroids` | true | gbuffer and ship renderers `drawAsteroids` (+336) |
| +368 | `planetFlag368` | false | planets with rings flag / light probes |
| +369 | `drawSpaceObjects` | true | forward pass (+296); gbuffer/ship renderers `drawPlayerShip`/`drawOtherObjects` (+337/+338) |
| +370 | `enableShield` | true | "Shield" |
| +371 | `mute` | false | "Mute" |

## FeatureDemo members (2018 offset -> reconstruction)

ApplicationBase provides +8 DeviceManager, +16 scene-loaded flag, +24 `m_TextureCache`, +40 loading thread, +48
`m_CommonPasses` and +64 async flag.

| Offset | Member | Notes |
|---|---|---|
| +96 | `m_PreviousViewsValid` | cleared on pass/target changes |
| +100 / +104 | `m_CurrentTime` / `m_ElapsedTime` | demo clock / frame step (0 when paused) |
| +112 | `m_DeviceManager` | |
| +120 | `m_ShaderFactory` | base path "/shaders" |
| +136 | `m_RootFs` | donut RootFileSystem (deviation, see below) |
| +152 | `m_MediaPath` | "/media" |
| +184 | `m_Scene` (SpaceScene) | |
| +200 / +216 | `m_OpaqueDrawStrategy` / `m_TransparentDrawStrategy` | donut |
| +232 | `m_MeshletResources` | MeshletRenderResources |
| +248..+312 | `m_GBufferRenderer`, `m_DepthRenderer`, `m_MaterialIdRenderer`, `m_ForwardRenderer`, `m_ShipForwardRenderer` | MeshletDrawStrategy (roles 1, 0, 2, 3, 3) |
| +328 | `m_PipelineStatsQuery` | PipelineStatisticsQuery(device, 3) |
| +336 | `m_ActiveCamera` | |
| +344 | `m_FirstPersonCamera` | donut FirstPersonCamera, move speed 100, then 2^cameraSpeedExponent |
| +576 | `m_ShipCamera` | ThirdPersonCamera, owned (deviation) |
| +584 | `m_CameraPresets` | 40-byte {pos, look_at, up, valid} |
| +608 / +624 | `m_View` / `m_ViewPrevious` | swapped every frame |
| +640 / +652 | `m_ViewOrigin` / `m_ViewOriginPrevious` | -camera position |
| +664 / +672 | `m_GBufferPixelShader` / `m_DeferredLightingPixelShader` | |
| +680 / +688 | `m_GBufferBindingLayout` / `m_GBufferBindingSet` | |
| +696 | `m_CommandList` | |
| +704..+728 | blit binding sets (LdrColor, HdrColor, ResolvedColor1/2) | not declared: donut's BlitTexture takes textures |
| +736 | `m_AccumulationPixelShader` | created, never used |
| +744..+760, +784 | never written by the binary | not declared |
| +768 | `m_RenderTargets` | RenderTargets |
| +776 | `m_ForwardPass` | ForwardShadingPass2018 |
| +800 / +808 / +824 | `m_EnvironmentMapPass` / `m_RealStarFieldResources` / `m_RealStarFieldPass` | fx |
| +832 | `m_ToneMappingPass` | donut (not the SSAO pass the old notes said) |
| +840 / +848 | `m_DeferredLightingPass` / `m_GBufferPass` | DeferredLightingPass2018 / GBufferFillPass2018 |
| +856 | `m_SsaoPass` | pixel SSAO in 2018, created but never rendered; not created here (deviation) |
| +864 | `m_LightProbePass` | donut LightProbeProcessingPass |
| +880 / +888 | `m_TemporalAntiAliasingPass` / `m_BloomPass` | donut |
| +896 | `m_HbaoPlus` | HbaoPlusPass (src/passes) |
| +904 / +912 | `m_MaterialIdPass` / `m_MaterialIdReadback` | never created by the binary (picking is dead code) |
| +920 / +928 / +936 / +944 / +952 / +960 | `m_FogPass`, `m_Particles`, `m_LensFlarePass`, `m_HqBlitPass`, `m_ShowCubemapPass`, `m_ShieldPass` | fx |
| +968 | `m_ShadowMap` | donut CascadedShadowMap(2048, 3, 1, D24S8) |
| +984 / +1000 | `m_SunLight` / `m_Lights` | SceneDirectionalLight / SceneLight |
| +1024 / +1040 / +1056 / +1072 | `m_SplashTexture`, `m_SkyTexture`, `m_RandomsTexture`, `m_ColorLutTexture` | LoadedTexture |
| +1088 | `m_LightProbes` | `DemoLightProbe` (DemoLightProbe.h) |
| +1112 / +1128 / +1144 | `m_LightProbeDiffuse` / `m_LightProbeSpecular` / `m_EnvironmentBrdf` | |
| +1160 / +1168 | `m_Planets` / `m_SunDisk` | fx::PlanetSet / fx::SunDisk |
| +1176 | `m_ViewDistanceMap` | Texture1D 128 x R32_FLOAT, SRV |
| +1184 / +1192 | `m_ConstructionTimestamp` / `m_FirstFrameTimestamp` | "Loading time" |
| +1200 | `m_UI` | |
| +1208 | `m_JoystickButtons[15]` | edge detection |
| +1224..+1252 | `m_KeyThrottle`, `m_JoystickThrottle`, `m_KeyYaw`, `m_JoystickYaw`, `m_KeyPitch`, `m_JoystickPitch`, `m_KeyRoll`, `m_JoystickRoll` | int key / float pad pairs |
| +1256 | `m_MousePosition` | |
| +1264 | `m_PickRequested` | never set |
| +1272 | `m_Replay` | InputReplay |
| +1320 / +1400 | `m_OpeningSequence` / `m_OpeningSequenceFast` | "start" / "start_fast" |
| +1480 | `m_ActiveSequence` | |
| +1488 / +1492 | `m_CollisionIntensity` / `m_CollisionNormal` | shield flash |
| (new) | `m_ShadowLightGraph`, `m_ShadowLight`, `m_ShadowFramebuffer` | donut DirectionalLight mirroring the sun for CascadedShadowMap (deviation) |
| (new) | `m_ScreenshotPath`, `m_ScreenshotFrame`, `m_RenderedFrames` | `-screenshot` developer option (deviation) |

## Calls into the other modules

These are the real APIs, verified against the headers that were present at the time of the compile check:

| Where | Call |
|---|---|
| Animate | `m_Scene->GetPlayerShip()`, `ship->SetThrottle/SetYawInput/SetPitchInput/SetRollInput`, `ship->GetPosition()`, `ship->Move(t, dt, freeze)`, `m_Scene->ResolveCollision(from, to, 31)`, `ship->Update(t, dt, desired, actual)`, `ship->PlayShieldSound()`, `ship->Animate(t, dt)` |
| Keys / gamepad | `ship->Stop()`, `m_Scene->GetAmbienceMusic()->Play() / Stop() / Rewind()` |
| ApplyAnimation | `ship->SetPose(pos, yawRad, pitchRad, 0)`, `ship->SetSpeed(v)` |
| ThirdPersonCamera | `ship->GetCargoShip()->GetTransform()`, `ship->GetOrientation()`, `ship->GetUp()`, `ship->GetBoundaryCorrection()` (ship+108) |
| UIRenderer | `SpaceScene::GetLoadProgress().completed`, `GetAmbienceMusic()->GetVolume()/SetVolume()`, `SpaceScene::SetSoundMuted(bool)` |
| Constructor | `std::make_shared<PipelineStatisticsQuery>(device, 3)`, `std::make_shared<fx::RealStarFieldResources>(device, rootFs, mediaPath)` |

The pass constructors are given in `src/passes/NOTES.md`, `src/fx/NOTES.md` and `src/meshlets/NOTES.md`; the 2018 call
order is in `docs/featuredemo_map.md` (with a function-name table at the end).

## Render side

| Function | 2018 | Notes |
|---|---|---|
| `LoadScene` | 0x140031280 | ThreadPool; PlanetSet::Load(planets.json); sky DDS and randoms_texture.dds async (linear); probe DDS unless -renderLightProbes; SpaceScene::Load |
| `SceneLoaded` | 0x140036540 | colour LUT, donut draw strategies, CreateRenderingResources, far distance, sun (first directional light or a default one), chase camera, meshlet resources, probes, opening sequence + music |
| `CreateRenderPasses` | 0x14002BD60 | every pass of the 2018 list (see featuredemo_map.md), the five MeshletDrawStrategy renderers |
| `CreateLightProbeTextures` / `CreateLightProbeObjects` / `RenderLightProbes` | 0x14002B590 / 0x14002B920 / 0x1400327F0 | -renderLightProbes capture with LightProbeProcessingPass (mips, diffuse, specular, BRDF) |
| `RenderScene` | 0x1400342A0 | frame order as in the binary |
| `SetupViewsAndTargets`, `UpdateViews` | 0x140031EF0, 0x140037C50 | targets, PlanarViews with TAA jitter, `perspProjD3DStyle(fov, window aspect, 1, far)`, camera-relative matrices |
| `UpdateSun` | inlined | solar axis rotation (Rodrigues), planet occlusion * planetLightScale |
| `UpdateLights` | 0x140031980 | point/spot lights copied into translated space |
| `CollectLightProbes` | 0x1400377C0 | sun-scaled probe intensities, height-band bounds (planes softened by 0.0005) |
| `UpdateScene`, `UpdateViewDistanceMap` | 0x140038380, 0x1400384F0 | erf-based fog transmittance, 12-step bisection per texel |
| `UpdateAsteroidRendererSettings` | 0x140037370 | UIData -> five renderers |
| `RenderShadows` | 0x140034F70 | cascades (stable or view-fit) + per-object shadow of the cargo ship, depth renderer |
| `RenderGBuffer`, `RenderLightingAndEffects` | 0x140032630, 0x140033CA0 | deferred lighting, environment, real stars, sun, planets, HBAO+, fog, particles, forward, shield |
| `ResolveOrAccumulate`, `PostProcess`, `RenderOverlay` | 0x14002B2A0, 0x140033960, 0x1400321F0 | TAA, particles/bloom/lens flare/tone mapping, shadow-map and probe debug views |

Render-side deviations (also marked in the code):
- **donut passes as-is.** TemporalAntiAliasingPass, ToneMappingPass, BloomPass, CascadedShadowMap, LightProbeProcessingPass and
  CommonRenderPasses replace the 2018 framework passes.
  - TAA: donut keeps its own feedback pair (`RenderTargets::TemporalFeedback` is added), resolves into ResolvedColor1, and
    `feedbackIsValid = m_PreviousViewsValid` replaces the 2018 first-frame HdrColor blit. Motion vectors get the
    camera-relative offset difference `m_ViewOrigin - m_ViewOriginPrevious`.
  - Bloom: donut's composite blend factor is a parameter; 0.05 is used (unresolved: the 2018 pass had none).
  - CascadedShadowMap: needs a donut `DirectionalLight` in a scene graph; a one-node graph mirrors the sun's direction.
  - Final blit: the 2018 sharpen blit is reconstructed (`passes/SharpenBlitPass2018`, 2018 `framework/sharpen_ps.hlsl` with the 2018
    BlitConstants layout plus donut's rect_vs). It is used when `UIData::sharpness` (0.125, set only in the UIData ctor, with no UI) > 0
    and AA is not off, as in 0x1400342A0. donut's CommonRenderPasses cannot set a sharpen factor.
  - SSAO (+856): donut's SsaoPass is compute with different inputs; since the binary never renders it, it is not created.
  - CommonRenderPasses has no blit binding cache, so the 2018 `ResetBindingCache` on resize is dropped.
- **G-buffer pass.** `GBufferFillPass2018` gets no `materialPixelShader`: with it, the pass builds its never-used input-assembler
  pipelines, and their root signature is invalid on D3D12 (c_GBuffer b0 with VS|GS visibility overlaps the material constants b0).
  The meshlet path, the only one the demo uses, is unaffected.
- **Space-object binding sets** are reset on resize together with the asteroid types (they bind the Hi-Z texture).
- **New previous view** is initialised from the current one (the binary left it uninitialised for one frame).
- **HBAO+** is disabled when the nvrhi validation layer wraps the device (`-debug`): HbaoPlusPass needs `nvrhi::d3d12::IDevice`.

## WinMain options

| Option | Effect |
|---|---|
| `-width N`, `-height N` | back buffer size (default 1920x1080) |
| `-scene NAME` | stored in `g_Options.sceneName`, never used (the constructor always loads asteroids.fscene) |
| `-disableMipmaps`, `-disableShadows`, `-disableToneMapping`, `-disableSSAO` | `g_Options` flags (texture cache, UIData defaults) |
| `-disableAsync` | synchronous scene loading |
| `-debug` | D3D12 debug runtime + nvrhi validation |
| `-aa N` | `UIData::aaMode = (N == 1) ? TemporalAA : None` |
| `-meshletDrawFile X` | parsed and discarded |
| `-adapter NAME` | adapter name substring (matched against donut's `EnumerateAdapters`) |
| `-fullscreen` | start in full screen (also the dialog checkbox) |
| `-nodialog` | skip the startup dialog |
| `-replay N` | load media/replay.N.json and play it after the opening sequence |
| `-benchmark` | benchmark mode (also the dialog checkbox); forces replay >= 0, fast opening sequence, no GUI, shows "Benchmark Result" |
| `-view N` | start at camera preset N (free camera, no GUI, no opening sequence) |
| `-renderLightProbes` | skip loading the probe DDS files; render the probes and save them |
| `-screenshot FILE FRAME` | developer option (deviation): save the scene image of rendered frame FRAME (without the UI pass) with donut `SaveTextureToFile`, then close |
| `-log FILE` | developer option (deviation): mirror the donut log into FILE, without the default error message boxes |

Device parameters: D3D12 at feature level 12_1, 2 back buffers, SRGBA8, no MSAA, title "NVIDIA Asteroids Demo",
and icon 106 through WM_SETICON. The mesh-shader check shows the 2018 message boxes.

## Startup dialog (resource 101, `Asteroids.rc`)

The dialog is 331x181 DLUs, titled "NVIDIA Asteroids Demo", using MS Shell Dlg 8 with DS_MODALFRAME | DS_FIXEDSYS.

| Control | ID | Type | Position (DLU) |
|---|---|---|---|
| banner bitmap 107 (475x167, 24 bpp) | 1009 | static SS_BITMAP \| SS_REALSIZEIMAGE | 7,7 317x103 |
| "Resolution" | -1 | LTEXT | 96,122 34x8 |
| resolution list | 1001 | COMBOBOX CBS_DROPDOWN (editable), "%d x %d" | 134,119 80x30 |
| "Full screen" | 1002 | auto checkbox | 134,136 50x10 |
| "Run benchmark" | 1008 | auto checkbox | 134,149 65x10 |
| "Start" | IDOK | default push button | 216,160 50x14 |
| "Close" | IDCANCEL | push button | 274,160 50x14 |

DialogFunc behaviour:
- It centres the dialog on the desktop.
- It fills the combo with the unique `EnumDisplaySettingsW` resolutions, sorted, and pre-fills the current width x height.
- Start parses "%d x %d"; on failure it shows "Invalid resolution specification".
- The checkboxes write the full-screen and benchmark flags.

## Deviations (also marked in the code)

- **File systems.** A `RootFileSystem` replaces the SQLite root:
  - `/media` -> `RelativeFileSystem(SQLiteFileSystem(media.db, readOnly, "HjLxk8CwekjjquQM"), "media")`
  - `/shaders/donut`, `/shaders/asteroids` and `/shaders/demo` (= asteroids/demo) -> ShaderMake output
    (`ASTEROIDS_*_SHADER_DIR`, falling back to `<exe>/shaders/...`)
  - The shader factory base path is `/shaders`.
- **Mesh shaders.** Two modes (see src/meshlets/NOTES.md, "Mesh-shader modes"). The 2018 NVAPI check (0x140001980) selects the
  original NVAPI mesh-shader path by default. Otherwise the D3D12 SM6.5 path is used (`queryFeatureSupport(Meshlets)`). `-meshShaders nvapi|d3d12` overrides the choice (deviation).
- **Adapters.** The adapter is chosen by name substring -> index (donut has no name filter).
- **Shutdown order.** Passes are destroyed before `DeviceManager::Shutdown`.
- **ThirdPersonCamera.** It is reconstructed on `BaseCamera`, owned by FeatureDemo, and reset in SceneUnloading.
  The binary leaked it and cleared its maps in place.
- **Cascaded shadow map.** donut's CascadedShadowMap ctor drops the 2018 shader factory, common passes and the {4.0, 1000, 1} block.
- **Animation tracks.** These are reconstructed as small classes, not donut samplers. Their clamping and spline end tangents differ.
- **InputReplay.**
  - `Save()` is added. The binary only loads, but replay.0.json is jsoncpp output of exactly this structure.
  - The recording capacity loop uses `<=` to fix a 2018 out-of-bounds write.
- **Fonts and counters.** These use ImGui 1.92:
  - fonts go through `CreateFontFromFile` and `PushFont(font, size)` instead of `SetWindowFontScale`
  - the counter window is pinned at (0,0)
  - the LOD-bias slider gets a non-empty ID
  - "makePretty" uses snprintf; the binary misused swprintf with narrow formats
- **Music volume.** The volume slider uses the voice volume. SpaceScene+424 is not exposed by the scene module.
- **SaveCameraPreset.** It grows the vector instead of writing out of bounds.
- **JSON loading.** `demo::LoadJsonFile` adds a silent mode (Camera%d.json probing), because donut's loader always logs.

## Open questions

- **Recording mode.** Replay recording (mode 1) exists in the input handlers, but nothing in the binary starts it or
  saves the result. `InputReplay::StartRecording/Save` are provided without a key binding.
- **`-scene` and `-meshletDrawFile`.** Both are parsed and never used.
- **Toggle GUI key.** The UI help says "Tab: Toggle GUI", but the key handler toggles on '`' (GLFW_KEY_GRAVE_ACCENT).
  The code follows the handler.
- **Unnamed UIData fields.** +28, +145, +226, +228, +272, +292, +352, +360 and +368 keep neutral names. Their readers are listed
  in the table.
- **ViewDistanceMap.** featuredemo_map.md says UAV, but the 2018 descriptor sets none of the RT/UAV flags.
  It is created as an SRV-only Texture1D.
- **HiZ texture flags.** The binary sets three flag bytes (render target, UAV and a third that may be "typeless"). It is
  created as an R32_FLOAT render-target + UAV texture with 5 mips at ~1/8 resolution (`((size + 127) >> 3) & ~15`).
- **Sequence restart.** Pressing P right after a finished sequence ends it immediately: the current time is only
  reset while the sequence is unfinished. This is kept from the binary.
- **Ship physics while paused.** The ship still advances with the unpaused frame step. Only the demo clock stops.
  This is kept from the binary.
- **Counter font size 0.** The binary would scale a counter without `font_size` to 0; here it falls back to 51.

## Verification (Phase 2)

Build: target `Asteroids` in recon/build (Release, VS 2022) with no errors and no warnings from src/app, src/ui or src/passes/HbaoPlusPass
(only donut's C4819 code-page warnings).

Runs (`build/bin/Asteroids.exe -nodialog`, RTX 4050):
- The scene loads in about 7 s and RenderScene runs every frame; 35 s and 240 s runs end without a crash.
- `-screenshot`: frames 60 to 6000 render the asteroid field, cliffs, planet, stars, nebula, sun disk, fog layer, lens flare,
  with deferred lighting, light probes, shadows, HBAO+, TAA, bloom and tone mapping.
- `-debug` (D3D12 debug layer + nvrhi validation, debug output captured): the only D3D12 error is the shader issue below.

Remaining issues in other modules (not fixable in src/app):
- **shaders/demo/basicMS.hlsl** does not declare the task payload that basicTS.hlsl dispatches (4 bytes), so every basic-object
  mesh-shading PSO fails: "D3D12 ERROR: CreateMeshShader_MismatchedASMSPayloadSize ... Amplification shader (4) ... mesh shader (0)".
  Effects: the cargo ship and the other space objects are not drawn (G-buffer, forward, shadows), and `MeshletShaderSet` retries
  the failed PSOs every frame (about 20 per frame, roughly 26 fps instead of 60+).
  Verified fix: add `in payload BasicTaskPayload payload` (same struct as basicTS) to `ms_main`. With a patched blob dropped into
  build/bin temporarily (and restored afterwards), the ship renders, there are no PSO errors and no D3D12 errors under -debug, and
  the post-opening view matches build/reference/original_default_view.png (chase camera behind the ship between the two cliffs).
- **MeshletShaderSet** does not cache failed pipelines, so a failing PSO is recompiled every frame.
