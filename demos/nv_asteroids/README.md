# nv_asteroids — the 2018 NVIDIA *Asteroids* meshlet demo, reconstructed

Source reconstruction of `Asteroids.exe` (D3D12) and `NvVolumetricLighting.d3d11.dll`, with the tools
that unpack `media.db`, ported to this tree's Donut and nvrhi.

The original demo is not redistributable and is not part of this repository. Point
`NV_ASTEROIDS_ORIGINAL_DIR` at an installation of it: the build reads its `media.db` and generates
import libraries from the DLLs it shipped.

## Layout

| Path | Contents |
|---|---|
| `src/` | Reconstructed demo C++: `app` (WinMain, FeatureDemo, UIData, render targets, cameras, replay), `ui`, `scene`, `meshlets`, `passes` (2018 geometry/lighting passes), `fx` (space effects), `audio`, `fs` (SQLiteFileSystem) |
| `shaders/demo`, `shaders/framework` | Reconstructed HLSL, the two ShaderTool configs and the `*.NOTES.md` verification notes |
| `shaders/shaders_sm60.cfg`, `shaders/shaders_sm65.cfg` | Shader build, split by shader model (see *Shaders* below) |
| `nv_volumetric_lighting/` | Reconstructed D3D11 volumetric-lighting DLL (C++, HLSL with permutation tables, smoke test). Independent of the demo, which never calls it; it is kept here because it comes from the same title |
| `cmake/third_party_dlls.cmake` | PhysX 3.4.2 / assimp 4.1 / HBAO+ 4.0: upstream headers plus import libraries generated from the shipped DLLs |
| `tools/` | `extract_media.py` (decrypt and unpack media.db), `split_nvsp.py` (shader permutation blobs), `chk2gltf.py` (meshlet `.chk` → glTF/OBJ), `nvsp2shadermake.py`, `dll2def.py`, `asset_check.cpp` |
| `docs/` | `formats.md` (media.db, NVSP, NVDACHNK, Stars.buf), `class_map.md` (2018 classes → donut), `featuredemo_map.md` |

Each source module has a `NOTES.md` with the function map (binary address → reconstructed function),
deviations and open questions. The reconstruction conventions are in [`CONVENTIONS.md`](CONVENTIONS.md).

## Build

From the `ethereal` root:

```powershell
.\build.ps1 -CMakeArgs '-DETHEREAL_BUILD_NV_ASTEROIDS=ON','-DNV_ASTEROIDS_ORIGINAL_DIR=<path to the original>'
.\build.ps1 -Target nv_asteroids            # afterwards, the options are cached
```

`build\bin` receives `nv_asteroids.exe`, the compiled shaders under `shaders/nv_asteroids/dxil`, the
original's third-party DLLs and a copy of `media.db`. Targets: `nv_asteroids`,
`nv_asteroids_asset_check`, `NvVolumetricLighting.d3d11`, `NvVolumetricLighting.smoketest`.

The demo is opt-in (`ETHEREAL_BUILD_NV_ASTEROIDS`, default OFF) because it only configures with the
original installed.

Asset extraction, for the tools and for inspecting the data:

```powershell
python tools\extract_media.py <original>\media.db assets
python tools\chk2gltf.py assets\media assets\chk_gltf
```

### Running

```powershell
build\bin\nv_asteroids.exe
build\bin\nv_asteroids.exe -nodialog -view 0 -aa 0        # no startup dialog, camera preset 0, no AA
build\bin\nv_asteroids.exe -nodialog -benchmark           # replay.0.json, unthrottled
build\bin\nv_asteroids.exe -debug                         # D3D12 debug layer + nvrhi validation
```

On a laptop with a second GPU, pass `-adapter NVIDIA`: the switchable-graphics profile keys on the
executable name, and `nv_asteroids.exe` is not the name the original's driver profile knows, so the
demo can otherwise land on the integrated GPU (where the 2018 NVAPI mesh-shader path is
unavailable and the D3D12 SM 6.5 path is used instead).

Developer options added by the reconstruction: `-screenshot <file.png> <frame>`
(`-screenshotCount N` for N consecutive frames, i.e. all TAA jitter phases), `-log <file>`,
`-perfLog`, `-gpuProfile` (per-pass GPU ms), `-shaderOverride <dir>` (load shipped DXIL converted
with `tools/nvsp2shadermake.py` in place of the rebuilt shaders), `-dumpGBuffer <prefix>`,
`-set field=value`, `-probeOutput <dir>`, `-renderLightProbes`, `-showLightProbe N`,
`-meshShaders nvapi|d3d12`.

## Shaders

ShaderTool takes the shader model per invocation, so the shaders are split into two configs and two
targets:

| Config | Target | Shader model | Contents |
|---|---|---|---|
| `shaders/shaders_sm65.cfg` | `nv_asteroids_shaders` | 6.5 | the 7 meshlet amplification/mesh rows |
| `shaders/shaders_sm60.cfg` | `nv_asteroids_shaders_sm60` | 6.0 | the other 49 rows |

Keeping the second group at 6.0 is a correctness requirement: from SM 6.2 ShaderTool passes
`-enable-16bit-types`, which changes `min16float` from the "may be 32-bit" semantics the 2018
shaders were compiled under to real 16-bit, and with it the arithmetic of the surface, lighting and
fog shaders. Both targets write into one output directory, which is where the demo's `ShaderFactory`
base path points.

## Deliberate deviations

- **Framework**: this tree's Donut (`ethereal-dev`) replaces the 2018 donut. The 2018-only pieces
  (`SQLiteFileSystem`, the geometry/lighting passes whose cbuffers differ, the chunk reader) are
  reconstructed demo-side. The 2018 classes are mapped to Donut in `docs/class_map.md`.
- **Object model**: the demo was reconstructed against vanilla donut `main` and ported to the
  `ethereal-dev` object model (`nvrhi::AutoPtr`, interface tables, out-parameter factories,
  `nvrhi::IDataBlob`, `QueryInterface` instead of `dynamic_cast`, ShaderTool). See
  [`../../doc/adr/0001-nv-asteroids-on-the-ethereal-dev-object-model.md`](../../doc/adr/0001-nv-asteroids-on-the-ethereal-dev-object-model.md).
- **Mesh shaders**: the 2018 NVAPI path is reconstructed and is the default when the 2018 NVAPI
  check passes; `-meshShaders d3d12` uses D3D12 SM 6.5 amplification/mesh shaders on nvrhi meshlet
  pipelines instead.
- **Rendering handedness**: the render pipeline is left-handed. Donut's camera builds the
  left-handed basis (`right = cross(up, dir)`) and every `engine::IView` carries its matrix
  unchanged; the 2018 demo was authored through an older donut camera that built the mirrored one,
  so the content's handedness is reconciled once, in the projection (`src/app/RenderHandedness.h`).
  World-to-clip, winding and the 2018 rasterizer states are therefore unchanged. Code reading the
  projection's focal terms as sizes takes their magnitudes; HBAO+ gets the reflection on its
  world-to-view matrix instead.
- **Fonts**: `app::ImGuiRenderPass` has no font registry (donut main's `ImGui_Renderer` did), so
  `UIRenderer` registers the three demo fonts against the ImGui atlas itself.
- Shaders are compiled from the reconstructed HLSL with current DXC. Non-mesh stages stay at SM 6.0,
  so `min16float` behaves as in 2018.
- See each module's `NOTES.md` for every other deviation.

## Verification

- **media.db**: all 299 entries extract. All 170 `.chk` LODs load through the reconstructed C++
  (`nv_asteroids_asset_check <media.db>`), with 0 failures after the port.
- **Shaders**: every permutation matches the original's reflection (signatures, bindings, cbuffer
  layouts); instruction streams are equivalent apart from DXC 1.2 → 1.8 lowering differences
  (`shaders/*/**.NOTES.md`). All 56 compiled blobs are **byte-identical** to the pre-port build's,
  so the move from ShaderMake to ShaderTool changes neither image nor timing.
- **NvVolumetricLighting**: 268 of 269 shader permutations are byte-identical to the original DLL's.
  Exports and imports are identical. The smoke test renders bit-identically to the original DLL (WARP).
- **Reference GPU**: RTX 4050 Laptop GPU. The original 2018 executable is not bit-deterministic
  (TAA jitter phase, particle and noise state), so for every camera preset the achievable ceiling is
  the original's own PSNR between two captures of the same view, and it is reported next to ours.

The image and performance results of the port are in
the ethereal aggregate's
[`docs/plans/2026-10-04-nv-asteroids-migration.md`](../../../docs/plans/2026-10-04-nv-asteroids-migration.md).
