# nvrhi / Diligent / native Asteroids benchmark — plan

Goal: measure the CPU-side cost of nvrhi (via Donut) against DiligentCore and hand-written
D3D11/D3D12 on the Asteroids workload (50,000 asteroids, 1000 meshes, 10 textures, CPU-bound),
following http://diligentgraphics.com/diligent-engine/samples/asteroids/, and produce a
performance analysis report.

## Decisions (fixed)

1. **Reference patching**: the reference sample is patched in place in
   `DiligentEngine/DiligentSamples/Samples/Asteroids` on a local branch `nvrhi-benchmark` of the
   DiligentSamples repo. The diff is exported to `patches/diligent-samples-asteroids.patch` in the root repo.
2. **Vulkan is in scope**: Diligent Vulkan and nvrhi Vulkan are measured and compared with their
   D3D12 counterparts. There is no native Vulkan renderer; native D3D12 stays the D3D12 baseline.
3. **Version control**: the root is a git repo. `Donut` and `DiligentEngine` are registered as
   submodules at their current commits. Nothing is ever pushed.

## Layout

| Path | Content |
|---|---|
| `src/common/benchmark.h` | Shared benchmark config, timers, CSV/JSON writers, scene hash (single source of truth) |
| `src/nvrhi/` | `asteroids_nvrhi` — Donut + nvrhi renderer |
| `DiligentEngine/DiligentSamples/Samples/Asteroids` | `asteroids_reference` — native D3D11/D3D12 + Diligent D3D11/D3D12/Vulkan |
| `scripts/` | `build.ps1`, `run_matrix.ps1`, `analyze.py`, `profile.ps1` |
| `tests/analysis/` | Python unit tests for the analysis code |
| `results/` | Raw run output (git-ignored) |
| `report/` | `report.md`, charts, aggregated CSV (committed) |

## Benchmark contract (both executables)

Command line:

```
-benchmark                 enable benchmark mode (no GUI sprites, fixed timestep, auto-exit)
-renderer native|diligent|nvrhi
-api d3d11|d3d12|vk
-binding dyn|mut|tex_mut|tex_mut_pc|bindless
-threads N                 1 = single-threaded rendering
-warmup SECONDS  -duration SECONDS
-window W H                default 1080 720
-adapter_luid N            fail if the created device is on another adapter
-gpu_timing                optional GPU timestamp queries
-output DIR                writes DIR/frames.csv and DIR/run.json
```

Rules:

- Fixed simulation timestep 1/60 s, seed 1337. `static_scene_hash` must be identical for every renderer.
- Vsync off, windowed, debug/validation layers off, Release build.
- A combination a renderer cannot express exits non-zero and writes `run.json` with
  `"status":"failed"` and a reason. It must never silently fall back to another mode.
- Exit code 0 only after both files were written.

Per-frame columns (`frames.csv`), all CPU wall-clock milliseconds on the main thread:

| Column | Definition |
|---|---|
| `update_ms` | simulation update |
| `render_ms` | start of command recording until every command list is recorded (workers joined) |
| `submit_ms` | closing and executing/submitting the command lists |
| `wait_ms` | blocking on GPU fences / frame pacing |
| `present_ms` | the Present call |
| `frame_ms` | start of frame N to start of frame N+1 |
| `gpu_ms` | optional GPU time of the frame (`-gpu_timing`) |
| `gc_ms` | the part of `wait_ms` that is nvrhi's per-frame garbage collection (Donut's `runGarbageCollection`); 0 for the other renderers |

Primary metric: `render_ms + submit_ms` (CPU render cost). Secondary: `frame_ms`.
Sensitivity metrics, reported next to the primary one for every configuration:
`render + submit + present` (Diligent and native D3D11 replay their deferred work inside Present) and
`render + submit + gc` (nvrhi releases what the finished command lists referenced in its garbage collection).

Details fixed by `src/common/benchmark.h` (setup phase):

- `frames.csv` header: `frame,update_ms,render_ms,submit_ms,wait_ms,present_ms,frame_ms,gpu_ms,draw_count,index_count,gc_ms`.
  Only measured frames are written; `frame` is the absolute index including warmup frames; `gpu_ms` is empty when unavailable.
- Exit codes: 0 ok; 2 invalid arguments or runtime error (`"failure_kind":"error"`); 3 combination not
  expressible (`"failure_kind":"unsupported"`, reported as N/A).
- `-adapter_luid N` is the decimal `(HighPart << 32) | LowPart`; see `docs/ENVIRONMENT.md`.
- A run fails if the renderer did not use exactly `-threads N` threads or did not report its adapter when one was requested.
- Extension for parity checks only: `-capture_frame N` writes `DIR/capture.bmp` and
  `capture_dynamic_scene_hash` for absolute frame N.
- Build, Vulkan and shader compilation notes: `docs/BUILD.md`. Known differences that remain: `docs/LIMITATIONS.md`.
- `-gpu_timing` runs are only for the CPU-bound check: the queries sit inside the timed columns, so
  `scripts/analyze.py` excludes such runs from every other metric and `scripts/run_matrix.ps1` keeps them in
  their own session. The query readback is outside the render/submit columns in every renderer.
- In benchmark mode both executables keep their window topmost (`Benchmark::KeepWindowOnTop`) and record
  `display_refresh_hz`, `window_foreground`, `window_visible_fraction`, `window_topmost`, `present_mode` and
  `swap_chain_tearing` in `run.json` `extra` (`Benchmark::AddDesktopFacts`): whether the compositor paces a
  swap chain depends on whether the window is displayed (`docs/LIMITATIONS.md` 3.1).
- Renderer-specific flags outside the contract (recorded in `run.json` `extra`):
  `asteroids_nvrhi -always_set_state | -track_liveness | -no_auto_barriers` (sensitivity switches, see below);
  `asteroids_nvrhi -cb_page_split_avoid | -cb_page_split_force` (D3D12 only: placement of the nvrhi command
  list objects, `docs/LIMITATIONS.md` 5.1; without them the run records the placement it got in
  `extra.cb_page_split`);
  `asteroids_reference -native_geometry_heap upload|default` and `-native_present tearing|default`
  (native renderers; benchmark mode defaults: `default` heap and `tearing`).

## Binding modes

| Mode | Diligent | nvrhi |
|---|---|---|
| `dyn` | one SRB, dynamic texture variable | not expressible (binding sets are immutable) — reported as N/A |
| `mut` | 50,000 SRBs | 50,000 binding sets |
| `tex_mut` | 10 SRBs + dynamic constant buffer | 10 binding sets + volatile constant buffer (headline comparison) |
| `tex_mut_pc` | N/A | 10 binding sets + push constants (nvrhi best case) |
| `bindless` | bindless mode (D3D12/Vk) | descriptor table + per-instance data (D3D12/Vk) |

Native D3D11/D3D12 have one fixed strategy; `-binding` is ignored and recorded as `original`.
Native D3D12 binds all 10 textures once per command list and selects one in the shader, so only `bindless`
uses an equivalent binding strategy: `bindless` versus native D3D12 is the **layer overhead with equivalent
binding strategy**; `dyn`/`mut`/`tex_mut`/`tex_mut_pc` versus native D3D12 is the **binding-model cost** and
must not be called abstraction overhead. Native D3D11 sets one texture per draw, like the D3D11 layers.

Threading is the same in every multi-threaded renderer: `-threads N` = the main thread plus N-1 persistent
worker threads, subset size `ceil(50,000 / N)`, worker wake-up and join inside `render_ms`.
Native D3D11 and nvrhi D3D11 are single-threaded (`-threads 1` only).

nvrhi sensitivity switches (defaults are representative competent usage: redundant binding-set filter on,
`trackLiveness` off, automatic barriers on):

| Flag | Effect |
|---|---|
| `-always_set_state` | `setGraphicsState` before every draw (what Diligent's per-draw `CommitShaderResources` does) |
| `-track_liveness` | binding sets created with nvrhi's default `trackLiveness = true` |
| `-no_auto_barriers` | `setEnableAutomaticBarriers(false)` after the first `setGraphicsState` of each command list, re-enabled after the draw loop |
| `-cb_page_split_avoid` / `-cb_page_split_force` | D3D12 only, not an nvrhi usage choice: command list objects placed so that no / a page boundary lies inside nvrhi's volatile constant buffer state (`docs/LIMITATIONS.md` 5.1) |

## Phases

1. **Setup** — git init, submodules, shared header, Vulkan enabled in CMake, baseline build.
2. **Implement (parallel)** — (a) instrument the reference, (b) write `asteroids_nvrhi`, (c) runner + analysis tooling.
3. **Integrate** — one build tree, smoke-run every configuration, scene-hash and image parity.
4. **Review** — independent fairness and nvrhi-usage reviews, then fixes.
5. **Measure** — full matrix on the RTX 4050, nothing else running on the machine.
6. **Profile** — CPU sampling of the headline configurations for hot-spot attribution.
7. **Report** — aggregate, chart, write `report/report.md`, then verify every claim against the data.

## Measurement protocol

- Matrix: {native, diligent, nvrhi} x {d3d11, d3d12, vk} x applicable bindings x threads {1, 2, 4, 8, 15};
  104 configurations. N/A combinations are not scheduled (native Vulkan, Diligent `tex_mut_pc`, nvrhi `dyn`,
  `bindless` on D3D11, more than one thread for native D3D11 and nvrhi D3D11).
- One protocol for every run that enters a ratio: **5 s warmup + 20 s measured**, for all configurations.
- Headline set: D3D12 and Vulkan, bindings `tex_mut` and `bindless` (plus native D3D12), threads 1 and 8,
  all renderers (18 configurations): **5 repetitions**. All other configurations: **3 repetitions**. 348 runs.
- Sensitivity set (`run_matrix.ps1 -Set sensitivity`, its own session): nvrhi D3D12 and Vulkan, `tex_mut`,
  threads 1 and 8, without a switch (baseline of the same session) and with each switch singly, plus on D3D12
  `cb_page_split_avoid` and `cb_page_split_force` (the two levels of the page-split store); 3 repetitions;
  configuration directories `<config>__<switch>`, reported in `sensitivity.csv`, never in the main matrix.
- Desktop state: the runner keeps the display and the system awake for the session
  (`SetThreadExecutionState`, restored at exit), refuses to start and stops before the next run when the
  display is off or the session is locked (`-AllowHiddenDesktop` overrides), and records the desktop state
  of every run in `manifest.json`. The session must be unlocked with the display on; nobody should use the
  machine meanwhile. `-CoverWindow` (its own session) covers every benchmark window so that no renderer is
  paced; it is the only way to an unpaced `frame_ms` of Diligent Vulkan (`docs/LIMITATIONS.md` 3.1).
- Fresh process per run, randomized interleaved order, cool-down between runs, resumable runner. A resume is
  refused when an executable changed since the session started (`-AllowExeChange` overrides) or when it would
  mix GPU-timed and untimed runs, quick and full runs, or resolutions.
- Adapter pinned by LUID to the RTX 4050; AC power; power plan, driver, OS, and commit hashes recorded
  (the power plan is recorded, not enforced).
- CPU-bound check, in separate sessions: `gpu_ms` well below `frame_ms` (`-GpuTiming` session) and `frame_ms`
  unchanged at half resolution (`-Width 540 -Height 360` session); `analyze.py --gpu-session --half-res-session`
  writes `cpu_bound_check.csv`.
- Statistics: per run the median and p1/p99 of each column; per configuration the median of run medians and
  the min-max spread across runs. A configuration is `ok` only when a majority of its scheduled repetitions
  are valid, otherwise `partial`. Diligent runs that reported engine errors are invalid.
- Paced runs (`analyze.py`: median `frame_ms` on a multiple of the refresh period with the slack absorbed by
  wait + present) are excluded from `frame_ms`, `present_ms`, `wait_ms`, `render + submit + present` and the
  CPU-bound check, and stay in `render + submit`. On a visible desktop that is every Diligent Vulkan run.
- nvrhi D3D12 runs are split by `extra.cb_page_split`: a configuration is reported from its unaffected runs
  when it has any, the affected runs separately (`cb_page_split.csv`); the two are never pooled.
- Comparisons (`scripts/analyze.py`): nvrhi / Diligent for the same API, binding and threads; overhead versus
  the native renderer of the same API only (native D3D12 for D3D12 rows, native D3D11 for D3D11 rows; for
  Vulkan rows native D3D12 appears only as an explicitly labelled cross-API reference); ns per draw from the
  recorded `draw_count` (50,001).

## Report content

Setup and methodology; CPU render cost per configuration; overhead versus native in percent and
ns per draw; thread scaling; binding-mode comparison; Vulkan versus D3D12 per abstraction layer;
hot-spot attribution; limitations; recommendations for nvrhi usage.
