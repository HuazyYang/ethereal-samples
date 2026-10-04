# scene/ and audio/: reconstruction notes

This module covers the 2018 scene layer: the asteroid type library, SpaceObject (models), the player ship and
cargo ship, SpaceScene (fscene loader, sectors, PhysX collision), lights, materials and the XAudio2 sound player.
Each addresses refers to `Asteroids.exe`. Decompiled sources are in `recon/build/decomp/`.

## Classes

| File | 2018 class | Binary provenance |
|---|---|---|
| `AsteroidLibrary.h/.cpp` | AsteroidTypeLibrary, AsteroidType, AsteroidLod | library: 0x38 bytes at SpaceScene+120, load 0x1400101C0, name lookup 0x14000FF60, id lookup 0x14000FEF0 / 0x1400144F0, dirty constants 0x1400126A0. Type: 0x298 bytes, ctor 0x14000D9C0, dtor 0x14000E060, load 0x14000E420, materials 0x1400119C0 / 0x14000E9F0, collision mesh 0x14000E710, bounds / LOD table 0x14000ECD0, buffers 0x14000F120, GPU upload 0x140012770, GetBuffer 0x140010060. LOD: load 0x1400111C0 |
| `SpaceObject.h/.cpp` | SpaceObject (vtable 0x14025AAB0) | ctor 0x140050520, dtor 0x140050EB0, vfunc03 0x140054760, vfunc04 (Load) 0x140054DE0, assimp 0x140055030, chunk 0x140055730, assimp materials 0x140056480, JSON materials 0x140056FD0, material resolve 0x1400521E0 / 0x140051F20, node walk 0x140057C20, render resources 0x140052AD0, geometry 0x140052D80, Transforms 0x140052C00 / 0x1400585D0, meshlets 0x140053A00 / 0x140058930 / 0x1400532D0, materials 0x1400541B0, bounds 0x140051CC0, buffer getter 0x140054510 |
| `SceneGraph.h/.cpp` | scene-graph node, MeshInfo, MeshInstance, BufferGroup | node ctor 0x14004B6F0, Find 0x14004BD50, UpdateTransforms 0x14004C170, print 0x14004BF80 / 0x14004BC20, box x affine 0x140051A30 |
| `MeshletBuilder.h/.cpp` | runtime meshlet builder | 0x14006C5B0 (thunk 0x14006D800), clustering 0x14006CBA0, culling data 0x14006C000, Forsyth 0x14006EBB0 / tables 0x14006E320 |
| `SceneMaterial.h/.cpp` | Material (0xB0) | constants 0x1400741B0, binding set 0x14000F780 / 0x1400526C0, JSON 0x1400119C0 / 0x140056FD0, dirty upload 0x140058530 |
| `Lights.h/.cpp` | Light / DirectionalLight / SpotLight / PointLight | vtables 0x140257DA0 / DB8 / DD0 / DE8, `make_shared<PointLight>` 0x140047540 |
| `PlayerShip.h/.cpp` | flight model (no RTTI; 0xA8 bytes at SpaceScene+256) | ctor 0x140045A40, controls 0x140045CA0, sounds 0x140045EF0, shield 0x140046370, basis 0x1400463E0, SetPose 0x140046450, SetSpeed 0x140046550, Stop 0x140046560, Animate 0x140046590, Move 0x1400465F0, Update 0x140046820 |
| `CargoShip.h/.cpp` | ship visuals (no RTTI; 0x2A0 bytes at SpaceScene+264) | Init 0x140049580, glows 0x140048380 / 0x140049190, glow ctor 0x140047910, nav lights 0x140048D20, Update 0x140047DB0, rotating part 0x140049B50, accessors 0x1400490B0 / 0x14004A010 / 0x14004A020 / 0x14004A040 |
| `SpaceSector.h/.cpp` | SpaceSector (0x88), AsteroidPlacement (0xCC) | make_shared 0x1400611E0, AddInstance 0x140060FF0, transforms / bounds 0x1400616F0, Instances buffers 0x140060E80, upload 0x140061410, clear 0x140060DB0 |
| `SpaceScene.h/.cpp` | SpaceScene (vtable 0x14025B040) | ctor 0x14005A010, dtor 0x14005A700, Load 0x14005C890, LoadScene 0x14005D9D0, lights 0x14005E970, models 0x14005F710 (task 0x14005AC70), instances 0x14005D420, asteroids 0x14005C9D0, placement 0x14005D830 / 0x14005CFB0, collision 0x14005B6D0, vfunc03 0x14005B440, CreateResources 0x14005B3D0, Update 0x14005FDD0, Animate 0x14005FE50, UpdateBounds 0x14005FF00, sector helpers 0x14005FD60 / 0x140060060 / 0x14005C830 / 0x14005C740, mute 0x14005FC80 |
| `audio/SoundEngine.h/.cpp` | audio::Engine, Sound, WAV reader | Engine::Get 0x140015C00, error sink 0x140015B10, Sound::Create 0x140014EF0, destroy 0x140014DF0, WAV 0x140016170 / 0x1400160A0, IsPlaying 0x1400160F0, Rewind 0x1400163F0, SetMute 0x140016120 |

`RenderTargets` (0x14004A2C0 / 0x14004B270) and the ShowCubemap pass (0x14004C750) sit in the same address range,
but they belong to other modules and are not reconstructed here.

## Design choices (all marked `// deviation:` in the code)

**Audio: the 2018 player is reconstructed, not donut's AudioEngine.**
- donut's `donut::engine::audio::Engine` is a voice-pool and music-crossfade engine. It has an update thread and
  different voice lifetimes.
- The demo only needs three things: one looping music voice, a looping jet voice whose frequency ratio follows
  thrust, and a one-shot shield voice that restarts when it has finished.
- So `audio::Engine` / `audio::Sound` reproduce the 2018 code: XAudio2 2.9 (`xaudio2.lib`) with a mastering voice
  of category GameEffects and a canonical PCM WAV reader.

**Lights are demo-side, not donut lights.**
- The 2018 lights are free objects that store their own position. Their intensity uses the 2018 flux formulas:
  - point: `flux*100 / (8 (r*pi)^2)`
  - spot: `flux / (8 (r*pi)^2)`
  - directional: `irradiance / (1 - cos(angle/2))`
- They are owned by `SpaceScene::GetLights()` and the cargo ship updates their positions every frame.
- `SceneLight::FillLightConstants` fills donut's `LightConstants`, which has the same layout. The 2018 struct has
  no `shadowChannel` field, so it is set to -1. donut's `Light` would need SceneGraph nodes.

**Materials (`SceneMaterial`) are demo-side.**
- `SceneMaterial` mirrors the 2018 material and the 64-byte `c_Material` cbuffer (`MaterialConstants2018`, layout
  from the gbuffer_ps / forward_ps reflection).
- The material binding layout (pixel stage: b0, s0, t0..t3) is created by `CreateMaterialBindingLayout()` and
  owned by SpaceScene. In 2018 it lived at CommonRenderPasses+328.
- Missing textures fall back to `m_GrayTexture` (diffuse) and `m_BlackTexture` (the other slots). The sampler is
  `m_AnisotropicWrapSampler`.

**The scene graph is a demo-side copy of the 2018 donut Scene graph** (`SceneNode`, `SceneMesh`,
`SceneMeshInstance`). It uses `Scene*` names so it does not clash with `donut::engine::MeshInfo` / `MeshInstance`.

**Threading uses donut's ThreadPool instead of concurrency::task_group.**
- When a `donut::engine::ThreadPool*` is passed to `SpaceScene::Load`, models, asteroid types and texture decodes
  run on it.
- Without a pool, everything loads serially and textures use `LoadTextureFromFileDeferred`.

**Buffers need explicit view flags on donut's nvrhi.**
- The 2018 SRV buffers had stride 0 and no view flags.
- Here every meshlet and vertex SRV buffer sets `structStride`, `canHaveRawViews` and `canHaveTypedViews`, uses
  `initialState = ShaderResource` with `keepInitialState`, and has its size padded to 4 bytes.
- "Instances" uses stride 64.

**PhysX glue replaces the static extensions library.**
- The 2018 exe statically linked PhysX3Extensions. Only the core, cooking and character-kinematic DLLs exist, so
  `SpaceScene.cpp` provides a small allocator, an error callback (to `donut::log`), an inline CPU dispatcher and a
  default filter shader.
- `PxInitExtensions` / `PxCloseExtensions` are skipped (they only set up serialization and PVD).

**Fixes added on top of the 2018 code:**
- range and null checks where the 2018 code crashed or overflowed (marked in the code)
- the collision mesh is released
- materials are owned by the map, so `default_material` cannot be freed twice
- the meshlet-bounds bug that skipped vertex 0 is fixed

**Faithful quirks that are kept on purpose:**
- the fscene ship yaw/pitch are used as radians
- the top speeds are never applied
- the front glows are always 0 (inputs are cleared before Update)
- the collision sphere test uses `r^2 + 2*radius`
- assimp meshes get one meshlet set per instance
- only the first LOD that has more materials resolves chunk materials
- the last meshlet info's material is used per LOD

## Public API used by other modules

### FeatureDemo

Loading:
```cpp
auto scene = SpaceScene::Load(rootFs, "asteroids.fscene", textureCache.get(), threadPool /*optional*/);
// loading screen: SpaceScene::GetLoadProgress().completed / .total (atomics, readable from the UI thread)
textureCache->ProcessRenderingThreadCommands(*commonPasses, 0.f); textureCache->LoadingFinished();
scene->CreateRenderingResources(device, commonPasses, /*useMeshlets*/ true);   // 2018 SceneLoaded
```
- Sun: if no light in `scene->GetLights()` has `GetLightType() == LightType_Directional`, FeatureDemo adds its own
  `SceneDirectionalLight`. In 2018 that fallback wrote normalize(112,127,162) into the colour and set angularSize 4.5.
- Music: start `scene->GetAmbienceMusic()->Play()` unless muted (null-check it). 'O' calls Stop + Rewind and 'P'
  calls Play. The mute checkbox calls `SpaceScene::SetSoundMuted(bool)`.
- Colour LUT path: `GetColorLut()`, relative to the media directory. Other scene values: `GetSunColour()`,
  `GetAmbientColour()`, `GetWorldDiagonal()` (g_MaxSceneDistance), `GetBounds()`.

Per frame (2018 0x140038380):
```cpp
for (auto& o : scene->GetObjects()) o->UpdateBounds();
if (!scene->AreRenderingResourcesCreated()) scene->CreateResources(device);
scene->Update(cmd);                                        // sector instance uploads
scene->GetAsteroidLibrary().UpdateObjectConstants(cmd);    // dirty lodBias etc.
scene->UpdateBounds();
scene->Animate(cmd);                                       // dirty material constants (ship glows)
```

Ship control (2018 FeatureDemo::Animate):
```cpp
PlayerShip* ship = scene->GetPlayerShip();
ship->SetThrottle(...); ship->SetYawInput(...); ship->SetPitchInput(...); ship->SetRollInput(...);
float3 from = ship->GetPosition();
float3 desired = ship->Move(time, dt, freezeShipPosition);
float3 actual = scene->ResolveCollision(from, desired, 31.f);
ship->Update(time, dt, desired, actual);
// |desired - actual| > 0.1 -> ship->PlayShieldSound(); shield impact decay is FeatureDemo state
// without collision: ship->Animate(time, dt). Space / pad X: ship->Stop().
```
- Camera-path playback calls `ship->SetPose(pos, yawRad, pitchRad, 0)` and `ship->SetSpeed(v)`.
- ThirdPersonCamera uses `ship->GetCargoShip()->GetTransform()` and `ship->GetOrientation()`.
- Shadows use `scene->GetCargoShip()->GetSpaceObject()->GetBounds()`. Picking uses `scene->GetMaterials()`, where
  `materialID` = index + 1.

### Meshlet draw strategy

Asteroids:
- `scene->GetAsteroidLibrary()` gives the types. `AsteroidType::GetBuffer(AsteroidBufferType)` returns the
  buffers for t0..t8 and t10, where the enum value is the register. `GetObjectConstantsBuffer()` is cbObjectInfo
  (b4, space1, 384-byte `AsteroidObjectConstants`).
- Also on the type: `GetMaterial()` (material binding set at `->bindingSet`), `GetMeshletBindingSet()` (cache
  slot), `GetNumPrims()` (statistics), `GetLodInfos()`, `GetBounds()`, `GetCenter()`, `GetRadius()`,
  `GetLodBias()` and `GetMaxLevelToRender()`.

Sectors:
- `GetSectorIndex(pos)`, `GetSector(int2)` (wrapped), `GetSectorOrigin(int2)` (unwrapped), `GetNumSectors()`.
- Per sector and type: `GetInstanceBuffer(t)` is `StructuredBuffer<AsteroidInstance>` at t11, space1.
  `GetBindingSet(t)` is a cache slot. `GetInstances(t)`, `GetMaxRadius(t)` and `GetTotalInstanceCount()` are also
  available, and `GetBounds()` is in sector-local space.
- `SectorInfo {float3 sectorOffset; uint asteroidId}` (b5, space1) is filled by the strategy with
  `GetSectorOrigin(cell)` plus the camera offset.

Objects (the ship):
- Buffers: `SpaceObject::GetMeshletBuffer(SpaceObjectBufferType)` for t0..t10 (t9 = SubElements, t10 = LOD Info,
  which is always null).
- Per instance from `GetMeshInstances()`: `{firstMeshlet, numMeshlets}`, `transform` and `mesh->material`. The
  material name prefixes `SF_Gargoship_` / `MatGlow_` select pipelines, and the material `domain` selects the pass.
- Per object: `GetMeshletBounds()` (quantization box), `shipGlowParams` (float4 into the per-object constants) and
  the cache slot `GetMeshletBindingSet()`.

Shared:
- `AsteroidLODInfo`, `AsteroidObjectConstants`, `AsteroidInstance`, `SectorInfo` and `MaterialConstants2018` are
  `static_assert`ed to the shader reflection sizes.

### Non-meshlet passes (instanced forward / gbuffer)

- `SpaceObject::CreateRenderResources(..., meshlets = false)` builds index and vertex buffers per attribute bit
  (`GetBuffers()`), plus the "Transforms" instance vertex buffer. That buffer holds 96 bytes per instance: the
  current and previous float3x4, column-major.
- Each `SceneMesh` has `indexOffset` / `vertexOffset` / `numIndices`. The indices are mesh-local.

## JSON schemas confirmed

`asteroids.fscene` (object form; a top-level array is read as a models list):
- `ambienceMusic` (string), `ambienceMusicVolume` (float, default 1).
- `lights` (array, optional; absent in the shipped file). Each entry has `type`:
  - `dir_light` / `directional`: `name`, `direction`, `angularSize` (deg, default 1), then either `intensity`
    [rgb], which also sets irradiance 1, or `irradiance` + `color`.
  - `point_light` / `point` / `omni`: `name`, `position` (or `pos`), `radius` (0.2), then either `intensity` [rgb]
    or `flux` + `color`.
  - `spot_light` / `spot`: `name`, `position` (or `pos`), `direction`, `flux`, `color`, `innerAngle` (60),
    `outerAngle` (90).
- `models` and `player_ship` (arrays of models). Each model has:
  - `file`, a `.fbx` (assimp) or anything else (.chk)
  - `materials` (optional), a materials.json path
  - `instances`: either a 16-float row-major matrix, or `{translation, scaling, rotation(deg, yaw/pitch/roll)}`
- `player_ship[0]` adds:
  - `position`, `yaw`, `pitch` (used as radians)
  - `jetSound`, `jetSoundVolume`, `shieldSound`, `shieldSoundVolume`
  - `rotating_parts` [{`name`, `axis`}] (loaded, never animated)
- `controls`: `top_speed_forward` (>=1), `top_speed_reverse` (>=0), `acceleration_scale` (>=1),
  `rotation_sensitivity`, `roll_sensitivity` and `time_to_stop` (each >=0.01), `play_volume_min`,
  `play_volume_max`.
- `asteroids`: `library`, `world_min`, `world_max`, `number_of_sectors` [x, z], `placement_file`.
- Colours: `sun_colour` (default 1.3) and `ambient_colour` (default 0.1). `color_lut` (string).
- `version` and `camera_speed` are present in the file but not read.

`asteroids_chk-lod.json` is a top-level array of types:
- `name` (default "default-asteroid"; duplicates are skipped with a warning)
- `maxLevelToRender` (9), `lodBias` (0), `collisionLod` (5)
- `materialFile` (default "Clustered/materials.mtl", relative to the media dir)
- `lods`: [{`level`, `model`}]

`asteroid-placement.json` is an array of `{name, translation, scaling, rotation}`:
- `rotation` is in radians, ordered yaw/pitch/roll.
- The sector is `floor((t - world_min) / sectorSize)`, wrapped, over x and z. The sector size is
  `((max - min).x / nx, (max - min).y, (max - min).z / nz)`.
- The instance transform is `yawPitchRoll(r) * scaling(s) * translation(t - sectorOrigin)`.
- `uniformScale` = max(s).

materials.json maps a material name to an entry:
- `Diffuse`, `Specular`, `Emittance` [rgb] (default 0)
- `Shininess`, `Opacity`: no defaults, so a missing value reads as 0
- `Textures`: {`Diffuse` (sRGB), `Bumpmap` (linear), `Specular` (sRGB), `Emittance` (sRGB)}

The base path for textures depends on who loads the file:
- Asteroid types resolve texture paths against the media root (`materialFile/../..`).
- SpaceObjects resolve them against the model's directory.
- `default_material` is the assimp fallback only.
- Opacity < 1 makes the material Transparent (domain 2).

## PhysX usage

- **Setup (SpaceScene ctor):** `PxCreateFoundation`, `PxCreatePhysics` with the default tolerances, and
  `PxCreateCooking` with default `PxCookingParams`.
- **Asteroid collision meshes:** each type cooks one `PxTriangleMesh` from its `collisionLod` LOD. The triangles
  are expanded from the meshlet-local indices.
- **`ResolveCollision(from, to, radius)`:** called by FeatureDemo every frame with radius 31. Steps:
  1. Create a temporary `PxScene` and a material (0.5, 0.5, 0.1).
  2. For the 3x3 sectors around `to`, add a `PxRigidStatic` with a `PxTriangleMeshGeometry` (per-instance mesh
     scale) for each asteroid whose bounding sphere passes the literal 2018 test.
  3. Call `PxCreateControllerManager` and create a `PxCapsuleControllerDesc` controller. Its parameters:
     radius = `radius`, height 1e-6, step and contact offset 0.1, slope 0, ePREVENT_CLIMBING, eEASY, density 10,
     scaleCoeff 0.8, volumeGrowth 1.5.
  4. Call `move(to - from, 0, 1/60)` with the default filters.
  5. Read `getPosition()`, then release everything.

## Build integration

- Link libraries (`link_libraries.txt`): `asteroids_physx`, `assimp`, `xaudio2`. The headers need the PhysX 3.4
  and assimp include dirs, which come with those targets.
- **Bug in `cmake/ThirdPartyDlls.cmake`:** it copies `assimp/config.h.in` to `config.h` with COPYONLY, which leaves
  a `#cmakedefine` line that does not compile. `SpaceObject.cpp` works around this by pre-defining the
  `AI_CONFIG_H_INC` guard. The proper fix is a `configure_file` without COPYONLY, or deleting that line.
- `asteroids_physx` defines `PX_PHYSX_STATIC_LIB=0`. PhysX tests the macro with `#if defined`, so this drops
  `__declspec(dllimport)`. The functions still link through the import-library thunks.
- **Compile check:** all files compile in a private build (`build_scene`) with the PhysX and assimp include paths
  added through the `CL` environment variable.
- **Link check:** every PhysX, assimp and XAudio2 import they reference is present in the generated import
  libraries (`_asteroids_deps/implibs/*.def`).

## Open questions

- **AsteroidType+536.** It is set to 0x3D, and bit 0x40 would force `specularTextureType = 3`. Its meaning is
  unknown.
- **Writer of the library's dirty-type list.** None was found. `MarkObjectConstantsDirty()` is provided for the UI.
- **SpaceObject LOD table.** SpaceObject+440 is a 40-byte-per-entry "LOD Info" table, and nothing fills it. The
  +216 vector is now known to be the fscene instance transforms.
- **SpaceScene +272/+296 and +424.** These are pointer vectors that are never used, and a float set to 1.0.
- **Glow intensity formulas.** `PlayerShip::Update` reads the yaw/pitch inputs after `Move()` has cleared them, so
  the front glows never light up. This is kept as in 2018.
- **Placement re-parsing.** The placement parse aborts on the first unknown or empty name, as in 2018.
- **Shininess mapping.** Shininess maps to `roughness = 1 - shininess`. Assimp-imported materials use the raw assimp
  shininess (often > 1). The ship and monolith use materials.json, so this does not matter in practice.
- **Shader declarations of the vertex buffers.** The exact HLSL declarations (structured vs typed vs raw) are up to
  the shader agents. The buffers allow all three view kinds.
