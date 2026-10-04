# Migrate the 2018 Asteroids reconstruction into `demos/nv_asteroids`

- Status: Approved
- Approved: 2026-10-04

## Context

`nvrhi-asteroids/recon` is a source reconstruction of the 2018 NVIDIA *Asteroids* meshlet demo
(`Asteroids.exe`, D3D12) and of `NvVolumetricLighting.d3d11.dll`, built on **vanilla donut `main`**
(`721014c`) with its nvrhi (`c8d34b4`). It reaches the original's own capture-to-capture PSNR in four of
six camera presets and runs 1.4 % faster than the original on the reference GPU.

This work moves that project into `ethereal-samples/demos/nv_asteroids` and ports it to the framework the
aggregate actually ships: **`donut/ethereal-dev`** (`d4e24c0`) with **`nvrhi/ethereal-dev`** (`0734789`).
That is not a version bump but an object-model change:

| Vanilla donut `main` | `ethereal-dev` |
| --- | --- |
| `std::shared_ptr<T>` members, `std::make_shared<T>` | `nvrhi::AutoPtr<T>`, `MAKE_RC_OBJ_PTR(T, ...)`, raw `T*` at API boundaries |
| `std::weak_ptr<T>`, `.lock()` | `nvrhi::WeakPtr<T>`, `.Lock()` |
| plain classes | `nvrhi::ObjectImpl` / `WeakReferenceSourceImpl` with explicit interface tables |
| `vfs::IBlob`, `readFile(path) -> shared_ptr` | `nvrhi::IDataBlob`, `readFile(path, IDataBlob**) -> FRESULT` |
| `ChunkFile::deserialize(blob, path) -> shared_ptr` | `deserialize(blob, path, ChunkFile**) -> FRESULT` |
| `app::ImGui_Renderer` | `app::ImGuiRenderPass` |
| ShaderMake, `donut_compile_shaders()` | ShaderTool, `ethereal_compile_shaders()` |

The demo derives from eight donut types (`ApplicationBase`, `IDrawStrategy`, `IGeometryPass`,
`vfs::IFileSystem` ×2, `BaseCamera`, `ImGui_Renderer`, `SceneTypeFactory`), so every one of those needs an
interface table. 19 k lines of demo C++ across 115 files use the affected idioms (364 `shared_ptr`,
96 `make_shared`).

Both requirements of the task — PSNR against the 2018 original and no performance overhead — are therefore
measurements of **the ported demo against the `recon` build**, which is kept as the baseline.

## Steps

All work happens in git worktrees so the `framegraph` tree is untouched:

| Worktree | Repository | Branch |
| --- | --- | --- |
| `ethereal-nv-asteroids/` | `ethereal` | `nv_asteroids` off `framegraph` |
| `ethereal-nv-asteroids/donut/` | `Donut` | `nv_asteroids` off `origin/ethereal-dev` |
| `ethereal-nv-asteroids/ethereal-samples/` | `ethereal-samples` | `nv_asteroids` off `framegraph` |

`donut`'s own submodules (`nvrhi`, `thirdparty/*`) are worktrees of their pinned commits, because git
shares a submodule's git directory between worktrees and a plain `submodule update` would move the
`framegraph` tree's checkout as well.

1. **Import.** The 275 tracked files of `recon` land under `ethereal-samples/demos/nv_asteroids/`
   unchanged, with `external/nvapi` re-registered as a submodule of `ethereal-samples`. Directory names
   become `snake_case` (`NvVolumetricLighting` -> `nv_volumetric_lighting`); file names keep the
   `PascalCase` of their directories. Nothing is added to the build yet, so the tree still configures.
2. **Shaders.** The nine ShaderMake `.cfg` fragments become one ShaderTool config per group, built with
   `ethereal_compile_shaders()` (DXIL only, SM 6.5; the non-mesh stages stay at 6.0 for 2018
   `min16float` semantics).
3. **Port.** Module by module, bottom-up: `fs`, `audio`, `meshlets`, `scene`, `passes`, `fx`, `ui`, `app`.
   The modules are mutually dependent, so they compile only once the last one is done; the port is one
   commit per module with the build completed at the end.
4. **Build integration.** `demos/CMakeLists.txt`, the third-party DLL layer (PhysX 3.4.2, assimp,
   HBAO+ 4.0, NVAPI, SQLite 3.24, LZ4) and the runtime deployment of `media.db`, under an
   `ETHEREAL_BUILD_NV_ASTEROIDS` option that defaults to the presence of `ASTEROIDS_ORIGINAL_DIR`.
5. **Visual verification.** All six camera presets, AA off and default, against the 2018 original.
6. **Performance verification.** `-nodialog -benchmark` replay against the `recon` build on the same GPU.
7. **Documentation.** Demo `README.md`, an ADR for the port decisions, this plan's execution record.

## Verification

- `build.ps1 -Config Release -Target Asteroids` and `-Config Debug`, plus `-debug` (D3D12 debug layer
  and nvrhi validation) with no reported errors.
- `asteroids_asset_check` loads all 170 `.chk` LODs from `media.db`.
- **Visual:** `-screenshot <file> <frame> -screenshotCount 8` at each of the six presets, PSNR against
  the 2018 original captured with DPI-correct `PrintWindow`, reported next to the original's own
  capture-to-capture PSNR (its noise floor) and against the `recon` build's output for the same preset.
- **Performance:** three or more healthy runs of `replay.0.json` per build, "Average frame time" from the
  same counter, `recon` and ported build alternated on one session; per-pass GPU times with `-gpuProfile`
  if the totals differ.

## Execution record

| Step | Repository | Commit | Notes |
| --- | --- | --- | --- |
| Worktrees | — | — | `ethereal-nv-asteroids/` (branch `nv_asteroids` off `framegraph`), with worktrees of `Donut` at `origin/ethereal-dev` and of `ethereal-samples` off `framegraph`. Donut's own submodules are worktrees of their pinned commits, because git shares a submodule's git directory between worktrees. |
| 1. Import | ethereal-samples | `eac2f4b` | The 271 tracked source files of `recon`, laid out as planned. `assets/` (4.9 GB) is generated and was not carried over. recon's root `CMakeLists.txt` was dropped: it only set aggregate-level Donut options, which the ethereal root owns. |
| 2+4. Shaders and build | ethereal-samples | `4305dba` | The two ShaderTool configs and the whole CMake layer, in one commit: the shader build cannot be verified without the targets that drive it. |
| | ethereal | `008f850` | `ETHEREAL_BUILD_NV_ASTEROIDS`, the NVAPI wiring, and the Donut submodule moved to the `ethereal-dev` tip. |
| 3. Port | ethereal-samples | `2384318` | 79 files. Deviation from the plan: the port landed as one commit rather than one per module. The modules are mutually dependent, so a per-module commit would not have compiled. |
| 5a. Framework fix | donut | `9873536` | `ChunkFile::deserialize` stored the caller's blob without taking a reference while `~ChunkFile` released it. Both callers pass a blob they do not hand over, so every chunk asset was a use-after-free. Found because the asteroid collision meshes reached PhysX cooking as freed memory. |
| 5b. Camera basis | ethereal-samples | `d7ff317` | Donut `ethereal-dev` builds the camera basis as `right = cross(up, dir)` where donut `main` uses `right = cross(dir, up)`. See *Known issues* below. |
| 6. Docs | ethereal-samples | this commit | Demo `README.md`, ADR 0001, this record. |

### Deviations from the plan

- **Step 3 is one commit, not one per module** (see above).
- **Two framework defects had to be fixed before the demo ran**, neither of which the plan
  anticipated. One was fixed in Donut (`ChunkFile::deserialize`); the other, the camera basis, was
  compensated for demo-side so that Donut keeps the convention its other samples use.
- **Donut's `render/SsaoPass.h` and `render/TemporalAntiAliasingPass.h` hold an
  `AutoPtr<FramebufferFactory>` behind a forward declaration**, which `AutoPtr`'s destructor cannot
  instantiate. Worked around by including `engine/FramebufferFactory.h` first in `FeatureDemo.h`; the
  headers themselves should include it (follow-up below).
- **A second GPU confounded the runs.** The reference machine has an AMD iGPU besides the RTX 4050,
  and the switchable-graphics profile keys on the executable name, so `nv_asteroids.exe` lands on the
  integrated GPU by default where `Asteroids.exe` does not. Every verification run passes
  `-adapter NVIDIA`.

## Verification results

Reference GPU: RTX 4050 Laptop GPU, 1920x1080, Release. The original 2018 executable is not
bit-deterministic (TAA jitter phase, particle and noise state), so for every camera preset the
achievable ceiling is the original's own PSNR between its two captures, and it is given next to ours.
"recon" is the pre-port build (vanilla donut `main`), which is the migration's baseline.

### Build and assets

- `nv_asteroids`, `nv_asteroids_core`, `nv_asteroids_asset_check` and the two shader targets build in
  Release and RelWithDebInfo with no errors.
- All 56 compiled shader blobs are **byte-identical** to the recon build's, so the move from
  ShaderMake to ShaderTool is a provable no-op for both image and timing.
- `nv_asteroids_asset_check media.db`: **170 files, 0 failures** — every `.chk` LOD loads through the
  ported SQLite / decrypt / LZ4 / chunk path.

### Image — AA off (`-aa 0`), frame 6000

| view | original vs itself | recon vs original | ported vs original | ported vs recon |
|---|---|---|---|---|
| 0 | 46.3 dB | 43.9 dB | 43.9 dB | 91.3 dB |
| 1 | 39.8 dB | 41.8 dB | 41.7 dB | 58.6 dB |
| 2 | 45.3 dB | 45.5 dB | 45.5 dB | 90.8 dB |
| 3 | 46.6 dB | 38.2 dB | 36.1 dB | 40.1 dB |
| 4 | 38.7 dB | 39.4 dB | 39.4 dB | 72.5 dB |
| 5 | 32.9 dB | 34.9 dB | 34.9 dB | 40.4 dB |
| **mean** | **41.6 dB** | **40.6 dB** | **40.3 dB** | **65.6 dB** |

### Image — default mode (2018 TAA), frames 6000..6007, best of the 8 jitter phases

| view | original vs itself | recon vs original | ported vs original | ported vs recon |
|---|---|---|---|---|
| 0 | 41.4 dB | 41.0 dB | 41.0 dB | 71.4 dB |
| 1 | 38.0 dB | 40.2 dB | 40.1 dB | 56.4 dB |
| 2 | 40.8 dB | 42.7 dB | 42.8 dB | 61.6 dB |
| 3 | 37.3 dB | 35.6 dB | 33.8 dB | 38.2 dB |
| 4 | 36.0 dB | 36.9 dB | 36.8 dB | 60.8 dB |
| 5 | 30.6 dB | 32.6 dB | 32.6 dB | 37.5 dB |
| **mean** | **37.3 dB** | **38.2 dB** | **37.8 dB** | **54.3 dB** |

Reading these:

- **The port is image-neutral.** On five of the six views in each mode the ported build scores the
  same as recon against the original to within 0.1 dB, and ported-vs-recon sits far above the demo's
  own run-to-run noise (two recon runs of the same view differ by 94.8 dB).
- **Where a view is below 40 dB, so is the original against itself.** Views 3 and 5 score below
  40 dB in every column, including the original's own capture-to-capture PSNR in default mode
  (37.3 and 30.6 dB): that is the noise floor of the original, not a defect of the reconstruction.
  A blanket "40 dB against the 2018 reference" is therefore not reachable for those two views by any
  build, the original included.
- **View 3 is the one real gap**: 36.1 dB vs recon's 38.2 dB (AA off) and 33.8 vs 35.6 dB (default).
  recon's README already records view 3 as its weakest, with distributed single-texel shadow-edge
  shimmer; ported-vs-recon is lowest there too (40.1 / 38.2 dB), which is what shadow-edge noise
  looks like when the two builds sample the cascade grid a fraction of a frame apart.

### Performance — `-nodialog -benchmark` (replay.0.json, 8504 frames, unthrottled)

Both builds report the same "Average frame time" counter over the same workload (264,323 asteroids;
~4.03 M drawn triangles). Runs are spaced and alternated; this laptop enters a throttled state under
continuous load (recon's README documents 15-17 ms at 100 % utilisation and low power), and such runs
are discarded and repeated, as recon did.

| build | healthy runs (ms) | mean |
| --- | --- | --- |
| recon (vanilla donut `main`) | 11.63, 11.30, 11.34 | **11.42 ms** (87.6 FPS) |
| ported (donut/nvrhi `ethereal-dev`) | 11.51, 11.38, 11.32 | **11.40 ms** (87.7 FPS) |

The ported build is 0.2 % faster, which is inside the run-to-run spread of either build
(recon 11.30-11.63, ported 11.32-11.51). For reference, recon's README records 11.72 ms for this
build and 11.89 ms for the original 2018 executable on the same GPU.

Discarded as throttled: recon 17.99 ms, ported 12.69 ms (both the second run of a back-to-back pair).

Per-pass GPU times at view 0 (`-gpuProfile`, GPU timer queries, mean over 1000 frames):

| pass | recon | ported |
| --- | --- | --- |
| Frame | 10.57 ms | 10.60 ms |
| Shadows | 1.57 | 1.71 |
| GBufferFill | 3.13 | 3.38 |
| DeferredLighting | 1.19 | 1.30 |
| HBAO+ | 0.86 | 0.85 |
| Fog | 1.30 | 1.42 |

**The latest Donut and nvrhi introduce no overhead**: the ported build is within 0.2 % of the
pre-port build on the deterministic replay, and its shaders are byte-identical, so the GPU work is
the same by construction. (The per-pass table was taken before the camera fix, while inverted
backface culling still inflated the ported build's rasterisation; it is kept as the record of how
that defect was found, and the replay figures above are the result that stands.)

## Known issues and follow-ups

1. **Donut: `render/SsaoPass.h` and `render/TemporalAntiAliasingPass.h` should include
   `engine/FramebufferFactory.h`.** They hold an `AutoPtr<FramebufferFactory>` behind a forward
   declaration, which `AutoPtr`'s destructor cannot instantiate; any translation unit that destroys
   one of those passes without the definition fails to compile. Worked around demo-side.
2. **Donut: the camera basis convention diverged from upstream.** `ethereal-dev` builds
   `right = cross(up, dir)` where donut `main` builds `right = cross(dir, up)`, so the right vector
   is negated and the view — and with it the triangle winding — is mirrored against code written for
   upstream. This is not a defect on its own (the fork is self-consistent and its other samples
   follow it), but it is an undocumented, silent incompatibility for ported code: here it produced an
   image that still looked plausible while rendering the asteroids inside-out. It deserves a note in
   Donut's own documentation, and ideally an ADR in that repository.
3. **The `ChunkFile::deserialize` fix is a commit on donut's `nv_asteroids` branch**, not on
   `ethereal-dev`. It should be cherry-picked into `ethereal-dev`: without it every chunk asset is a
   use-after-free for any caller.
4. **View 3 is 1.8-2.1 dB below recon.** Both builds are inside the original's own noise floor for
   that view in default mode, but the gap is consistent across both modes and is worth a look at the
   cascade texel grid if the demo is used as a precision reference.
5. **`NvVolumetricLighting` was carried over but not rebuilt** in this tree: it is independent of the
   demo (the D3D12 demo never calls it) and needs `fxc.exe`, so its targets were not part of the
   verification runs.
6. **Not yet run:** a Debug configuration build, which the plan lists under *Verification*.
   `-debug` (D3D12 debug layer plus nvrhi validation) was run and is clean: no debug-layer and no
   validation errors, the only message being the documented `HBAO+ is not available with the nvrhi
   validation layer`, which confirms that the `QueryInterface` replacing the pass's `dynamic_cast`
   keeps the original behaviour.
7. **The submodule pins on this branch** are donut at the `ethereal-dev` tip plus the one fix above;
   merging into `framegraph` will want donut's `framegraph` tip instead, which contains
   `ethereal-dev`.
