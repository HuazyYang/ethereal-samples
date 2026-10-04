# Asteroids (2018 NVIDIA meshlet demo): reconstruction

Source reconstruction of `Asteroids.exe` (D3D12) and `NvVolumetricLighting.d3d11.dll`, plus tools to unpack `media.db`.
The original binaries are expected in `../asteroids` (`ASTEROIDS_ORIGINAL_DIR`).

## Layout
| Path | Contents |
|---|---|
| `asteroids/src/` | Reconstructed demo C++: `app` (WinMain, FeatureDemo, UIData, render targets, cameras, replay), `ui`, `scene`, `meshlets`, `passes` (2018 geometry/lighting passes), `fx` (space effects), `audio`, `fs` (SQLiteFileSystem) |
| `asteroids/shaders/demo`, `asteroids/shaders/framework` | Reconstructed HLSL plus ShaderMake `.cfg` fragments and `*.NOTES.md` verification notes |
| `NvVolumetricLighting/` | Reconstructed D3D11 volumetric-lighting DLL (C++, HLSL with permutation tables, smoke test) |
| `external/donut` | Framework: NVIDIA donut (main), with nvrhi, ShaderMake and third-party code as submodules |
| `external/nvapi` | NVAPI SDK |
| `cmake/ThirdPartyDlls.cmake` | PhysX 3.4.2 / assimp / HBAO+ 4.0: upstream headers plus import libraries generated from the shipped DLLs |
| `tools/` | `extract_media.py` (decrypt and unpack media.db), `split_nvsp.py` (shader permutation blobs), `chk2gltf.py` (meshlet .chk → glTF/OBJ), `dll2def.py` |
| `docs/` | `formats.md` (media.db, NVSP, NVDACHNK, Stars.buf), `class_map.md` (2018 classes → donut main), `featuredemo_map.md` |

Each source module has a `NOTES.md` with the function map (binary address → reconstructed function), deviations and open questions.
Conventions are in `asteroids/CONVENTIONS.md`.

## Build
```
git submodule update --init --recursive
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target Asteroids NvVolumetricLighting.d3d11
```
`build/bin/` receives `Asteroids.exe`, the compiled shaders, the original third-party DLLs and `media.db`.
Asset extraction: `python tools/extract_media.py ../asteroids/media.db assets`, then `python tools/chk2gltf.py assets/media assets/chk_gltf`.

## Deliberate deviations (summary)
- Framework: donut main replaces the 2018 donut. The 2018-only pieces (SQLiteFileSystem, the geometry/lighting passes whose cbuffers
  differ, the chunk reader) are reconstructed demo-side.
- NVAPI 2018 mesh shaders are replaced by D3D12 SM 6.5 amplification/mesh shaders on nvrhi meshlet pipelines.
- Shaders are compiled from the reconstructed HLSL with current DXC. Non-mesh stages are kept at SM 6.0, so `min16float`
  behaves as in 2018.
- See the module `NOTES.md` files for every other deviation.

## Verification status
- media.db: all 299 entries extracted. All 170 `.chk` LODs load through the reconstructed C++ (`asteroids_asset_check`).
- Demo shaders: every permutation matches the original reflection (signatures, bindings, cbuffer layouts). Instruction streams are
  equivalent apart from DXC 1.2 → 1.8 lowering differences. See `asteroids/shaders/*/**.NOTES.md`.
- NvVolumetricLighting: 268 of 269 shader permutations are byte-identical. Exports and imports are identical. The smoke test renders
  bit-identically to the original DLL (WARP).
- Reference: the original runs on an RTX 4050 Laptop GPU (driver 610.47). A screenshot of its default view is kept in
  `build/reference/` (not versioned) for visual comparison.
- Reconstructed demo: builds and runs on the RTX 4050 Laptop GPU (D3D12 mesh shaders). It loads media.db in ~7 s and renders the full frame
  (meshlet asteroids with LOD/Hi-Z, cargo ship, planets, stars, fog, lens flare, particles, shadows, HBAO+, TAA, bloom, tone mapping).
  Mesh-shader modes: the original NVAPI path (default when supported) or D3D12 SM6.5 (`-meshShaders d3d12`).
  Against the original exe (same camera presets, DPI-correct PrintWindow capture) the image matches the original more
  closely than the original matches itself in four of six views; the other two are within 0.4-2.4 dB of that ceiling
  in default mode. See the multi-view tables below.
  `-renderLightProbes` produces probes within ~3% (RGB) of the shipped files, and an EnvironmentBrdf within 0.1%. With `-debug`
  (D3D12 debug layer + nvrhi validation), no errors are reported.
  Developer options added: `-screenshot <file.png> <frame>` (`-screenshotCount N` for N consecutive frames, i.e. all TAA jitter phases), `-log <file>`, `-perfLog`, `-gpuProfile` (per-pass GPU ms),
  `-shaderOverride <dir>` (load shipped DXIL converted with `tools/nvsp2shadermake.py` in place of the rebuilt shaders), `-dumpGBuffer <prefix>`, `-set field=value`, `-probeOutput <dir>`, `-showLightProbe N`, `-meshShaders nvapi|d3d12`.
  The benchmark result is also written to the log.

### Multi-view check (Release build, all six camera presets)
Setup: original captured with PrintWindow (client area, DPI-correct) at 90 s and 130 s; reconstruction captured with `-screenshot`
at frame 6000 (all 8 TAA jitter phases with `-screenshotCount 8`, best phase reported). 1920x1080, otherwise default settings.
The original is not bit-deterministic (TAA jitter phase, particle/noise state), so the **achievable ceiling is the original's own
PSNR between its two captures** and is listed next to ours.

AA off (`-aa 0`, static content, no TAA jitter):

| view | original vs itself | reconstruction vs original |
|---|---|---|
| 0 | 46.3 dB | **43.9 dB** |
| 1 | 39.8 dB | **41.8 dB** |
| 2 | 45.3 dB | **45.5 dB** |
| 3 | 46.6 dB | 38.2 dB |
| 4 | 38.7 dB | **39.4 dB** |
| 5 | 32.9 dB | **34.9 dB** |

Default (2018 TAA reconstructed):

| view | original vs itself | reconstruction vs original |
|---|---|---|
| 0 | 41.4 dB | **41.0 dB** |
| 1 | 38.0 dB | **40.2 dB** |
| 2 | 40.8 dB | **42.7 dB** |
| 3 | 37.3 dB | 35.6 dB |
| 4 | 36.0 dB | **36.9 dB** |
| 5 | 30.6 dB | **32.6 dB** |

- In views 1, 2, 4 and 5 the reconstruction matches the original more closely than the original matches itself between
  two captures; views 0 and 3 are within 0.4-2.4 dB of that ceiling in default mode. Where a view is below 40 dB, so is
  the original against itself (TAA jitter, particle and noise state).
- What got it there (before: 18-35 dB):
  1. The asteroid surface noise (`fbmd`) rotation matrices were compiled transposed by DXC 1.8 under
     `#pragma pack_matrix(row_major)`; the swapped-operand `mul()` form compiles to the shipped rotation and makes the
     G-buffer normals bit-identical to the shipped DXIL (`shaders/demo/surface_shaders.NOTES.md`).
  2. The 2018 TAA (`taa_cs` without PQ, 8-entry jitter table, output/history ping-pong) is reconstructed in
     `src/passes/TemporalAAPass2018.*`.
  3. The cascaded-shadow texel grid was rolled relative to the original: the 2018 donut builds the light basis as
     `lookatZ(direction, up=(0,0,1))`, while donut main's `Light::SetDirection` (one-argument `lookatZ`) picks a
     different roll for this scene's sun. Same shadows, differently rasterized texels - patchy PCF edges in every
     shadowed view (view 3, low sun, lost 8-16 dB). Fixed by setting the node rotation to the 2018 basis
     (`src/passes/NOTES.md`).
- Remaining: view 3 AA off carries distributed single-texel shadow-edge shimmer (38.2 vs the 46.6 dB ceiling, worst
  tile 0.22%, no structure); in default mode the view is within 1.7 dB of the original's own noise floor.

### Performance (Release, RTX 4050 Laptop GPU, 1920x1080 windowed, vsync off)
Measured with `-nodialog -benchmark` (replay.0.json, 8504 frames, unthrottled), using the same "Average frame time" counter in
both builds. The workload is identical (264,323 asteroids; ~4.03 M drawn triangles; the original shows 4,029,922, ours 4,033,792).

| build | runs (ms) | mean | FPS |
|---|---|---|---|
| original 2018 (NVAPI mesh shaders) | 11.94, 11.87, 11.87 | 11.89 ms | 84.1 |
| reconstruction, NVAPI mesh shaders (default) | 11.72, 11.72, 11.72, 11.70 | **11.72 ms** | 85.3 |

The reconstruction is 1.4% faster than the original on this machine. The first measurement of this project was 12.39 ms
(+4%); the gap was shader codegen, found with the per-pass profile below.

**Survey of the performance gap** (view 0, `-gpuProfile`, GPU timer queries, mean of 3 healthy runs; "original DXIL" = all shipped
shaders converted and loaded through `-shaderOverride` into the same pipeline, which isolates shader differences from everything else):

| pass | rebuilt, before fixes | original DXIL | rebuilt, now |
|---|---|---|---|
| GBufferFill | 3.26 | 3.09 | 3.12 |
| DeferredLighting | not measured | 1.45 | 1.20 |
| Fog | 1.76 | 1.27 | 1.27 |
| LensFlare | not measured | 0.35 | 0.28 |
| frame total | not measured | 10.59 | 10.33 |

(Other passes are within 0.01 ms: shadows 1.49, HBAO+ 0.81, environment map 0.84-0.88, TAA 0.31, particles 0.16, bloom 0.16.)
Causes found, all DXC 1.8 vs 2018 DXC 1.2 codegen, none algorithmic:
1. **Fog** (1.76 → 1.27 ms): `shadowCascades[cascade]` on a constant-buffer `int4` made DXC 1.8 re-copy the four ints into a local array on every loop
   iteration; hoisting the copy (`int4 cascadeIndices = light.shadowCascades`) fixed it. Found by loading only the original
   `fog_ps_trace` (1.28 ms) vs only `fog_ps_filter` (1.70 ms), then diffing the DXIL. The same pattern in the deferred lighting, forward,
   lens flare and particle cascade loops also makes those passes faster than with the original DXIL (DeferredLighting −0.25 ms, LensFlare −0.07 ms).
2. **GBufferFill** (3.26 → 3.12 ms): `colorRamp`'s `static const float3 palette[4]` became a writable global refilled with 12 stores per pixel;
   per-channel `static const float` arrays compile to constant data like the original. (Mesh/task shaders: no difference: 3.24 vs 3.25 ms.)
3. Not a cause: loop-invariant hoisting in the fog march (tried, no gain), light-struct copies, `-disableAsync`, resolution.

Measurement caveat: this laptop (30 W power cap, 6 GB VRAM) enters a throttled state after a few minutes of continuous load and
when the WDDM VRAM budget is exceeded (frame time jumps to 15-17 ms at 100% utilisation and low power). Runs above are from the healthy state
(waits of ≥25 s between runs; unhealthy runs were discarded and repeated). Per-pass GPU times are far more stable than whole-replay averages.
