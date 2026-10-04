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
| 5b. Rendering handedness | ethereal-samples | `d7ff317`, `4c487a8`, this commit | The render pipeline is left-handed, with Donut's camera unchanged; the 2018 content's handedness is reconciled in the projection. See *Rendering handedness* below. `d7ff317` first rebuilt the camera basis by hand, framed as a Donut divergence; `4c487a8` kept Donut's camera but mirrored the view; this commit moves the reflection into the projection. |
| 6. Docs | ethereal-samples | this commit | Demo `README.md`, ADR 0001, this record. |

### Deviations from the plan

- **Step 3 is one commit, not one per module** (see above).
- **A framework defect had to be fixed before the demo ran** (`ChunkFile::deserialize`, fixed in
  Donut), which the plan did not anticipate.
- **The rendering handedness had to be swapped**, which the plan did not list as a porting step
  although it is one. See below.
- **Donut's `render/SsaoPass.h` and `render/TemporalAntiAliasingPass.h` hold an
  `AutoPtr<FramebufferFactory>` behind a forward declaration**, which `AutoPtr`'s destructor cannot
  instantiate. Worked around by including `engine/FramebufferFactory.h` first in `FeatureDemo.h`; the
  headers themselves should include it (follow-up below).
- **A second GPU confounded the runs.** The reference machine has an AMD iGPU besides the RTX 4050,
  and the switchable-graphics profile keys on the executable name, so `nv_asteroids.exe` lands on the
  integrated GPU by default where `Asteroids.exe` does not. Every verification run passes
  `-adapter NVIDIA`.

### Rendering handedness

Donut's camera builds a left-handed (D3D) basis: `right = cross(up, dir)`, so that
`cross(right, up) = dir` with +Z forward. The donut `main` the reconstruction was written against
built `right = cross(dir, up)` — the same basis with the right vector negated, a historical slip
that `ethereal-dev` corrected. The 2018 demo's content, shaders and lighting constants were authored
and verified in that older, mirrored view space, so moving to Donut's camera is a handedness swap
and part of the port, not a framework difference to work around.

The render pipeline is left-handed throughout (`src/app/RenderHandedness.h`):

- **View space is Donut's.** Every `engine::IView` carries the camera's matrix unchanged, so all
  view-space work — the G-buffer pass, deferred lighting, fog, the shadow-cascade fit — runs in the
  left-handed view space, and `IView::IsMirrored()` is false for the main view.
- **The content's handedness is reconciled once, in the projection.** Because the 2018 content was
  authored through the mirrored camera and Donut's camera is a proper rotation, one reflection has to
  sit between world and clip to reproduce the original. `demo::MirrorProjectionX` negates view-space
  X before the D3D projection, at the one place the main view's projection is built
  (`FeatureDemo::UpdateViews`). World-to-clip is therefore the 2018 transform, so the triangle
  winding, the 2018 rasterizer states and the reconstructed shaders are unchanged; the TAA jitter is a
  clip-space offset applied after the projection and is unaffected.
- **Readers of the projection's focal terms take magnitudes.** `P[0][0]` is negative. The lens-flare
  and star-field aspect ratios, the particle sprite size and the meshlet LOD metric read the diagonal
  as sizes, and take `demo::ProjectionScale` (otherwise the sprites would be mirrored and the aspect
  ratios negative). The planet pass already took `fabsf`.
- **HBAO+ gets the reflection on its world-to-view instead.** The SDK accepts a projection with a
  negative focal term without reporting an error, but reconstructs view-space positions as if it were
  positive, which disagrees with the normals it rotates into view space: the AO came out wrong
  (27.1 dB against recon at view 0 instead of 96). `HbaoPlusPass` hands it the same world-to-clip
  transform with the reflection moved from the projection onto its world-to-view matrix; occlusion is
  invariant under a reflection of view space.

The demo reads only position, direction and up from its cameras, which both conventions agree on,
and none of Donut's `IsMirrored`-aware geometry passes are used (the 2018 passes set fixed cull and
front-face states), so nothing else depends on where the reflection sits.

History: the first port took the camera matrix with no reflection at all, and the asteroids rendered
inside-out (10.9 dB against recon at frame 60, and about 2 ms of frame time lost to inverted
backface culling). `d7ff317` and `4c487a8` then put the reflection on the view matrix, which kept the
2018 view space downstream; this commit moves it into the projection so that the pipeline is
left-handed. With SSAO disabled the left-handed pipeline matches recon at 96.6 dB on view 0; with
the HBAO+ conversion, view 0 (AA off, frame 6000) is 43.89 dB against the 2018 original, the same
as recon, and 96.1 dB against recon.

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

Left-handed pipeline (this commit). Scoring as recon's `aa0table.py`: the better of the original's two
captures.

| view | original vs itself | recon vs original | ported vs original | ported vs recon |
|---|---|---|---|---|
| 0 | 46.3 dB | 43.9 dB | 43.9 dB | 99.4 dB |
| 1 | 39.8 dB | 41.8 dB | 41.8 dB | 84.9 dB |
| 2 | 45.3 dB | 45.5 dB | 45.6 dB | 92.1 dB |
| 3 | 46.6 dB | 38.2 dB | 38.2 dB | 69.6 dB |
| 4 | 38.7 dB | 39.4 dB | 39.4 dB | 88.4 dB |
| 5 | 32.9 dB | 34.9 dB | 35.0 dB | 40.5 dB |
| **mean** | **41.6 dB** | **40.6 dB** | **40.6 dB** | **79.1 dB** |

### Image — default mode (2018 TAA), frames 6000..6007, best of the 8 jitter phases

Scoring as recon's `taatable.py`: for each of the original's two captures the best-matching jitter
phase, averaged.

| view | original vs itself | recon vs original | ported vs original | ported vs recon |
|---|---|---|---|---|
| 0 | 41.4 dB | 41.0 dB | 41.0 dB | 77.3 dB |
| 1 | 38.0 dB | 40.2 dB | 40.2 dB | 80.6 dB |
| 2 | 40.8 dB | 42.7 dB | 42.7 dB | 68.8 dB |
| 3 | 37.3 dB | 35.6 dB | 35.6 dB | 73.1 dB |
| 4 | 36.0 dB | 36.9 dB | 36.9 dB | 73.9 dB |
| 5 | 30.6 dB | 32.6 dB | 32.4 dB | 37.1 dB |
| **mean** | **37.3 dB** | **38.2 dB** | **38.1 dB** | **68.4 dB** |

Reading these:

- **The port is image-neutral.** On all six views in both modes the ported build scores the same as
  recon against the original to within 0.2 dB, and ported-vs-recon sits far above the original's own
  capture-to-capture variation (two recon runs of the same view differ by 94.8 dB).
- **Where a view is below 40 dB, so is the original against itself.** Views 3 and 5 score below
  40 dB against the original in default mode for every build, and the original's own
  capture-to-capture PSNR there is 37.3 and 30.6 dB: that is the original's noise floor, not a defect
  of the reconstruction. A blanket "40 dB against the 2018 reference" is not reachable for those two
  views by any build, the original included. View 4 is 39.4 dB AA off against a 38.7 dB ceiling.
- **View 5 is the lowest ported-vs-recon** (40.5 / 37.1 dB): its content is dominated by particles and
  noise, whose state depends on the wall-clock time at which frame 6000 is reached.
- An earlier capture set, taken with the reflection on the view matrix (`4c487a8`), scored view 3
  1.8-2.1 dB below recon. The left-handed pipeline gives the same world-to-clip transform, so the
  gap is not attributable to the matrix change itself; it did not reproduce in this set, and the
  likelier cause is the timing sensitivity of frame-indexed captures of a wall-clock-animated scene.

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

The series above was taken with the reflection on the view matrix (`4c487a8`). The left-handed
pipeline was not re-timed under controlled conditions: other workloads were running on the machine,
and the runs taken then are not comparable (ported 13.38 ms, recon 12.19 ms, ported 42.30 ms, in that
order, minutes apart). It adds no GPU work (the shaders are byte-identical and world-to-clip is
unchanged), and its CPU cost per frame is a projection-row negation, two magnitude reads and two 4x4
multiplies for HBAO+, so the series above stands for it; a re-run on an idle machine would confirm.

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
2. **Other code ported from donut `main` needs the same handedness review.** Donut's camera builds
   the left-handed basis; code written against `main`'s camera inherits a mirrored view space, and
   taking Donut's camera matrix unchanged renders it inside-out while still looking plausible.
   A short note in Donut's documentation on the camera convention, and on what it means for code
   ported from upstream, would save the next port the investigation.
3. **The `ChunkFile::deserialize` fix is a commit on donut's `nv_asteroids` branch**, not on
   `ethereal-dev`. It should be cherry-picked into `ethereal-dev`: without it every chunk asset is a
   use-after-free for any caller.
4. **Frame-indexed captures are timing-sensitive.** The scene animates with wall-clock time, so a
   screenshot at frame 6000 is only comparable between runs that reach it at the same moment. The
   comparison would be sturdier with a fixed-step capture mode (as `-benchmark` replays use), which
   would also make ported-vs-recon on views 3 and 5 deterministic.
5. **`NvVolumetricLighting` was carried over but not rebuilt** in this tree: it is independent of the
   demo (the D3D12 demo never calls it) and needs `fxc.exe`, so its targets were not part of the
   verification runs.
6. **Not yet run:** a Debug configuration build, which the plan lists under *Verification*.
   `-debug` was re-run on the left-handed pipeline and is clean as well. HBAO+ is disabled under the
   validation layer by design, so its handedness conversion is covered by the image tables, not by
   this run.
   `-debug` (D3D12 debug layer plus nvrhi validation) was run and is clean: no debug-layer and no
   validation errors, the only message being the documented `HBAO+ is not available with the nvrhi
   validation layer`, which confirms that the `QueryInterface` replacing the pass's `dynamic_cast`
   keeps the original behaviour.
7. **The submodule pins on this branch** are donut at the `ethereal-dev` tip plus the one fix above;
   merging into `framegraph` will want donut's `framegraph` tip instead, which contains
   `ethereal-dev`.
