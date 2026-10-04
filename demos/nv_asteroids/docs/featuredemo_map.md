# FeatureDemo map (Asteroids.exe)

Working notes for reconstructing `FeatureDemo` (vtable 0x140257F68) on donut main. Offsets are into the 2018 object.

## Virtual overrides (2018 slot → donut main virtual)
| slot | 2018 fn | size | donut main |
|---|---|---|---|
| 1 | 0x14002A4B0 | 2408 | `Animate(float)` |
| 4 | 0x14002EC20 | 1409 | `KeyboardUpdate(key, scancode, action, mods)` |
| 6 | 0x140031900 | 86 | `MousePosUpdate` |
| 7 | 0x140031960 | 24 | `MouseScrollUpdate` |
| 8 | 0x1400318E0 | 24 | `MouseButtonUpdate` |
| 9 | 0x14002EB90 | 144 | `JoystickButtonUpdate` (records the replay button mask at +1272, then 0x1400381C0) |
| 10 | 0x14002EB00 | 128 | `JoystickAxisUpdate` (records replay, then 0x1400380D0) |
| 11 | 0x1400342A0 | 3269 | `RenderScene(IFramebuffer*)` |
| 12 | 0x1400354A0 | 879 | `RenderSplashScreen` (splash.jpg) |
| 14 | 0x140031280 | 1623 | `LoadScene(fs, path)` (planets.json, sky DDS, randoms_texture.dds) |
| 15 | 0x140037020 | 837 | `SceneUnloading` |
| 16 | 0x140036540 | 2782 | `SceneLoaded` |

2018 IRenderPass slots: 0 dtor, 1 Animate, 2 BackBufferResizing, 3 BackBufferResized, 4 KeyboardUpdate, 5 KeyboardCharInput,
6 MousePosUpdate, 7 MouseScrollUpdate, 8 MouseButtonUpdate, 9 JoystickButtonUpdate, 10 JoystickAxisUpdate.
ApplicationBase adds: 11 RenderScene, 12 RenderSplashScreen, 13 BeginLoadingScene, 14 LoadScene, 15 SceneUnloading, 16 SceneLoaded.

## Constructor 0x1400230B0 (FeatureDemo(DeviceManager*, UIData*, bool asyncLoad))
1. `NativeFileSystem` → path `<exe dir>/media.db` (0x1400768E0 is the executable directory).
2. `SQLiteFileSystem(dbPath, readOnly=true, "HjLxk8CwekjjquQM")`. On failure: log error "Couldn't open the media database file." and `ExitProcess(1)`.
   Stored as the root FS (+136/+144).
3. `ShaderFactory(device, fs, "shaders/framework", "shaders/demo")` (+120). Media path "media" (+152).
   - deviation: the rebuilt shaders are compiled by ShaderMake to disk, so mount a donut RootFileSystem:
     `/shaders/donut` → donut shader output, `/shaders/asteroids` → demo shader output, `/media` → SQLiteFileSystem.
4. `CommonRenderPasses(device, shaderFactory)` (+48), then a command list (+696).
5. `UIData->aaMode(+100) = (option -aa == 1)`.
6. `CascadedShadowMap(device, shaderFactory, commonPasses, 2048, 3 cascades, 1 per-object, format 47, {4.0f, 1000, 1})` (+968).
7. HBAO+ wrapper object (0x140096F40, `GFSDK_SSAO_CreateContext_D3D12` at 0x140097000, header version {4,0,0,24284062}) (+896).
   (The immediate in Asteroids.exe is 24284062 = 0x1728B9E; the shipped GFSDK_SSAO_D3D12.win64.dll rejects 24283550 with
   GFSDK_SSAO_VERSION_MISMATCH. Reconstructed as src/passes/HbaoPlusPass.)
8. `TextureCache(device, fs)` (+24); `SetGenerateMipmaps(!-disableMipmaps)`; 0x140073420(0).
9. `SetAsynchronousLoadingEnabled(asyncLoad)` (0x1400770E0).
10. Texture "ViewDistanceMap": 1D, width 128, format 33 in the 2018 nvrhi format enum (R32_FLOAT expected; t_ViewDistance is texture1d f32), UAV (+1176).
11. Pipeline statistics query object (0x140040C10, "PipelineStatsQueryResult", 3 frames) (+328).
12. If `-replay N`: load `replay.N.json` (0x1400310B0). Load `animations.json` (0x140030580).
13. `BeginLoadingScene(fs, media/asteroids.fscene)`; load camera presets `Camera0..9.json` (0x140030760, vector at +584, 40-byte entries {pos, lookAt, up}).
14. `RealStarFieldResources(device, fs, mediaPath)` (0x140066BF0: SkyAndStars/Stars.buf → "Star instance buffer") (+808).
15. Camera: if `-view N` then use preset N. `FPSCamera::LookAt(pos, target, up)` (+344); active camera pointer (+336).

## CreateRenderPasses 0x14002BD60
Creates (target member; reconstructed in src/app/FeatureDemo_Scene.cpp):
- `ForwardShadingPass2018` (+776; 0x1400840B0, passes/forward_vs/ps, SINGLE_PASS_STEREO) on HdrFramebuffer
- `gbuffer_ps.hlsl main` (+664), `deferred_lighting_ps.hlsl main` (+672)
- `GBufferFillPass2018` (+848; 0x140087630, forward_gs/cubemap_gs, MOTION_VECTORS); G-buffer binding layout (+680) and set (+688:
  depth, GBuffer0..2) for the deferred lighting pass
- `DeferredLightingPass2018` (+840; 0x14007BDC0)
- PlanetSet render passes (+1160; 0x140007910), SunDisk (+1168; 0x140006C80), textured starfield `fx::EnvironmentMapPass`
  (+800; 0x140064D10), real stars `fx::RealStarFieldPass` (+824; 0x140066020)
- `TemporalAntiAliasingPass` (+880; 0x140094C10, includes motion vectors), pixel SSAO pass (+856; 0x140089E90; created, never
  rendered: HBAO+ is used), Fog (+920; 0x140019CE0), Particles (+928; 0x140067DA0, previous particle buffer reused), Shield
  (+960; 0x14006A2B0), LensFlare (+936; 0x1400637E0), HQ blit "Upscale" (+944; 0x14006AF60),
  `accumulation_ps.hlsl main` (+736, never used), tone mapping (+832; exposure buffer of the previous instance reused, else the
  exposure is reset; colour LUT +1072), `BloomPass` (+888; 0x14008E320)
- +704 / +712 / +720 / +728 are CommonRenderPasses blit binding sets (LdrColor, HdrColor, ResolvedColor1/2), not framebuffers
- five asteroid renderers built by 0x14001BF90 (+248 gbuffer_ps GBuffer, +264 no PS Depth, +280 material_id_ps MaterialId,
  +296 forward_ps Forward with transparentPass, +312 forward_ps Forward ship overlay) with IS_SHIP and _ASTEROIDS permutation macros
- finally +952 (ShowCubemap) is reset; RenderOverlay creates it lazily

## RenderScene 0x1400342A0 (order)
frame counter++; get window size; if aaMode==3 (upscale), round the render size to /1.5 and align to 8; SetupViewsAndTargets (0x140031EF0);
timestamp; pipeline-stats readback (0x140041260); commandList->open; scene Animate (0x14005FE50);
clear back buffer; sun direction: axis = sph(UI+8 azimuth, UI+4 elevation), start = sph(azimuth - 90, 0), rotated about the axis by
UI+12 degrees, sun.direction = -that (y clamped to <= -1e-5) → light (+984); irradiance = planets->GetSunVisibility (0x140008070) * UIData+288;
UpdateLights (0x140031980); if exposure reset: tone mapping ResetExposure(0.05) (0x1400949D0); UpdateScene (0x140038380);
UpdateViewDistanceMap (0x1400384F0, camera height = view origin y - (+644), g_MaxSceneDistance, fog UIData+236);
MeshletDrawStrategy::ResetFrameStatistics (0x1400448B0); UpdateAsteroidRendererSettings (0x140037370);
RenderShadows (0x140034F70) when UIData+120 is set, else reset the sun's shadow pointer and UIData+316; ClearRenderTargets (0x14004B270);
RenderGBuffer (0x140032630); stats → UIData+292..360; RenderLightingAndEffects (0x140033CA0); commandList->close/open;
AA: when UIData+121 or aaMode not in 1..3 then resolve, else accumulation/TAA (0x14002B2A0);
post (0x140033960); aaMode==3 → upscale (0x14006B630, "Upscale"), else blit or sharpen (UIData+108 sharpness) to the back buffer;
overlay (0x1400321F0); stats resolve (0x140044900); tone-mapping frame end (0x140094A00); close and execute.
Frame time EMA → UIData+312. Optional readback of numCulled (UIData+125). Picking: material id readback (+912) → UIData+280.
TAA AdvanceFrame (0x140096290); swap the previous/current buffers (+768 targets, +720/+728, +608/+624 views, +640 → +652).
Vsync from UIData+112. Loading-time log.

## Members (partial)
+8 DeviceManager*, +24 TextureCache, +48 CommonRenderPasses, +96 bool, +112 DeviceManager* (ctor a2), +120 ShaderFactory,
+136 root FS, +152 media path, +184 SpaceScene, +248..+312 asteroid renderers, +328 pipeline-stats query, +336 active camera,
+344 FPSCamera, +584 camera presets, +608/+624 views (cur/prev), +640/+652 view origin (= -camera position) cur/prev,
+664/+672 shaders, +680/+688 G-buffer binding layout/set, +696 command list, +704/+712/+720/+728 blit binding sets,
+736 accumulation shader (unused), +768 render targets (0x14004A2C0), +776 forward pass, +800 environment map (textured starfield),
+808 RealStarFieldResources, +824 real star field pass, +832 tone mapping, +840 deferred lighting, +848 G-buffer pass,
+856 SSAO pass (never rendered), +864 light-probe processing, +880 TAA, +888 bloom, +896 HBAO+, +904 material-id pass (never
created), +912 material-id readback (never created), +920 fog, +928 particles, +936 lens flare, +944 upscale pass,
+952 ShowCubemap (probe debug view), +960 shield, +968 CascadedShadowMap, +984 sun light,
+1000 lights, +1160 planet, +1176 ViewDistanceMap, +1184/+1192 load-timing timestamps, +1200 UIData*, +1264 bool pick request,
+1272 replay state, +1320/+1400 animation players.

## LoadScene 0x140031280 (fs, sceneFile) → bool
- A task group (concurrency) for parallel loading. Wait on it before returning.
- `m_Planets` (+1160) = new PlanetSet (0x140006C60); `Load(fs, media/PlanetFx/planets.json, textureCache, mediaPath, tasks)` (0x140008740).
- Sky texture (+1040) = `textureCache->LoadTextureFromFileDeferred(media/SkyAndStars/loc00180_8_Space_Sky_2k-ExposureCorrected-16bit.dds, sRGB=false, tasks)` (0x1400728E0).
- Randoms texture (+1056) = deferred load `media/randoms_texture.dds`.
- Unless -renderLightProbes: load the light probes (0x140030D40: LightProbeDiffuse.dds, LightProbeSpecular.dds, EnvironmentBrdf.dds).
- `m_Scene` (+184) = `SpaceScene::Load(fs, sceneFile, textureCache)` (static, 0x14005C890). Return `m_Scene != nullptr`.

## SceneLoaded 0x140036540
- Color LUT (+1072): when the scene has a `color_lut` (scene+352), `textureCache->LoadTextureFromFile(media/<lut>, sRGB=false)` (synchronous, 0x140072C40), else an empty LoadedTexture.
- `m_OpaqueDrawStrategy` (+200) = `InstancedOpaqueDrawStrategy(scene)`; `m_TransparentDrawStrategy` (+216) = `TransparentDrawStrategy(scene)`.
- `textureCache->ProcessRenderingThreadCommands(commonPasses, 0)`; `scene->CreateRenderingResources(device, commonPasses, true)` (SpaceScene vtable slot 3).
- `g_MaxSceneDistance` (0x1402D10B4) = max(it, scene->GetBoundingRadius() (0x14005C740)).
- Sun: the first scene light whose `GetLightType()==Directional` → +984. Otherwise create a DirectionalLight with colour normalize(112,127,162)
  (i.e. /sqrt(54917)) and angular size 4.25 degrees, and add it to the scene's lights (scene+432 vector).
- `m_ThirdPersonCamera` (+576) = new ThirdPersonCamera(scene->GetPlayerShip() (0x14005C810), …, 0.4f) (0x140062170). Active camera = it.
- `m_MeshletResources` (+232) = MeshletRenderResources(device) (0x140040680), created once.
- -renderLightProbes: create the probe cubemap arrays (0x14002B590(3): diffuse 128^2, specular 256^2 with 8 mips, RGBA16F);
  0x14002B920(3) always creates the three DemoLightProbe objects (+1088). Per probe (+176 capture position, +188/+192 height band,
  +196 tint): probe0 (10000, 5000, 10000) [1000, 50000] (1,0,0,0); probe1 (10000, -1000, 10000) [-5000, 1000] (0,0,1,0);
  probe2 (10000, -9000, 10000) [-50000, -5000] (0,1,0,0).
  -renderLightProbes → render the probes (0x1400327F0) and save them (0x140035D40).
- If no -view: UIData[0] (show UI?) = !benchmark; UIData+364 = 1.
- Animation player = benchmark ? +1320 : +1400 → +1480, time = -1.
- If !UIData+371: scene->ships(+416)[0]->vfunc@152(0, 0).

## SetupViewsAndTargets 0x140031EF0 (width, height, framebuffer, bool* needNewPasses)
- If there are no render targets (+768) or their size differs (0x14004B3C0): `commonPasses->ResetBindingCache()` (0x14007B6A0);
  release the scene meshes' cached binding sets (scene+120 list, 0x14005C260); recreate `RenderTargets(device, size, sampleCount…)` (0x14004A2C0);
  framebuffers +720 = FB(targets[10]), +728 = FB(targets[11]) (0x14007B140); changed = true.
- `changed |= UpdateViews()` (0x140037C50).
- UIData+113 (reload shaders): `shaderFactory->ClearCache()` (0x140074710) and reset the pipelines of the asteroid renderers +248,+312,+264,+280 (0x1400412D0).
- If changed or reloading: `CreateRenderPasses(needNewPasses, framebuffer)` (0x14002BD60), `device->waitForIdle()`, `device->runGarbageCollection()`. Clear UIData+113.

## RenderLightingAndEffects 0x140033CA0
1. 0x1400377C0 (prepare). Ambient = sun irradiance (+984 → +84) * UIData ambient color (+16,+20,+24) * 0.01, or 0 when UIData+168 is set.
2. `DeferredLightingPass::Render` (+840, 0x14007CA30): view (+608), lights (+1000), light probes (+688), random offset (2 x rand*32767 from MT state 0x1402D1100), ambient top/bottom.
3. Environment/sky pass (+800, 0x1400658B0 "Environment Map"). When UIData+160: second environment pass (+824, 0x1400671F0).
4. Sun disk (+1168, 0x140008E80) and planets (+1160, 0x140008870; g_MaxSceneDistance, UIData+368).
5. When UIData+29: HBAO+ (+896, 0x140097230; parameters UIData+32; depth = targets[0], normals = targets[4], output = targets[5]).
6. When UIData+224: fog (+920, 0x14001AE60; jitter from MT rand, camera offset (+640..+648), sun, UIData+236, UIData+196).
7. When UIData+225: particles (+928); if UIData+232, reset (0x140069EA0) and clear the flag; update (0x14006A130).
8. When UIData+369: forward pass (+776, `ForwardShadingPass`, 0x140085A60) with asteroid forward renderer +296. When UIData+126 and +312 exists: forward pass with +312.
9. When UIData+370: shield (+960, 0x14006AB40) around the player ship (pos = ship + camera offset; dir +1492; radius 30.0f; strength +1488; time +100).

## RenderShadows 0x140034F70
- `csm->Clear` (0x140081200); sun->shadowMap = csm.
- Fit cascades to the view: when UIData+208 (stable/ortho variant) use 0x140081CA0 with the view's inverse view/projection (cascade split exponent UIData+196, maxDistance UIData+200 * (2 - |sunDir.y|), UIData+204), else 0x1400815A0.
- Per-object shadow (index 0) around the player ship's world bounds (0x14005C820 ship, 0x140049180 bounds), offset by the camera position (0x140082500).
- `csm->SetLitOutOfBounds(true)` (0x1400814F0); `csm->SetFalloffDistance(0.5f)` (0x140081440).
- Stats begin (0x140041170, slot 0). Depth renderer +264 flags {+336=1,+337=0,+338=1}: render the cascades (0x1400812C0).
  Flags {0,1,0}: render the per-object shadows (0x140081360). Stats end (0x1400411C0). Shadow time EMA → UIData+316.

## Other RenderScene helpers
- 0x140038380 UpdateScene(cmd): per scene object 0x140051CC0 (transform update); if !scene+384, scene->CreateResources(device) (0x14005B3D0);
  scene->Update(cmd) (0x14005FDD0); asteroid library upload (scene+120 → 0x1400126A0 with cmd); 0x14005FF00.
- 0x1400384F0 UpdateViewDistanceMap(cmd, fogParams=UIData+236, …): builds the 1D LUT (width = ViewDistanceMap.width) with expf falloff and a
  12-step bisection per texel, then `cmd->writeTexture(ViewDistanceMap, 0, 0, data, rowPitch)`.
- 0x140037370 UpdateAsteroidRendererSettings: for the 5 asteroid renderers (+248,+264,+280,+296,+312) copy UIData fields into renderer fields:
  r+253←UI+122, r+255←UI+123, r+254←UI+127, r+260←UI+104, r+264←UI+180, r+272←UI+128, r+280←UI+184, r+284←UI+132+lodBiasAdj,
  r+288←UI+140, r+268←UI+188, r+328←view(+608), r+340←cameraPos(+640), r+352←prevCameraPos(+652), r+364←time(+100);
  lodBiasAdj = (1 - UI+140) * 9. Then: gbuffer(+248).276←UI+124, .257←UI+125, .368←UI+216, .252←UI+121; materialId(+280).252←UI+121;
  depth(+264).280=0, materialId.280=0, depth.284 = adj + UI+136; ship(+312).252=1, .256←UI+126; gbuffer/ship .336←UI+367, .337/.338←UI+369.
- 0x14004B270 ClearRenderTargets(cmd): clear depth (targets[0]) and colour targets [6],[5],[2],[3],[4],[7] (clear value 0x1402D3910).
- 0x140032630 RenderGBuffer: stats slot 1; geometry pass (+848, render fn 0x1400890D0, same as the material-id pass +904) drawing the
  gbuffer asteroid renderer +248 into the gbuffer targets [2..4], views +608/+624; time EMA → UIData+320.
  (+832 is the tone-mapping pass: 0x140094240 frame begin, 0x1400949D0 ResetExposure, 0x140094A00 frame end,
  0x140094A50 Render with UIData+52; ctor 0x140094290.)
- 0x14002B2A0 ResolveOrAccumulate(viewport): TAA (+880) motion vectors (0x140096380, views cur/prev); when +96 (previous views valid)
  TAA resolve (0x140096BC0, params UIData+88 = {newFrameWeight, clampingFactor, enableHistoryClamping}), otherwise (first frame) blit
  HdrColor (targets[5]) into ResolvedColor1 (targets[10]); then blit the resolved colour into BloomColor (targets[12]), which feeds
  PostProcess. Reconstruction: donut TemporalResolve with feedbackIsValid = +96 replaces the first-frame blit.
- 0x140033960 PostProcess(hdrSrc, ldrDst, outFb) → final texture: when UIData+225, particles render (+928, 0x140069250; stats slot 2;
  sun, camera pos, 5000.0f, UIData+228, UIData+226). When UIData+114: bloom (+888, 0x140090420; sigma = height/1080 * UIData+116).
  When UIData+233: lens flare (+936, 0x140064170). When UIData+144: tone mapping (+832, `ToneMappingPass::SimpleRender`/Render 0x140094A50,
  params UIData+52) into targets[6] (+48), which becomes the result.
- 0x1400321F0 RenderOverlay(viewport, fb): picking (when +1264 and +904; dead code, see below): clear targets[8] (+64), material-id pass
  (+904, 0x1400890D0) with renderer +280, readback (+912, 0x1400978C0, pixel +1256). When UIData+189: draw the shadow-map cascades as 256 px thumbnails
  (commonPasses blit 0x14007B0A0, slice i, starting at x = 10 + 266*i, bottom-left). When UIData+190 and probe UIData+192 is valid:
  `ShowCubemap` pass (+952, 0x14004C750 / render 0x14004CFC0) of the probe's specular cubemap, mip UIData? (probe+60), at (10, h-266)-(266, h-10).

- 0x1400377C0 CollectLightProbes(out vector): when UIData+168, the probes (+1088) get diffuse/specular scales = sun irradiance * UIData+172/+176.
  When UIData+169, their bounds are recomputed around the scene radius and the camera height (+644): box from (-r, cam.y+probe.y+2000…),
  scaled by 0.0005 for the blend falloff. Otherwise only probe[0], with infinite bounds (0x1401EEBC0).
- 0x140037C50 UpdateViews → bool created: active camera = UIData+220 (0 FPS +344, 1 third-person +576); create PlanarViews +608/+624 if
  missing; viewport from the render-target size; pixel offset = TAA jitter (+880) when aaMode==1 && !UIData+121;
  projection = perspective(fov, aspect, zNear, g_MaxSceneDistance) (0x1401EDDD0); view matrix from the camera; camera-relative
  rendering: cameraPos (+640) = -translation; prevCameraPos (+652) is initialised on creation.

## Dead code in the shipped binary (from the passes/meshlet reconstruction)
- The material-id picking pass (+904) and its readback (+912) are never created, and render target [8] is never created.
  RenderOverlay's picking branch is therefore unreachable. +904 is NOT the light-probe capture.
- `accumulation_ps` (+736) is loaded but never used: 0x14002B2A0 only does TAA or the first-frame blit.
- All geometry, including the light-probe capture, is drawn through MeshletDrawStrategy instances. The 2018 passes' input-assembler
  pipelines for regular meshes are created (G-buffer) but never used.
- The Hi-Z texture (targets[1]) must have 5 mips (R32_FLOAT, 1/8 resolution, UAV) for create_hi_z_cs.

## Function names used by the reconstruction (src/app)
| address | name | file |
|---|---|---|
| 0x14002BD60 | `CreateRenderPasses` | FeatureDemo_Scene.cpp |
| 0x14002B590 | `CreateLightProbeTextures` | FeatureDemo_Scene.cpp |
| 0x14002B920 | `CreateLightProbeObjects` | FeatureDemo_Scene.cpp |
| 0x1400327F0 | `RenderLightProbes` (probe capture, -renderLightProbes) | FeatureDemo_Scene.cpp |
| 0x140031280 / 0x140036540 | `LoadScene` / `SceneLoaded` | FeatureDemo_Scene.cpp |
| 0x140031EF0 | `SetupViewsAndTargets` | FeatureDemo_Render.cpp |
| 0x140037C50 | `UpdateViews` | FeatureDemo_Render.cpp |
| inlined in 0x1400342A0 | `UpdateSun` | FeatureDemo_Render.cpp |
| 0x140031980 | `UpdateLights` | FeatureDemo_Render.cpp |
| 0x1400377C0 | `CollectLightProbes` | FeatureDemo_Render.cpp |
| 0x140038380 | `UpdateScene` | FeatureDemo_Render.cpp |
| 0x1400384F0 | `UpdateViewDistanceMap` | FeatureDemo_Render.cpp |
| 0x140037370 | `UpdateAsteroidRendererSettings` | FeatureDemo_Render.cpp |
| 0x140034F70 | `RenderShadows` | FeatureDemo_Render.cpp |
| 0x14004B270 | `RenderTargets::Clear` | RenderTargets.cpp |
| 0x140032630 | `RenderGBuffer` | FeatureDemo_Render.cpp |
| 0x140033CA0 | `RenderLightingAndEffects` | FeatureDemo_Render.cpp |
| 0x14002B2A0 | `ResolveOrAccumulate` | FeatureDemo_Render.cpp |
| 0x140033960 | `PostProcess` | FeatureDemo_Render.cpp |
| 0x1400321F0 | `RenderOverlay` | FeatureDemo_Render.cpp |
