# src/fx: space-FX render passes

These are reconstructions of the 2018 render passes that use the space/FX shaders in `asteroids/shaders/demo` (see `shaders/demo/space_shaders.NOTES.md`). They are built on donut main and its nvrhi.

Each pass follows the donut pattern:
* the constructor creates the shaders, layouts and pipelines;
* `Render(commandList, compositeView, …)` loops over the planar child views;
* constant buffers are volatile, built with `fx::ConstantBufferDesc`;
* shader paths are `demo/<file>.hlsl`, as required by CONVENTIONS.md.

Compile check: the private build in `recon/build_fx` (VS 2022 x64, Release, target `asteroids_core`) compiles every file in this folder with no errors and no warnings, apart from donut's code-page warning C4819.
* The target as a whole still fails because of one error in another agent's file: `src/app/InputReplay.h(39)`, `static_assert '2018 replay frames are 32 bytes'`.
* Configure needs the already downloaded FetchContent sources: `-DFETCHCONTENT_SOURCE_DIR_{DXC,DIRECTX_HEADERS,LZ4,SQLITE3}=recon/build/_deps/*-src`. The DXC download from GitHub fails in this environment.

## Class map

| 2018 (Asteroids.exe) | Addresses | Reconstruction | FeatureDemo member |
|---|---|---|---|
| RectPass (shared quad pass) | ctor 0x14000A500, Render 0x14000B330 | `fx::RectPass` (RectPass.h/.cpp) | owned by SunDisk / PlanetSet |
| SunDisk | ctor 0x140006C80, Render 0x140008E80 | `fx::SunDisk` (SunDisk.h/.cpp) | +1168 |
| PlanetSet (planets.json) | ctor 0x140006C60, CreateRenderPasses 0x140007910, Load 0x140008740, LoadPlanet 0x1400081F0, Render 0x140008870, GetSunVisibility 0x140008070 | `fx::PlanetSet`, `fx::Planet` (PlanetSet.h/.cpp) | +1160 |
| Environment map / textured starfield ("Environment Map") | ctor 0x140064D10, Render 0x1400658B0 | `fx::EnvironmentMapPass` (EnvironmentMapPass.h/.cpp) | +800 |
| RealStarFieldResources (RTTI) | ctor 0x140066BF0, loader 0x1400670E0 | `fx::RealStarFieldResources` (RealStarField.h/.cpp) | +808 |
| Real-star pass (reuses the "Environment Map" marker) | ctor 0x140066020, Render 0x1400671F0, celestial time 0x140066ED0 | `fx::RealStarFieldPass` (RealStarField.h/.cpp) | +824 |
| Fog | ctor 0x140019CE0, Render 0x14001AE60 | `fx::FogPass`, `fx::FogParameters` (FogPass.h/.cpp) | +920 |
| ShowCubemap (probe debug view) | ctor 0x14004C750, Render 0x14004CFC0 | `fx::ShowCubemapPass` (ShowCubemapPass.h/.cpp) | +952 |
| LensFlare | ctor 0x1400637E0, Render 0x140064170 | `fx::LensFlarePass` (LensFlarePass.h/.cpp) | +936 |
| HQ blit / "Upscale" | ctor 0x14006AF60, Render 0x14006B630 | `fx::HqBlitPass` (HqBlitPass.h/.cpp) | +944 |
| Particles | ctor 0x140067DA0, Render 0x140069250, Update 0x14006A130, Reset 0x140069EA0 | `fx::ParticleSystem` (ParticleSystem.h/.cpp) | +928 |
| Shield | ctor 0x14006A2B0, Render 0x14006AB40 | `fx::ShieldPass` (ShieldPass.h/.cpp) | +960 |
| (shared helpers) | 0x14000C200 (buffer desc), 0x14000BD90 (blend state) | `fx::ConstantBufferDesc`, `fx::BlendStateRT`, `fx::FillLightConstants2018`, `fx::FillShadowConstants2018` (FxCommon.h/.cpp) | n/a |

Of the class names, RTTI attests only `RealStarFieldResources`. The other names come from strings or markers ("LensFlare", "Fog", "Upscale", "Particles", "PlanetFx", "Environment Map", the constant-buffer names), or are invented.

## Public API and the arguments FeatureDemo passes

Abbreviations: `fd+N` is a FeatureDemo member offset, `UI+N` is a UIData field, `targets` is the RenderTargets object at fd+768, `view` is the composite view at fd+608, `cmd` is the command list at fd+696, and `sun` is the SceneDirectionalLight at fd+984.

### RectPass

| Call | Arguments |
|---|---|
| Constructor | `RectPass(device, shaderFactory, pixelShaderFile, pixelBindingLayoutDesc, blendState, framebufferFactory, compositeView)` |
| `Render` | `Render(cmd, view, direction, angularSizeDegrees, distance, pixelBindingSet)` |

* `Render` builds a camera-facing quad at `min(cos(a/2), 0.9) * distance` along `direction`, with half-size `tan(a/2) * dist`. The up axis is `(0,1,0)`, or `(1,0,0)` when the direction is nearly vertical.
* It fills `RectConstants` (clip corners from the translation-free view-projection, plus normalized corner directions), and collapses the viewport depth range onto the far plane.
* The pipeline is a triangle strip, depth test LessEqual (GreaterEqual with reverse Z), no depth write, no culling.

### SunDisk

| Call | Arguments |
|---|---|
| Constructor | `SunDisk(device, fd+120 shaderFactory, targets framebufferFactory, view)` |
| `Render` | `Render(cmd, view, sun, g_MaxSceneDistance, 1.0f)` |

* Called from RenderLightingAndEffects (0x140033CA0).
* `SunConstants.color = sun.color * irradiance * 10 * brightness`.
* Drawn as a RectPass around `-normalize(sun.direction)` with size `2 * sun.angularSize`. Blending is premultiplied (One / InvSrcAlpha).

### PlanetSet

| Call | Arguments |
|---|---|
| `Load` | `Load(fd+136 fs, "media/PlanetFx/planets.json", fd+24 textureCache, fd+152 mediaPath, threadPool)`, from LoadScene 0x140031280 |
| `CreateRenderPasses` | `CreateRenderPasses(device, fd+120, targets framebufferFactory, view, fd+48 commonPasses)` |
| `Render` | `Render(cmd, view, sun, g_MaxSceneDistance, 1.0f, UI+368 withRings)` |
| `GetSunVisibility` | `GetSunVisibility(-sun.direction, sun.angularSize)`, from RenderScene 0x1400342A0 |

* `Load` reads one JSON array of planets.
  * Keys read: Azimuth, Elevation, AngularSize (0 is treated as 30), RadiusRatio, AtmosphericAlpha, LitBrightness, AmbientBrightness, AtmosphereBrightness, Rotation, StretchAmount, InnerRingRadius, AtmosphereRingsRatio, RingsElevation, RingsAzimuth, SurfaceMap (sRGB), NormalMap and RingsPattern.
  * planets.json also has `AtmosphereRadius` and `VerticalComponent`; the exe never reads them.
* `CreateRenderPasses` builds two RectPasses: `PlanetFx_on_ps` and `PlanetWithRingsFx_on_ps`.
* `Render` computes PlanetConstants per planet and view:
  * texture LOD from `log2(texWidth / projected pixel size)`;
  * atmosphere brightness boost `9 * exp(-4 * max(0, angle to sun - size)) + 1`;
  * ring plane and squared ring radii from RingsAzimuth/Elevation and AtmosphereRingsRatio.
* RenderScene uses the visibility result as `sun.irradiance = GetSunVisibility(…) * UI+288`, so a planet in front of the sun dims the scene.

### EnvironmentMapPass

| Call | Arguments |
|---|---|
| Constructor | `EnvironmentMapPass(device, fd+120, fd+48, targets framebufferFactory, view, sky texture (fd+1040), starMap = nullptr)` |
| `Render` | `Render(cmd, view, UI+148 nebulaLinearBrightness, 0, 0)` |

* FeatureDemo creates it only after the sky DDS has loaded.
* `Render` is called every frame right after deferred lighting.
* This is the demo's variant of donut's EnvironmentMapPass, running `texturedstarfield_ps`. FeatureDemo always passes a null star map, so the star layer cannot be reached in the shipped exe.

### RealStarFieldResources and RealStarFieldPass

| Call | Arguments |
|---|---|
| Resources | `make_shared<RealStarFieldResources>(device, fs, mediaPath)`, in the FeatureDemo ctor |
| Pass constructor | `RealStarFieldPass(device, fd+120, fd+48, targets framebufferFactory, view, fd+808)` |
| `Render` | `Render(cmd, view, UI+164 brightness)`, only when UI+160 is set |

* The resources read `SkyAndStars/Stars.buf` and create the "Star instance buffer".
* The pass draws `numStars / 64` instances of 192 vertices with additive blending (One, One).
* `CelestialConstants.ST/SinLAT/CosLAT` are computed (GMST per Meeus) from a fixed date of 2018-08-01 12:00 and a fixed location of 121.93° W, 37.35° N, both hard-coded in the exe. The shader ignores them.

### FogPass

| Call | Arguments |
|---|---|
| Constructor | `FogPass(device, fd+120, fd+48, targets framebufferFactory, targets[0] depth, view)` |
| `Render` | `Render(cmd, view, fd+640 cameraOffset, sun, *(FogParameters*)(UI+236), float3(r3, r2, r1) * 32767, UI+196 maxShadowDistance)` |

* `Render` is called when UI+224 is set. The three random values come from the MT state at 0x1402D1100.
* The constructor creates a half-resolution trace target and two pipelines: `ps_trace` into the internal target, and `ps_filter`, premultiplied, into the view framebuffer.
* `FogParameters` is the 36-byte block at UI+236. Defaults: meanHeight −3000, thickness 4000, densityScale 0.0005, lightIntensityScale 1, visibilityThreshold 0.0001, color (0.667, 0.742, 1), intensity 1.5.
* `noisePattern` is the 4×4 Bayer pattern from .rdata.

### ShowCubemapPass

| Call | Arguments |
|---|---|
| Constructor | `ShowCubemapPass(device, fd+120, fd+48, overlayFramebuffer, probe specular cubemap (probe+40))` |
| `Render` | `Render(cmd, overlayFramebuffer, Viewport(10, 266, h−266, h−10), probe+60 arrayIndex, 0 mip)` |

* Created lazily in RenderOverlay 0x1400321F0 when UI+190 is set and UI+192 is a valid probe index.

### LensFlarePass

| Call | Arguments |
|---|---|
| Constructor | `LensFlarePass(device, fd+120, fd+48, hdr framebufferFactory, view)` |
| `Render` | `Render(cmd, view, PostProcess framebufferFactory, sun, fd+640 cameraPosition)` |

* `Render` is called from PostProcess 0x140033960 when UI+233 is set.
* LensFlareConstants: light plus 5 shadow slots (cascades, then per-object shadows), and the sun projected from `-1e6 * direction`.
  * `irradiance = sun.irradiance * saturate(2 - |x|) * saturate(2 - |y|)`.
  * The pass draws only when the sun is in front of the camera and the irradiance is above 0.
* Blending is additive (One, One).

### HqBlitPass

| Call | Arguments |
|---|---|
| Constructor | `HqBlitPass(device, fd+120, fd+48, output framebuffer)` |
| `Render` | `Render(cmd, back buffer framebuffer, output viewport, PostProcess result texture)` |

* `Render` replaces the blit/sharpen step in RenderScene when aaMode (UI+100) is 3.

### ParticleSystem

| Call | Arguments |
|---|---|
| Constructor | `ParticleSystem(device, fd+120, fd+48, targets framebufferFactory, view, 250000, float3(5000, 2000, 5000), previous instance's GetParticleBuffer())` |
| `Render` | `Render(cmd, view, targets framebufferFactory, sun, fd+640 cameraOffset, 5000.0f, UI+228 particlesPerBatch, UI+226 useMeshShader)` |
| `Update` | `Update(cmd, fd+100 time, fd+104 deltaTime)` |
| `Reset` | `Reset(cmd)` |

* Passing the previous particle buffer to the constructor keeps the simulation running across pass rebuilds.
* `Render` is called from PostProcess with pipeline-stats slot 2. It draws one constant-buffer update per volume tile that overlaps the view frustum.
* `Update` and `Reset` are called from RenderLightingAndEffects when UI+225 is set. `Reset` runs first when UI+232 is set, and FeatureDemo then clears that flag. `deltaTime` is clamped to 1/30.
* `Reset` uses mt19937 with the default seed: `position = r * volume`, `brightness = 2r + 0.1`.
* The mesh path uses `demo/particles_ms.hlsl` `ms_main` with `demo/particles.hlsl` `ps_main`. The VS path uses `vs_main` / `ps_main`, and the update uses `update_cs_main`.

### ShieldPass

| Call | Arguments |
|---|---|
| Constructor | `ShieldPass(device, fd+120, fd+48, targets framebufferFactory, view, targets[0] depth)` |
| `Render` | `Render(cmd, view, shipWorldPos + fd+640 cameraOffset, fd+1492 direction, 30.0f radius, fd+1488 intensity, fd+100 time)` |

* `Render` is called when UI+370 is set. It is a full-screen pass (`m_FullscreenVS` with `shield_ps`) with premultiplied blending, and it skips the draw when the intensity is 0 or less.
* The constants are `surface2018::ShieldConstants` from the surface agent's `surface_cb.h`.

## Deviations (project-wide for src/fx)

* **RectPass binding layouts.** The 2018 nvrhi used one binding layout with per-stage item lists. In nvrhi main a layout has a single visibility, and both RectPass stages bind b0. So the RectPass owns a vertex layout/set, and each owner passes a pixel layout description and its own set.
* **2018 LightConstants vs donut's.** The fx translation units use the 2018 `LightConstants` and `ShadowConstants` from `shaders/demo/include/light_cb.h`. `SceneLight::FillLightConstants` (src/scene/Lights.cpp) writes donut main's 112-byte `LightConstants`. `fx::FillLightConstants2018` calls it through a padded temporary and copies the first 96 bytes, which have identical layout.
  * There are two different global `struct LightConstants` definitions in the program: the 2018 one in fx TUs, and donut's in Lights.cpp and the surface TUs. They live in separate TUs and every access is layout-correct, but strictly this is an ODR violation. See the open questions.
* **Shadow constants.** These are filled through donut's `IShadowMap::FillShadowConstants`, whose `ShadowConstants` has exactly the 2018 layout. Cascade `i` goes to `shadows[i]` with `shadowCascades[i] = i`; LensFlare appends the per-object shadows after the cascades.
* **Binding caches.** The 2018 code cached binding sets in `unordered_map`s keyed by shadow-map or source texture. The rebuild uses `std::map`/`unordered_map` keyed the same way, or a donut `BindingCache` (Fog).
* **No-shadow fallback.** Without a shadow map, passes bind `commonPasses->m_BlackTexture2DArray`, which was commonPasses+272 in 2018.
* **Mesh-shader particles.** These follow the global mesh-shader mode. NVAPI mode uses the original-style `demo/nvapi/particles.hlsl` `ms_main` with `g_NvidiaExt` at u0 (2 PSO extensions). D3D12 mode uses an nvrhi `MeshletPipelineDesc` with `demo/particles_ms.hlsl` `ms_main` (SM 6.5). With neither, Render falls back to the VS path.
* **LensFlare buffer size.** The 2018 code wrote 768 bytes of LensFlareConstants, but the HLSL cbuffer is 804 bytes because each `pad[3]` element takes a register. The rebuild writes `sizeof(LensFlareConstants)`.
* **Texture loading.** PlanetSet loads its textures with `TextureCache::LoadTextureFromFileAsync` and a donut ThreadPool (or Deferred without one), replacing the 2018 concurrency task group.
* **Missing Stars.buf.** RealStarFieldResources logs a warning when Stars.buf is missing and creates a one-record buffer; the 2018 code left the list empty.

## Open questions

1. **LightConstants unification across agents.** `surface_cb.h` takes the C++ side from donut's light_cb.h, with the 2018 struct in `namespace surface2018`. `space_cb.h` and `particles_cb.h` embed the global 2018 `LightConstants` from `demo/include/light_cb.h`. A TU can use one family or the other, not both. A project-level fix would be either:
   * a C++-only `surface2018`-style namespace in light_cb.h, or
   * making `SceneLight::FillLightConstants` emit the 2018 layout.

   Both options touch frozen or other agents' files, so the decision is the coordinator's.
2. **CommonRenderPasses members.** The 2018 members are mapped by role:
   * +200 → `m_FullscreenVS`
   * +248 → `m_BlackTexture`
   * +272 → `m_BlackTexture2DArray`
   * +296 → `m_LinearClampSampler`
   * +304 → `m_LinearWrapSampler`

   This is consistent across passes but not verified against the 2018 class layout.
3. **Shadow comparison samplers** (LensFlare, Particles, Fog). They use border addressing, border colour 1 and linear filtering. The 2018 border colour constant `xmmword_140256830` and the comparison function were not decoded; nvrhi main has no compare-function field.
4. **Fog trace target format.** The 2018 format enum value is 34; it is assumed to be RGBA16_FLOAT.
5. **UI+196.** featuredemo_map calls it the CSM split exponent, but Fog uses it as `maxShadowDistance` (default 20000). It may serve both roles.
6. **Kept as compiled:**
   * LensFlare `pos = (-clip.x, clip.y)`, with no ×0.5 or uv conversion.
   * Particle mesh dispatch `numParticles >> 5`, which leaves 16 of 250000 particles undrawn.
   * Particle render constants `time`, `deltaTime` and `volumeSize` are 0.
   * Every planet gets a rings binding set; the rings flag is global (UI+368).
   * ShowCubemapPass is never recreated when a different probe is selected.
7. **Shield inverse matrix.** The 2018 IView slot +144 is assumed to be `GetInverseViewProjectionMatrix` for `ShieldConstants.matClipToTranslatedWorld`.
8. **SunDisk framebuffer factory offset.** The SunDisk report gives the RenderTargets framebuffer factory as targets+120, while the other passes give targets+136. Check this when FeatureDemo is wired.
