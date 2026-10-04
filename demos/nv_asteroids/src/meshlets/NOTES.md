# Meshlet renderer (src/meshlets)

This is the C++ side of the 2018 meshlet geometry path. It has two mesh-shading modes (see "Mesh-shader modes"):
- **NVAPI** (the 2018 path): the vs_6_0 NVAPI task / mesh shaders in `asteroids/shaders/demo/nvapi/`, PSOs from
  `NvAPI_D3D12_CreateGraphicsPipelineState` with the 2018 PSO extensions, draws through NVAPI DispatchMeshTasks.
- **D3D12**: the SM6.5 shaders in `asteroids/shaders/demo/` (asteroidTS/MS, basicTS/MS) through nvrhi meshlet
  pipelines.

Both use create_hi_z_cs (see `meshlet_shaders.NOTES.md`) and the scene module (`scene/SpaceScene.h`, `SpaceSector.h`, `SpaceObject.h`, `AsteroidLibrary.h`). `ChunkMeshSet.h/.cpp` are
unchanged.

## Files and binary map

| file | 2018 class / function | addresses |
|---|---|---|
| `MeshletDrawStrategy.h/.cpp` | MeshletDrawStrategy (vtable 0x140259970, 0x178 bytes) | ctor 0x1400400E0, make_shared wrapper 0x14001BF90, vfunc00 Render 0x1400435A0, vfunc01 0x1400448D0, vfunc02 0x140041240, CreateShaders 0x1400412D0, asteroid draw 0x1400415A0, space-object draw 0x140042400, statistics 0x1400448B0 / 0x140041210 / 0x140041220 / 0x140041230 |
| `MeshletShaderSet.h/.cpp` | shader set + pipeline cache (0xD8 bytes, no RTTI) | ctor 0x1400449E0, pipeline lookup 0x1400459C0, pipeline creation 0x1400454A0, dtor 0x140041090, desc dtor 0x140040EF0, define push_back 0x1400459D0 |
| `MeshletRenderResources.h/.cpp` | MeshletRenderResources (0x68 bytes, FeatureDemo+232) | ctor 0x140040680 |
| `HiZPass.h/.cpp` | Hi-Z builder (0x28 bytes, strategy+16) | ctor 0x14003FB60, dtor 0x140040FF0; dispatch inlined in 0x1400435A0 |
| `PipelineStatisticsQuery.h/.cpp` | pipeline statistics query (0x30 bytes, FeatureDemo+328) | ctor 0x140040C10, Begin 0x140041170, End 0x1400411C0, Resolve 0x140044900, ReadResults 0x140041260, member dtors 0x140040DA0 / 0x140040E20 |
| `MeshletShaderTypes.h` | C++ include of `meshlet_cb.h` (namespace `meshlet_shader`) | - |
| `NvMeshShaderPsoExt.h` | `nvmesh2018::` constants (meshlet limits 64/100, group size 32, extension ids / versions) and the 2018 NVAPI PSO extension structs (task desc ext 10 ver 0x10030, mesh desc ext 11 ver 0x10040) | stack layouts in 0x1400454A0 / 0x140067DA0 |
| `NvMeshShaderApi.h/.cpp` | NVAPI wrappers: mesh-shader support query, PSO creation, DispatchMeshTasks | 0x140001980 (interface 0xA47716F8), 0x1400016A0 (NvAPI_D3D12_CreateGraphicsPipelineState), 0x140001AD0 (interface 0x8AB10C89), PSO assembly from 0x1400454A0 |
| `MeshShaderMode.h/.cpp` | mode switch (not in 2018) | - |

The address ranges given in the task resolve as follows:
- 0x140045A40 is PlayerShip's constructor and 0x140045B20 a quaternion multiply. Both belong to the scene module,
  not to the meshlet renderer.
- 0x1400459D0 is the `vector<ShaderMacro>::push_back` helper used by 0x1400449E0.

## Construction (FeatureDemo::CreateRenderPasses 0x14002BD60)

```cpp
auto resources = std::make_shared<MeshletRenderResources>(device);           // SceneLoaded, once (+232)
auto gbuffer = std::make_shared<MeshletDrawStrategy>(device, shaderFactory, scene, resources,
    depthTexture /*targets[0]*/, hiZTexture /*targets[1]*/, randomsTexture /*+1056*/, viewDistanceMap /*+1176*/,
    "gbuffer_ps.hlsl", MeshletRenderRole::GBuffer);                             // +248
// +264: ""                 , MeshletRenderRole::Depth       (shadow maps)
// +280: "material_id_ps.hlsl", MeshletRenderRole::MaterialId
// +296: "forward_ps.hlsl"  , MeshletRenderRole::Forward     then  forward->transparentPass = true  (2018 writes 1 to +8)
// +312: "forward_ps.hlsl"  , MeshletRenderRole::Forward     (ship overlay)
```

The 2018 constructor received the RenderTargets object and used its first two textures: [0] is depth and [1] is
the Hi-Z "far Z" texture (1/8 resolution, 5 mips, UAV). The reconstruction passes the two textures explicitly.

## Rendering API (used by the G-buffer, material-id, forward and shadow passes)

```cpp
MeshletPassState state;
state.framebuffer = fb; state.viewport = view.GetViewportState();
state.renderState = ...;                        // pass blend / depth / raster state
state.bindingLayouts = { materialLayout, ... }; // pass layouts in space0; the meshlet layouts (space1) are appended
state.bindings = { materialSet, ... };
strategy->Render(commandList, state, view, [&](SceneMaterial* m, MeshletPassState& s) {
    s.bindings[0] = m->bindingSet; return true; });
```

- `Render()` writes cbFrame. It then draws the space objects, under the marker "BasicObjects", when
  `drawPlayerShip || drawOtherObjects`. It draws the asteroid field, under the marker "Asteroids", when
  `drawAsteroids && !transparentPass`.
- The material callback runs once per asteroid draw with `AsteroidType::GetMaterial()`; its result is ignored.
  It also runs on every material change while a space object's mesh instances are drawn; returning false skips
  that instance.
- `CreateShaders()` is the 2018 "reset pipelines" call that FeatureDemo makes on a shader reload (0x1400412D0).
- Statistics:
  - The static `ResetFrameStatistics()` is 0x1400448B0, called once per frame before the passes.
  - `GetNumMeshletDraws()` 鈫?UIData+292, `GetNumAsteroidInstances()` 鈫?+296, `GetNumAsteroidPrimitives()` 鈫?+328.
  - From the G-buffer strategy: `numSectorsDrawn` 鈫?+360, `numAsteroidInstances` 鈫?+304, `numAsteroidPrimitives` 鈫?+336.
- `ReadStatsReadback()[0]` is the "numCulled" value FeatureDemo prints when `enableZCullStats` (UIData+125) is set.
- Pipeline statistics, as FeatureDemo used them:
  - `ReadResults()` is called at frame start.
  - `Begin`/`End` use slot 0 for shadows, 1 for the G-buffer and 2 for particles.
  - `Resolve()` is called at frame end.
  - UIData+344 = `results[1].CInvocations`, plus `results[2].CInvocations` when particles are enabled.

### Settings (public members), 2018 offsets, and what FeatureDemo writes

UpdateAsteroidRendererSettings 0x140037370 writes these into all five strategies unless noted otherwise.
`adj = (1 - UI+140) * 9`.

| member | offset | default | FeatureDemo source |
|---|---|---|---|
| `transparentPass` | +8 (int) | false | forward strategy (+296) set to 1 after creation |
| `wireframe` | +252 | false | gbuffer, materialId 鈫?UI+121; ship overlay (+312) 鈫?1 |
| `enableLod` | +253 | true | UI+122 |
| `enableDistanceLod` | +254 | true | UI+127 |
| `enableAsteroidsCulling` | +255 | true | UI+123 |
| `showBBoxes` | +256 | false | ship overlay (+312) 鈫?UI+126 |
| `enableZCullStats` | +257 | false | gbuffer 鈫?UI+125 |
| `alphaPreset` | +260 | 0 | UI+104 |
| `forcedLod` | +264 | 0 | UI+180 |
| `visualizeLods` | +268 | 0 | UI+188 (byte) |
| `zCullSectorThreshold` | +272 | 3 | UI+128 |
| `enableZCull` | +276 | false | gbuffer 鈫?UI+124 |
| `transitionRange` | +280 | 0.1 | UI+184; depth and materialId 鈫?0 |
| `lodBias` | +284 | 0 | UI+132 + adj; depth 鈫?UI+136 + adj |
| `lodSlope` | +288 | 1 | UI+140 |
| `lodView` | +328 | null | FeatureDemo+608 (main view; only its extent is used, for rtDims) |
| `drawAsteroids` | +336 | true | gbuffer, ship 鈫?UI+367; shadows: cascades 1, per-object 0 |
| `drawPlayerShip` | +337 | true | gbuffer, ship 鈫?UI+369; shadows: cascades 0, per-object 1 |
| `drawOtherObjects` | +338 | true | gbuffer, ship 鈫?UI+369; shadows: cascades 1, per-object 0 |
| `preViewTranslation` | +340 | 0 | FeatureDemo+640 (camera-relative rendering offset) |
| `preViewTranslationPrevious` | +352 | 0 | FeatureDemo+652 |
| `time` | +364 | 0 | FeatureDemo+100 |
| `minAsteroidScreenSize` | +368 | 0 | gbuffer 鈫?UI+216 |
| `numSectorsDrawn` / `numAsteroidInstances` / `numAsteroidPrimitives` | +304 / +312 / +320 | output | read by RenderScene |

The other offsets:
- +292..+299 are never written.
- +16 is the Hi-Z pass, +24 the scene, +40 the nvrhi D3D12 device and +48 the mesh-instance copy (see Deviations).
- +72 is the IDrawStrategy cursor; +88 and +112 are the "DebugUAVDest" readbacks.
- +136, +144 and +152 are the randoms, view-distance and Hi-Z textures.
- +160 is the ShaderFactory, +176/+184/+192 the shader sets, +200 the resources, +216 the PS file name and +248 the role.

## Algorithm notes (vfunc00 0x1400435A0)

cbFrame:
- Matrices come from `view`; `rtDims` comes from `lodView` (or `view`).
- `blueNoise` is the constant 4x4 dither matrix at 0x140256820 / 0x140256800 / 0x1402567F0.
- Hi-Z: `invZFarDims` = 1/size of the Hi-Z texture, `invZFarTileSize` = 1/8, `zFarNumLevels` = Hi-Z mip count.
- `enableViewDistanceFade` = role != Depth and `orthographicProjection` = `view.IsOrthographicProjection()`.
- `padding` (offset 520) and `padding2` stay 0.

Asteroids:
1. The 8 frustum corners, minus `preViewTranslation`, give the min/max sector indices. The range is grown by 1
   in every direction.
2. In the depth role, types whose largest instance radius is below `0.015 * |corner0 - corner1|` are skipped.
3. Each sector (from `GetSector`, which wraps) whose bounds are visible (`IView::IsBoxVisible`, using the sector
   origin plus `preViewTranslation`) is collected. The list is sorted by squared distance, in sector units, to the
   camera's sector.
4. G-buffer role with `enableZCull`: after `zCullSectorThreshold` sectors, `HiZPass::Dispatch` builds the Hi-Z
   pyramid from the depth drawn so far. All later draws use pipeline variant HiZ, i.e. the `_MESHLETS_HI_Z=1`
   asteroidTS.
5. Per sector and type with instances:
   - the minimum-screen-size test: `maxRadius / max(|origin|, 1) * max(P11*h, P00*w) >= minAsteroidScreenSize`;
   - statistics;
   - a per-sector binding set (t11 instance buffer);
   - cbSectorInfo = {origin + preViewTranslation, running draw index};
   - launch `instanceCount` task groups (NVAPI DispatchMeshTasks / `dispatchMesh`).

Space objects (0x140042400):
- cbObjectInfo: `bbox` = `GetMeshletBounds()` (SpaceObject+588), `center` = `shipGlowParams.yzw` (+564..+572),
  `lodBias` = `shipGlowParams.x` (+560), and 0 for `radius` and `maxLevelToRender`.
- Materials whose names start with `SF_Gargoship_` or `MatGlow_` use the IS_SHIP pixel shader permutation.
- Material `domain` 0 or 1 is drawn by the non-transparent strategies and 2 (Transparent) by the forward strategy,
  which uses pipeline variant AlphaBlend (premultiplied, no depth writes).
- Per instance: cbMeshletInfo = {numMeshlets, firstMeshlet}; cbInstance / cbInstancePrev =
  `transform * translation(preViewTranslation)` (previous: the previous transform and offset) as `float3x4`;
  then one task group (NVAPI DispatchMeshTasks(1) / `dispatchMesh(1)`).

PSO (0x1400454A0). The pass render state is copied, then (applied in both modes):
- fill mode = Wireframe variant bit;
- cull mode = Back;
- depth sets: depth bias 100000 for asteroids, or 2 with slope 4.0 for basic objects;
- other sets: depth writes off and RT0 blend One / InvSrcAlpha / Add for the AlphaBlend variant (only if no
  integer render target). The first 8 bytes of D3D12_BLEND_DESC are zeroed: alpha-to-coverage off and
  IndependentBlendEnable = FALSE, so RT0's blend applies to all targets. NVAPI mode writes exactly that; D3D12 mode
  copies RT0's blend into the other targets (nvrhi always sets IndependentBlendEnable).
- Fixed fields: VS = null, SampleMask ~0, PrimitiveTopologyType TRIANGLE, NumRenderTargets / RTV / DSV formats from
  the pass framebuffer, SampleDesc {1, 0} (the reconstruction passes the framebuffer's sample count, 1 in practice).

Binding layouts (all space1), created lazily from the first binding set, as in 2018:
- asteroid type: t0..t8, t10 buffers, t15 randoms, t16 view distance, t17 Hi-Z, b0, b4 (type), b5, u0, u1, s0, s1.
- sector: t11.
- space object: t0..t8, t15, t17, b0..b4, u0, u1.

## Deviations

- **Two mesh-shading modes.** 2018 had the NVAPI path only. The D3D12 SM6.5 path is an addition, selected
  when the NVAPI check fails or with `-meshShaders d3d12` (see "Mesh-shader modes").
- **Root signature per pipeline.** The 2018 shader set built one root signature (+40) from the layouts of the first
  pass that asked for a pipeline. NVAPI mode builds it per cached pipeline from that pipeline's layouts (same
  layouts in practice).
- **Pass state.** The 2018 passes gave the strategy an `nvrhi::GraphicsState` whose pipeline was their own graphics
  pipeline. Its desc (render state, binding layouts) and framebuffer info were copied into the mesh PSO. Passes now
  hand over `MeshletPassState` (render state + layouts + sets).
- **Material callback.** It takes the state as an explicit second parameter instead of capturing it.
- **Pipeline cache key.** The 2018 cache had one slot per variant. Here each variant slot holds a list keyed by the
  pass render state (blend / depth-stencil / raster, compared field by field), the binding layouts and the
  framebuffer formats. The forward pass changes `MeshletPassState::renderState` per material through the callback,
  and the ship forward strategy draws opaque and transparent materials. The 2018 overrides (fill mode from the
  wireframe bit, cull Back, depth bias / depth writes / RT0 blend, alpha-to-coverage off) are still applied on top
  of the pass state.
- **u0 / u1 bindings.** The 2018 binding sets bound null resources at u0 / u1 (space1), so the asteroidTS
  `u_Stats` Hi-Z counter was only fed if a pass bound it. DebugUAV and StatsUAV are bound there now.
- **nvrhi buffer flags.** Buffers carry the strides and view flags donut's nvrhi requires. Volatile constant
  buffers use `nvrhi::utils::CreateVolatileConstantBufferDesc` (2018: BufferDesc +21).
- **Pipeline statistics.** These go through native D3D12 objects (query heap, Begin/End/ResolveQueryData) because
  nvrhi has no pipeline-statistics queries. The readback buffer is set to CopyDest through nvrhi before the resolve.
- **Unused mesh-instance list.** The scene's non-meshlet mesh-instance list, which the 2018 constructor copied and
  sorted (+48, sort 0x14003F3B0), is never read and is not reconstructed.
- **Sector sort.** It uses `std::sort` with the 2018 comparator, so the order of equidistant sectors can differ.

## Mesh-shader modes

WinMain (`src/app/main.cpp`) selects the mode once after the device exists and before FeatureDemo is created:
- `IsNvMeshShaderSupported()` is the 2018 check (0x140001980: `NvAPI_Initialize`, then NVAPI interface 0xA47716F8
  through `nvapi_QueryInterface`, called with the ID3D12Device and a 1-byte out flag). In 2018, flag != 0 and
  status >= 0 printed "Meshlets are supported on your hardware" and started the demo; otherwise the
  "The GPU does not support mesh shaders..." box closed it.
- Default: NVAPI if that check passes, else D3D12 if `queryFeatureSupport(Meshlets)`, else the 2018 error box.
  `-meshShaders nvapi|d3d12` forces a mode (it fails with the same box if that mode is unsupported).
  The choice is logged ("Mesh shaders: ...").
- `SetMeshShaderMode()` stores it; MeshletRenderResources, MeshletShaderSet and fx::ParticleSystem read it at
  creation.

NVAPI mode, as in 2018:
- **Shaders.** `demo/nvapi/<name>.hlsl` (vs_6_0, created as vertex shaders), same permutations and entry points as
  the SM6.5 set (`ts_main` / `ms_main`). They write their outputs through the NVAPI extension opcodes on the fake
  UAV `g_NvidiaExt` (u7 space0; u0 for particles).
- **Root signature.** `nvrhi::d3d12::IDevice::buildRootSignature(layouts)`. The layouts are the pass layouts, the
  meshlet layouts and `MeshletRenderResources::nvExtensionBindingLayout`: one StructuredBuffer_UAV u7 / space0
  bound to a 256-byte dummy buffer. The 2018 meshlet layouts carried u7 as an extra item (0x00030007, null
  resource); nvrhi layouts have one register space, so it is a separate layout.
- **PSO.** `CreateNvMeshPipeline()` converts the render state into a D3D12_GRAPHICS_PIPELINE_STATE_DESC and calls
  `NvAPI_D3D12_CreateGraphicsPipelineState` with 3 extensions:
  1. `NVAPI_D3D12_PSO_SET_SHADER_EXTENSION_SLOT_DESC` {u7, space 0}.
  2. Mesh desc (ext 11, ver 0x10040): numThreads 32, unknown40 96, unknown48 1, topology 3, 64 vertices,
     100 primitives.
  3. Task desc (ext 10, ver 0x10030): numThreads 32, unknown40 96.

  The particle PSO (0x140067DA0) passes extensions 1 and 2 only: u0, unknown40 0, unknown48 0, 96 vertices,
  32 primitives. The native PSO is wrapped with `createHandleForNativeGraphicsPipeline` (2018: 0x14019C990).
- **Draw.** `setGraphicsState` (pipeline, framebuffer, viewport, sets + the extension set), then
  `nvrhi::d3d12::ICommandList::updateGraphicsVolatileBuffers()`, then DispatchMeshTasks(native command list,
  count) (0x140001AD0, NVAPI interface 0x8AB10C89, `NvAPI_Status(ID3D12GraphicsCommandList*, NvU32)`).
  The 2018 nvrhi set volatile CB addresses in setGraphicsState / writeBuffer. Current nvrhi patches them only in
  its own draw calls, so the update is called explicitly. The 2018 particle code called its nvrhi equivalent
  (0x1401A9320) at the same point; the 2018 meshlet code did not need to.

D3D12 mode: nvrhi meshlet pipelines (`createMeshletPipeline`), `setMeshletState` + `dispatchMesh` with the same
group counts.

## Scene API used (reconciled with the finished scene module)

The earlier assumed interface (`meshlets/AsteroidGpuData.h`) was dropped. The strategy now calls the real API:

| need | API |
|---|---|
| objects | `SpaceScene::GetObjects()` / `GetSpaceObject(i)`, `SpaceObject::isPlayerShip`, `GetMeshInstances()` (`SceneMeshInstance::firstMeshlet/numMeshlets/transform/previousTransform`, `mesh->material`), `GetMeshletBuffer(SpaceObjectBufferType)` t0..t8, `GetMeshletBounds()`, `shipGlowParams`, `GetMeshletBindingSet()` |
| asteroid types | `SpaceScene::GetAsteroidLibrary()`, `AsteroidLibrary::GetNumTypes()/GetAsteroidType()/GetNumPrims()`, `AsteroidType::GetBuffer(AsteroidBufferType)` (value = register), `GetObjectConstantsBuffer()`, `GetMaterial()`, `GetMeshletBindingSet()`, `GetName()` |
| sectors | `SpaceScene::GetSectorIndex()` (0x14005FD60), `GetSector()` (wrapped, 0x140060060), `GetSectorOrigin()` (0x14005C830), `SpaceSector::GetBounds()` (sector-local), `GetTotalInstanceCount()`, `GetInstanceCount(t)`, `GetInstanceBuffer(t)` (t11), `GetMaxRadius(t)`, `GetBindingSet(t)` |

## Open questions

1. **Stats UAV never cleared.** No clear of StatsUAV appears in the decompiled code, so the "numCulled" counter
   seems to accumulate across frames. The 2018 binding of u1 was also null, as noted under Deviations.
2. **Role 4.** It disables ship and object drawing in the constructor, but FeatureDemo never creates it.
   It is named `AsteroidsOnly` provisionally.
3. **Hi-Z mip count.** The 2018 Hi-Z binding creates one UAV per Hi-Z mip, but the shader always writes 5 levels.
   The RenderTargets reconstruction must create the Hi-Z texture with 5 mips (R32_FLOAT, 1/8 resolution, UAV).
4. **MS statistics counters.** D3D12_QUERY_DATA_PIPELINE_STATISTICS1 (MS/AS counters) is not used, as in 2018.
   Whether CInvocations counts mesh-shader primitives on every driver has not been verified.
