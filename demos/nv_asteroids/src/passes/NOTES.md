# src/passes: 2018 framework render passes, demo-side

These three passes host the demo surface shaders (`shaders/demo/gbuffer_ps`, `forward_ps`, `deferred_lighting_ps`,
`material_id_ps`). The 2018 donut versions read the monolithic 2018 cbuffers (`c_GBuffer` 432 B, `c_Forward`
5552 B, `c_Deferred` 5648 B). donut main has different cbuffers and shaders, so the passes are reconstructed here and
donut is not modified. All cbuffer structs come from `shaders/demo/include/surface_cb.h` (namespace `surface2018`)
and `light_cb.h` (namespace `light2018`). Every .cpp includes them after `using namespace donut::math;`.

## Class map

| 2018 (Asteroids.exe) | ctor / Render | here | FeatureDemo member |
|---|---|---|---|
| GBufferFillPass (0x80 B, no RTTI) | 0x140087630 / 0x1400890D0 | `GBufferFillPass2018` | +848 G-buffer; +904 material ID (never created, see below) |
| ForwardShadingPass (0x110 B) | 0x1400840B0 / 0x140085A60 | `ForwardShadingPass2018` | +776; also a local instance in the light probe capture 0x1400327F0 |
| DeferredLightingPass (0xE8 B) | 0x14007BDC0 / 0x14007CA30 | `DeferredLightingPass2018` | +840 (+680 G-buffer layout, +688 set) |
| shared light / shadow / probe loops | inlined in both Render functions | `SurfaceLighting2018.h/.cpp` (`surface_lighting::`) | |
| material-ID readback (+912) | 0x1400978C0 | **donut main `donut::render::PixelReadbackPass`**: same compute pass + copy (cbuffer with the pixel, Dispatch(1,1,1), copyBuffer to staging) | +912 |
| accumulation_ps (+736) | — | no pass: the shader is created in CreateRenderPasses and never used | +736 |

## API and the exact FeatureDemo calls (CreateRenderPasses 0x14002BD60 and the render functions)

`materialLayout` is the nvrhi layout that every `SceneMaterial::bindingSet` was created with
(`CreateMaterialBindingLayout`, 2018 CommonRenderPasses+328). It must be the same layout object, because the strategy
binds `SceneMaterial::bindingSet` against it.

```cpp
// +776
m_ForwardPass = std::make_shared<ForwardShadingPass2018>(device, m_ShaderFactory, m_CommonPasses,
    m_RenderTargets->HdrFramebuffer /* rt[15] */, *m_View /* +608 */, materialLayout,
    /*singlePassStereo*/ false, /*singlePassCubemap*/ false, /*trackLiveness*/ true);

// +664 / +672
std::vector<ShaderMacro> macros = { {"IS_SHIP","0"}, {"_ASTEROIDS","0"} };
m_GBufferPS = m_ShaderFactory->CreateShader("demo/gbuffer_ps.hlsl", "main", &macros, nvrhi::ShaderType::Pixel);
m_DeferredLightingPS = m_ShaderFactory->CreateShader("demo/deferred_lighting_ps.hlsl", "main", nullptr, nvrhi::ShaderType::Pixel);

// +848: { gbuffer_ps, nullptr, stereo 0, cubemap 0, depthWrite 1, motionVectors 1, trackLiveness 0, stencilWriteMask 1 }
GBufferFillPass2018::CreateParameters gbufferParams;
gbufferParams.materialPixelShader = m_GBufferPS;
gbufferParams.enableDepthWrite = true;
gbufferParams.enableMotionVectors = true;
gbufferParams.trackLiveness = false;
gbufferParams.stencilWriteMask = 1;
m_GBufferPass = std::make_shared<GBufferFillPass2018>(device, m_ShaderFactory, m_CommonPasses,
    m_RenderTargets->GBufferFramebuffer /* rt[19] */, *m_View, materialLayout, gbufferParams);

// +680 / +688 (render targets 0, 2, 3, 4)
m_GBufferBindingLayout = DeferredLightingPass2018::CreateGBufferBindingLayout(device);
m_GBufferBindingSet = DeferredLightingPass2018::CreateGBufferBindingSet(device, m_GBufferBindingLayout,
    m_RenderTargets->DepthBuffer, m_RenderTargets->GBuffer0, m_RenderTargets->GBuffer1, m_RenderTargets->GBuffer2);

// +840
m_DeferredLightingPass = std::make_shared<DeferredLightingPass2018>(device, m_ShaderFactory, m_CommonPasses,
    m_RenderTargets->HdrFramebufferNoDepth /* rt[17] */, *m_View, m_DeferredLightingPS, m_GBufferBindingLayout);

// +736: created, never used by the shipped binary
m_AccumulationPS = m_ShaderFactory->CreateShader("demo/accumulation_ps.hlsl", "main", nullptr, nvrhi::ShaderType::Pixel);
```

Render calls:

```cpp
// RenderGBuffer 0x140032630 (stats slot 1): G-buffer meshlet strategy +248, views +608 / +624
m_GBufferPass->Render(commandList, *m_GBufferStrategy, *m_View, m_PreviousView.get());

// RenderLightingAndEffects 0x140033CA0, step 2
m_DeferredLightingPass->Render(commandList, *m_View, m_Lights /* +1000 */, m_GBufferBindingSet,
    randomOffset /* 2 x rand*32767 */, ambientTop, ambientBottom, lightProbes /* CollectLightProbes */, nullptr);

// step 8: when UIData+369, the asteroid forward strategy +296; when UIData+126 and +312 exists, the ship strategy +312
m_ForwardPass->Render(commandList, *m_ForwardStrategy, *m_View, m_Lights, ambientTop, ambientBottom, lightProbes);
```

The 2018 deferred pass draws a full-screen strip with donut's `CommonRenderPasses::m_FullscreenVS`. Its output is
`SV_Position` + `UV`, the same as the 2018 fullscreen_vs.

## Interop with MeshletDrawStrategy (src/meshlets)

The passes call `MeshletDrawStrategy::Render(commandList, MeshletPassState&, view, MeshletMaterialCallback)`. That
replaces the 2018 call `strategy->vfunc0(commandList, &graphicsState, view, materialFilter, attributes)`.

| | G-buffer / material ID | forward |
|---|---|---|
| `framebuffer` | `framebufferFactory->GetFramebuffer(childView)` | same |
| `viewport` | `childView->GetViewportState()` | same |
| `bindingLayouts` | `{ materialLayout, PS layout (b1 c_GBuffer) }` | `{ materialLayout, PS layout (b1 c_Forward, t4 shadow map, s1), PS layout (t5, t6, t7, s2, s3) }` |
| `bindings` | `{ material set, view set }` | `{ material set, forward set, probe set }` |
| `renderState` | set per material by the callback | set per material by the callback |

The callback reproduces the 2018 lambdas 0x140089BE0 (G-buffer) and 0x140086E50 (forward):
- **Binding:** it writes `bindings[0] = material->bindingSet` and returns false when the material has no binding set.
- **G-buffer:** transparent materials are skipped (`return false`). Alpha-tested materials (domain 1) use cull none plus alpha-to-coverage. Everything else is opaque.
- **Forward:** opaque, alpha-tested and transparent each get their own state. Transparent uses premultiplied blending: color One/InvSrcAlpha, alpha Zero/One, no depth writes, and cull none, which is left over from the reused 2018 desc.
- **Common state for both passes:** depth func LessOrEqual (GreaterOrEqual when `view.IsReverseDepth()`; the 2018 code also switched to 7 = GreaterEqual for reverse depth), cull back, `frontCounterClockwise = true` (2018 RasterState byte +762 = 1).
- **G-buffer stencil:** with `stencilWriteMask` set (FeatureDemo uses 1), stencil writes are enabled with read mask 0, write mask = ref = the mask, and pass op Replace.

Points the meshlet owner needs to reconcile:
1. **Pipeline cache key.** `MeshletDrawStrategy::GetOrCreatePipeline` caches by variant and framebuffer info only, but
   the passes change `renderState` per material domain. The forward ship strategy (+312) draws opaque and transparent
   ship materials, so with the current cache it would reuse the first PSO for both. The 2018 strategy copied the
   render state from the pass pipeline that the callback had just selected. Suggested fix: include the render state
   in the cache key, or a pass-provided hash of it.
2. **Asteroid path without a material set.** The asteroid path calls the callback and ignores its result. If an
   asteroid type's material had no binding set, `bindings[0]` would stay null. The 2018 code had the same hole.
   Asteroid materials always have a set in practice.
3. **Order of layouts and sets.** The pass layouts come first, with the material layout at index 0, as in the 2018
   pipeline desc. Space-1 meshlet layouts are appended after them. Registers are explicit, so the order does not
   affect the shaders.

## Light constants

`surface_lighting::FillLights` is the shared 2018 loop:
- **Lights:** up to 16. `SceneLight::FillLightConstants(light2018::LightConstants&)` (2018 Light vfunc1) fills each one.
- **Shadows:** cascades come first, then the light's per-object shadows, as long as fewer than 16 shadows are used in total. Each is filled by donut's `IShadowMap::FillShadowConstants` into `::ShadowConstants`, then copied into the 2018 struct; the layout is identical (static_assert).
- **Probes:** up to 16 active ones, filled by donut's `LightProbe::FillLightProbeConstants` and copied the same way.
- **Probe textures:** every enabled probe must use the same three textures. Otherwise the 2018 error "All lights probe submitted to …Pass::Render(...) must use the same set of textures" is logged and nothing is drawn.
- **Shadow map binding:** the first light with a shadow map supplies the texture and `shadowMapTextureSize`. The other lights are not checked.
- **Forward:** writes `shadowMapTextureSize` only; `shadowMapTextureSizeInv` stays 0, as in 2018.
- **Deferred:**
  - `indirectDiffuseScale` is 1 when an indirect diffuse texture is passed, else 0.
  - `noisePattern` uses the binary's constants (0x140256800..0x140256830), which are identical to donut 2021.
  - `gbufferArraySlice = view.GetSubresources().baseArraySlice`.
  - `matClipToView = inverse(projection)` and `matViewToWorld = inverse(view)`.
  - `cameraDirectionOrPosition = orthographic ? (direction, 0) : (origin, 1)`; forward fills it the same way (for both eyes in stereo).
- **G-buffer:**
  - Takes `matWorldToView` and `matViewToClip` (with offset) from the current view, and `matWorldToClipPrev` from the previous view, without offset.
  - `viewportScale/BiasPrev` comes from the previous view's viewport: scale = ((maxX-minX)/2, -(maxY-minY)/2), bias = (scale.x + minX, (maxY-minY)/2 + minY).
  - `pixelOffset` is the current view's offset only. donut 2021 uses the difference between current and previous; the 2018 code does not.

## Deviations

- **One layout per stage (G-buffer).** The 2018 binding layouts and sets had per-stage sections; the G-buffer layout had VS b0 and PS b1 on one buffer. nvrhi main layouts have a single visibility, so there are separate VS/GS (b0) and PS (b1) layouts and sets. Only the PS layouts go to the meshlet strategy.
- **Forward regular-geometry pipelines.** The 2018 forward ctor also built input-assembler pipelines from
  `passes/forward_vs`, `forward_gs` and the framework `passes/forward_ps` for IDrawStrategy geometry. The demo submits only meshlet
  strategies, including for light probe capture, which uses a meshlet strategy with forward_ps. The framework forward_ps was out of scope, so those
  pipelines are not built.
- **G-buffer regular-geometry pipelines.** `GBufferFillPass2018` does build its 2018 input-assembler pipelines:
  - Shaders: `framework/passes/gbuffer_vs.hlsl` with SINGLE_PASS_STEREO and MOTION_VECTORS, plus `forward_gs` (SPS) or `cubemap_gs`, plus the material PS.
  - Input layout: POS RGB32F, UV RG32F, NORMAL/TANGENT/BITANGENT RGBA8_SNORM, TRANSFORM/PREV_TRANSFORM 3×RGBA32F at offsets 0/48 in a 96-byte instance element (2018 0x14007E6E0).
  - Exposed through `GetOpaquePipeline` / `GetAlphaTestedPipeline` / `GetViewBindingSet`. FeatureDemo never uses them.
- **Fast GS flags:** `ForceFastGS | UseViewportMask` with the NV_X_RIGHT / NV_VIEWPORT_MASK semantics for SPS, and donut main's cubemap GS setup. The 2018 code passed flag value 13 for cubemap; the meaning of its bits in the 2018 nvrhi is not verified.
- **`trackLiveness`:** forwarded to nvrhi's `BindingSetDesc::trackLiveness` in the forward pass. In the G-buffer pass it is stored but ignored (sets always track), because the 2018 FeatureDemo value 0 would only remove safety.
- **Deferred binding sets:** cached with donut's `BindingCache`, which is equivalent to the 2018 hash maps keyed by shadow texture and by probe textures.

## Material-ID pass (+904) and readback (+912)

RenderOverlay 0x1400321F0 would do the following when a pick is requested (+1264) and +904 exists:
1. Clear `rt[8]`.
2. Run `GBufferFillPass::Render` (0x1400890D0) with the material-ID strategy +280 and view +608.
3. Run the readback 0x1400978C0 at pixel +1256.

The constructor 0x1400230B0 and 0x14001AD10 only null +904/+912, and the destructor 0x140026490 frees them. No
code path creates them, and `rt[8]` is never created either (RenderTargets.h: UnusedTexture8). Picking is therefore
dead in the shipped binary.

To revive it:
- Create a `GBufferFillPass2018` with `materialPixelShader = demo/material_id_ps.hlsl (IS_SHIP=0, _ASTEROIDS=0)` and a framebuffer factory on an R32_UINT target + depth.
- Use donut main's `PixelReadbackPass` for +912.

FeatureDemo.h currently labels +904 "m_CubemapGBufferPass light probe capture"; that label does not match the binary.

## Answer: ship emissive intensities in cbObjectInfo (open question 1 of surface_shaders.NOTES.md)

MeshletDrawStrategy's per-object draw 0x140042400 writes 384 bytes of `ObjectConstants` for each space object:
- `bbox` comes from SpaceObject+588..+608.
- `center.xyz` comes from SpaceObject+564/+568/+572.
- `lodBias` comes from SpaceObject+560.

That float4 at +560 is `SpaceObject::shipGlowParams` (src/scene), which CargoShip drives:
- **`lodBias` = `shipGlowParams.x`** is the rear engine glow intensity. It is computed as
  `0.25 * 0.2 * (glow[4..7].intensity summed) * (glow[4].noise * 0.4 + 1)` (CargoShip, 2018 0x14004A020 area). gbuffer_ps
  (IS_SHIP) applies it to the large region [0,1008)² of the 4096² ship texture, which is the engine and exhaust area.
- **`center.x` = `shipGlowParams.y`** is nav-light blink state x, which CargoShip also uses as `navLights[1]->flux / 6`.
  It drives the 34×10-texel region at (211, 2447).
- **`center.y` = `shipGlowParams.z`** is nav-light blink state y (`navLights[0]->flux / 4`). It drives the 17×30-texel
  region at (3898, 1207).

So the "unresolved" note in `shaders/demo/gbuffer_ps.hlsl` / `surface_shaders.NOTES.md` is resolved. Those files are outside
this phase's write scope and were not edited. Suggested comment text:
`lodBias = engine glow, center.x / center.y = nav-light blink states (SpaceObject::shipGlowParams.xyz)`.

## Open questions

- `frontCounterClockwise = true` is inferred from the 2018 RasterState byte layout (+760 fill, +761 cull, +762 front
  CCW, +763 depth clip = 1 by default). It is consistent with the deferred pass (+761 = 2 → cull none), but it was not
  checked against the meshlet winding at runtime.
- The forward and G-buffer depth func uses 2018 value 4 (LessOrEqual), and 7 (GreaterOrEqual) for reverse depth. RenderTargets
  clears depth to 1.0, so the demo uses standard Z.
- The 2018 shadow sampler enum bytes (514 = address modes, 65793 = filters, compare flag) were read as border/white/linear,
  matching donut 2021.

## Build check

`cmake -S recon -B recon/build_passes` (VS 2022, reusing build/_deps via FETCHCONTENT_SOURCE_DIR_*), then
`cmake --build build_passes --target asteroids_core --config Release` (logs in `build_passes/*.log`):
- All four pass TUs (`GBufferFillPass2018`, `ForwardShadingPass2018`, `DeferredLightingPass2018`, `SurfaceLighting2018`)
  compile without errors or warnings, apart from C4819 code-page warnings that come from other headers.
- asteroids_core does not link yet because of one error in another agent's file: `src/app/InputReplay.h(39)`
  static_assert "2018 replay frames are 32 bytes".
- `asteroids_shaders` (ShaderMake): all 58 tasks succeed, including every demo surface shader and every
  `framework/passes/*` permutation.

## Post-processing and light probe passes (2018): ToneMappingPass2018, BloomPass2018, LightProbeProcessingPass2018

| class | binary | FeatureDemo | shaders |
|---|---|---|---|
| `ToneMappingPass2018` | ctor 0x1400925B0 | +832 | `framework/passes/{histogram_cs,exposure_cs,tonemapping_ps}.hlsl` (HISTOGRAM_BINS=256, SOURCE_ARRAY=0) |
| `BloomPass2018` | ctor 0x14008E320, Render 0x140090420 | +888 | `framework/passes/bloom_ps.hlsl` + donut blit |
| `LightProbeProcessingPass2018` | ctor 0x14008B030 | +864 (lazily, RenderLightProbes) | `framework/passes/light_probe.hlsl` |

Constant buffers: `Framework2018Constants.h` includes `shaders/framework/framework_passes_cb.h`
(`framework2018::{LightProbeConstants, ToneMappingConstants, BloomConstants}`).

The C++ of all three is the first public donut (2021) code with these 2018 specifics:
- ToneMapping, FeatureDemo calls through +832:
  - 0x140094290 = `GetExposureBuffer` (kept across pass re-creation);
  - 0x140094240 = `BeginTrackingState(cmd)` at frame start and 0x140094A00 = `SaveTrackedState(cmd)` at frame end:
    2018 nvrhi `beginTrackingBufferState` / `getBufferState` for the histogram (+48) and exposure (+56) buffers;
    replaced by `keepInitialState` (UnorderedAccess), kept as no-ops;
  - 0x1400949D0 = `ResetExposure(cmd, 0.05f)` (clearBufferUInt with the float bits);
  - 0x140094A50 = `SimpleRender(cmd, UIData+52, view, source)`: marker "ToneMapping", clear histogram,
    `AddFrameToHistogram` (0x140093DC0; log2 range [-10, 4]: scale 1/14, bias 10/14), `ComputeExposure` (inlined;
    scale 14, bias -10, low percentile clamped to [0, 0.99], high to [low, 1], speeds, min/max, frameTime),
    `Render` (0x1400942C0; exposureScale = exp2(exposureBias), whitePointInvSquared, color LUT).
  - UIData+52 is donut's `ToneMappingParameters` with the same layout and defaults as the 2018 ctor (0x1400246A0:
    0.8, 0.95, 1, 0.5, 0.02, 0.5, -0.5, 3, true).
  - Fix: the previous FeatureDemo called donut main `ToneMappingPass::Render` only, so the histogram/exposure passes
    never ran and the adapted luminance stayed at the reset value 0.05; it now calls `SimpleRender` like 2018.
- Bloom: `Render(cmd, framebufferFactory, view, source, sigma)`, no blend-factor argument. The 2018 "Apply" pipeline
  (ctor) is the CommonRenderPasses blit (VS +216, PS +224, layout +320) with blend src BLEND_FACTOR(14),
  dest INV_BLEND_FACTOR(15), op add, srcAlpha ZERO, destAlpha ONE, and the fixed blend factor 0.05
  (xmmword_14025DCB0); expressed with donut `BlitTexture` + `blendConstantColor`. Blur: sigma' = clamp(sigma/4, 1,
  100), numSamples = round(4 sigma'), linear clamp sampler (CommonRenderPasses+296); BloomConstants 32 bytes with
  numSamples at offset 16.
- LightProbeProcessing: intermediate cube RGBA16_FLOAT (2018 format 34) 1024^2 full chain; EnvironmentBrdf 64^2
  RG16_FLOAT (2018 format 30); constants buffer debug name "SsaoConstants" (sic); linear wrap sampler
  (CommonRenderPasses+304); diffuse {4096, 1 + 0.5 log2(in^2/4096)}, specular {1024, 1, max(r, 0.01), inputSize};
  each result goes through two mip_ps blits (intermediate mip+1, then the output).

RenderLightProbes (FeatureDemo_Scene.cpp) fixes, from 0x1400327F0 / 0x14007FED0:
- the 2018 `CubemapView::SetTransform` multiplies the transform by diag(1, 1, -1) before the face matrices (same
  face matrices as donut main), so the cube view is now set up with `scaling(1, 1, -1)`; without it every face
  was mirrored and +-Z swapped;
- the forward pass is created with (singlePassStereo 0, singlePassCubemap 0, trackLiveness 1): six planar faces.
  With singlePassCubemap = 1 the meshlet strategy drew no asteroids at all into the probes;
- forward renderer fields as 2018: lodBias -2, transitionRange 0, enableLod/DistanceLod/AsteroidsCulling 1,
  wireframe/showBBoxes/alphaPreset/visualizeLods 0, lodView = the cube view.

Results (build_passes, RelWithDebInfo, this machine's GPU):
- `-renderLightProbes` vs assets/media (ddscmp.py):

  | file | before (donut main passes) | after |
  |---|---|---|
  | EnvironmentBrdf | donut main split-sum A/B (G != 0); with the 2018 HLSL but the intrinsic normalize: rows 0-2 low, [0,0] 3.09 vs 7.47 | 3966/4096 texels bit-identical, rows >= 2 identical; rows 0-1 max 0.23 ([0,0] 7.238 vs 7.473), reproduced by the original DXIL on this GPU |
  | LightProbeDiffuse | mean 0.0065 vs 0.0197 | RGB mean ratio 1.028, mean abs error 3.1 % |
  | LightProbeSpecular | mean 0.041 vs 0.128 | RGB mean ratio 1.026 on every mip; per-face mip-0 means within -8 %..+8 %; mean abs error 20 % (mip 0) to 3 % (mip 7) |

  Running the shipped 2018 DXIL in place of the reconstructed light_probe shaders gives bit-identical diffuse and
  specular output, so the remaining probe differences come from the captured scene (alpha coverage 0.50 vs 0.45:
  slightly more asteroid geometry in our capture, i.e. LOD / culling / animation state), not from the filtering.
- `-view 0 -screenshot ... 6000` vs build/reference/original_view0.png (lumcmp.py): full 0.733 -> 1.085, sky
  0.655 -> 0.990, fog band 0.732 -> 1.040, field 0.745 -> 1.068, cliffs 0.75-0.78 -> 1.17-1.20. The sky/fog now
  match; the cliffs were already ~1.17x relatively brighter than the sky before the change and show different
  lit faces (rotation / time of capture: the original was taken ~150 s after launch, ours at frame 6000), so the
  remainder is scene lighting/animation, not exposure or tone mapping.

## TemporalAAPass2018 (+880, resolve half)

| binary | reconstruction |
|---|---|
| `TemporalResolve` 0x140096BC0 | `TemporalAAPass2018::Resolve` |
| `AdvanceFrame` 0x140096290: `frameIndex = (frameIndex + 1) & 7`, swaps the two resolved textures (+88/+96) | `AdvanceFrame` |
| `GetCurrentPixelOffset` 0x140096310: 8-entry table (ymmword_14025E240 / xmmword_14025E230 / xmmword_14025E260) | `GetCurrentPixelOffset` |
| `ResolveOrAccumulate` caller 0x14002B2A0 | `FeatureDemo::ResolveOrAccumulate` |

- The jitter table equals donut's MSAA table except sample 5, which is (-0.4375, **+0.0625**) in 2018 and in the first
  public donut (donut main later changed it to -0.0625).
- With valid history (`m_PreviousViewsValid`): motion vectors, then the resolve into the output texture. Without
  history: **no resolve**; the HDR image is copied into the output texture (the 2018 caller blits it there).
- The output texture is what the post chain reads (`GetOutput()`); output and history swap every frame.
- Constants: previous view origin/size shrunk by 1 pixel per side, `clampingFactor = enableHistoryClamping ?
  clampingFactor : -1`, `newFrameWeight` straight from UIData+88 (0.05 / 1.0 / true).
- Deviation: the motion vector pass is still donut's (`TemporalAntiAliasingPass::RenderMotionVectors`, with the
  camera-relative translation difference); the 2018 version is `sub_140096380`. Static views only see the jitter
  difference in it, so the images match, but its moving-object behaviour has not been compared with the original.
- Result (AA on, 8 consecutive frames = all jitter phases, vs the original's default-mode capture, after the shadow
  basis fix below): view 0 41.0 dB (the original differs from itself by 41.4 dB between captures), view 2 42.7 (self
  40.8), view 3 35.6 (37.3), view 4 36.9 (36.0), view 5 32.6 (30.6).

Solved (was the open item "view 3, low sun"): shadow patches on the nearest rocks differed from the original
(AA off 30.7 dB vs its 46.6 dB self-PSNR; 38.1 dB with shadows disabled in both). Cause: the shadow texel grid was
**rolled** relative to the original. The 2018 donut in the binary builds the cascade light basis directly from the
direction, `lookatZ(direction, up = (0,0,1))` (up = (1,0,0) for a near-vertical light; 0x1400ADB00 and sub_1401EDF90).
donut main instead takes the light node's rotation, which our code set with `Light::SetDirection` - and that uses the
one-argument `lookatZ`, whose `orthogonal()` picks the (0,-z,y) perpendicular whenever |dir.x| <= |dir.z|: a completely
different roll about the light axis for this scene's sun. Same shadows, differently rasterized texels, hence patchy
PCF edges in every shadowed view (worst where the sun is low). Fixed in `FeatureDemo::RenderShadows` by setting the
node rotation to `inverse(lookatZ(dir, up2018))` directly. View 3 AA off 30.7 -> 38.2 dB (worst tile 3.87% -> 0.22%),
TAA 29.5 -> 35.6 dB (self 37.3); every other view gained 0.5-2.3 dB as well. Everything checked earlier was indeed
equal to the binary: cascade fit (`SetupForPlanarViewStable` 0x140081CA0), snapping, `FillShadowConstants`, UV range,
clip-to-UVZW matrix, raster state (bias 100000 / 2 + slope 4), D24S8 format, defaults, winding, animation phase.
The residual (38 vs 46.6 dB ceiling AA off) is distributed single-texel shadow-edge shimmer, no structure.
