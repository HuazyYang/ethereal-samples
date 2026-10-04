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

_To be completed._

## Verification results

_To be completed._

## Known issues and follow-ups

_To be completed._
