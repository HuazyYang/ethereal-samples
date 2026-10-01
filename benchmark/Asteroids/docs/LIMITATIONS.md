# Limitations and remaining differences

For the author of `report/report.md`. Everything here is a property of the benchmark as it is built and
run, not a finding about performance. Each item says where it lives in the sources, so that a claim in
the report can be checked against the code. Paths are relative to the repository root; line numbers are
those of the commits listed at the end.

Short forms: *sample* = `DiligentEngine/DiligentSamples/Samples/Asteroids/src`, *nvrhi* = `Donut/nvrhi`.

## 1. What a number means

- **Headline metric**: `render_ms + submit_ms` on the main thread. It contains the worker wake-up and
  join, the recording of the main thread's subset, the clears, the skybox, closing the command lists
  and handing them to the queue. It does **not** contain Present, fence waits or nvrhi's garbage
  collection. Work that a layer defers past `submit` is outside it, which is why two sensitivity
  metrics are reported next to it (`scripts/analyze.py`, module docstring):
  - `render + submit + present`: Diligent replays its deferred work inside `ISwapChain::Present`
    (back-buffer transition, its own submission, `FinishFrame` with the release queues), and on D3D11 the
    driver submits the whole frame inside Present for both the native renderer and Diligent
    (*sample* `asteroids_DE.cpp` `Render`, end; `asteroids_d3d11.cpp` `GetBenchmarkInfo` "columns").
    Diligent D3D11 with 8 threads had `present_ms` 1.09 ms against 0.23 ms with 1 thread in the review runs.
  - `render + submit + gc`: nvrhi releases what finished command lists referenced in
    `IDevice::runGarbageCollection`, which Donut calls once per frame after Present
    (`Donut/src/app/DeviceManager.cpp:768-770`). The `gc_ms` column is the whole interval between the end
    of Present and the start of the next frame, i.e. Donut's `sleep_for(0)` plus the garbage collection
    (`src/nvrhi/renderer.cpp` `OnBeforeFrame`); it is a part of `wait_ms`. On Vulkan with a volatile
    constant buffer per draw it was about 0.10 ms per frame at 1 thread (0.007 ms on D3D12), 0.28 ms
    with `-track_liveness` (8 s smoke runs, not measurements).
  Neither sensitivity metric is "the fair one". On Vulkan, Donut's Present also contains a wait (item 4),
  so `render + submit + present` overstates nvrhi's CPU work there.
- **All columns are main-thread wall-clock time.** Work done by worker threads is visible only through
  the time the main thread waits for them. CPU time summed over threads is not measured.
- **Overhead versus native is only like-for-like for `bindless`.** Native D3D12 binds all 10 textures
  as one descriptor table once per command list and picks the texture in the pixel shader
  (*sample* `asteroids_d3d12.cpp` `RenderSubset`: `SetGraphicsRootDescriptorTable(RP_TEX_SRV, ...)` before
  the draw loop; one `SetGraphicsRootConstantBufferView` + one draw per asteroid). `dyn`, `mut`, `tex_mut`
  and `tex_mut_pc` switch a texture binding per draw; their distance to native D3D12 is the cost of that
  binding model, not abstraction overhead. `analyze.py` writes the two into separate tables
  (`layer_overhead_vs_native.csv`, `binding_model_cost_vs_native.csv`) and labels every row
  (`native_comparison`). Native D3D11 sets one SRV per draw (`asteroids_d3d11.cpp` `Render`), so on D3D11
  the layers and the native renderer do bind per draw on both sides.
- **There is no native Vulkan renderer.** For Vulkan rows native D3D12 is a cross-API reference only
  (`overhead_vs_native_d3d12_cross_api_*` columns); it mixes API, driver and layer effects.
- **Native per-draw data is cheaper by construction.** Native D3D12 keeps a persistent 50,000-element
  constant buffer array in an upload heap per frame and writes only the two matrices per draw; the static
  colours were written once (`asteroids_d3d12.cpp` constructor, "Set any static asteroid data now").
  Diligent (`tex_mut`/`mut`/`dyn`) maps a dynamic buffer with DISCARD per draw and nvrhi writes a volatile
  constant buffer per draw; both copy the full per-draw structure every time.
- **`-gpu_timing` runs are not measurements.** The timestamp queries are recorded inside the timed
  columns (two `EndQuery` and a `ResolveQueryData` on D3D12, `Begin/End` on D3D11, `EndQuery` in Diligent,
  `beginTimerQuery/endTimerQuery` in nvrhi); only the readback/polling was moved out of `render`/`submit`
  (`src/common/benchmark_d3d12.h` `Collect`, `benchmark_d3d11.h` `Poll`, `asteroids_DE.cpp` `GpuTimerPoll`,
  `src/nvrhi/renderer.cpp` `PollGpuTimers`). `analyze.py` uses such runs for `gpu_ms` and `gpu_over_frame`
  only, and `run_matrix.ps1` refuses to mix them into a session of untimed runs.
- **`gpu_ms` is not defined identically.** Native and Diligent take the first timestamp at the start of
  the first command list and the second at the end of the last one; nvrhi begins its timer query in the
  main thread's command list and ends it in the skybox list. With several command lists all of them lie
  between the two timestamps in execution order, but the value is a span on the GPU timeline, not GPU
  busy time.

## 2. Differences that remain by design

| # | Difference | Where |
|---|---|---|
| 2.1 | **Bindless instance data upload.** Diligent maps a dynamic structured buffer with `MAP_FLAG_DISCARD` and writes into it in place. nvrhi has no dynamic buffers other than volatile *constant* buffers, so the nvrhi renderer fills a CPU staging array and calls `writeBuffer`, which copies it into the command list's upload chunk and records a GPU `CopyBufferRegion` into a default-heap buffer, with two state transitions per command list (ShaderResource -> CopyDest -> ShaderResource). One extra memcpy of 4.8 MB per frame in total and a GPU copy. | *sample* `asteroids_DE.cpp` `RenderSubset` (`MapHelper<AsteroidData> ... MAP_FLAG_DISCARD`); `src/nvrhi/renderer.cpp` `RecordAsteroids` `BindingMode::Bindless`; *nvrhi* `src/d3d12/d3d12-buffer.cpp` `CommandList::writeBuffer` (non-volatile branch, `CopyBufferRegion`) |
| 2.2 | **nvrhi D3D11 updates constants with `UpdateSubresource`, not Map/DISCARD.** A volatile constant buffer is created with `D3D11_USAGE_DEFAULT` unless `cpuAccess == Write` (the renderer does not set it, as no nvrhi application does for volatile buffers) and `writeBuffer` then calls `UpdateSubresource`; `setPushConstants` also calls `UpdateSubresource` on an internal buffer, once per draw. nvrhi's ProgrammingGuide says the opposite ("On DX11, VCBs directly map to buffers with the `D3D11_USAGE_DYNAMIC` usage"). Native D3D11 and Diligent D3D11 use Map with WRITE_DISCARD. | *nvrhi* `src/d3d11/d3d11-buffer.cpp:56-71` (usage), `:147-181` (`writeBuffer`), `src/d3d11/d3d11-commandlist.cpp:169-179` (`setPushConstants`), `doc/ProgrammingGuide.md:74`; *sample* `asteroids_d3d11.cpp` draw loop |
| 2.3 | **nvrhi D3D11 and native D3D11 are single-threaded.** The nvrhi D3D11 backend has one command list that maps to the immediate context and no deferred command lists; the native D3D11 renderer uses the immediate context only. Diligent D3D11 records on deferred contexts. D3D11 thread scaling therefore exists for Diligent only, and the runner schedules the other two at 1 thread only. | `src/nvrhi/main.cpp` (the `-threads` check with the nvrhi source reference); *sample* `WinWrapper.cpp` ("the native D3D11 renderer is single-threaded") |
| 2.4 | **nvrhi cannot express `dyn`.** Binding sets are immutable; there is no equivalent of changing a variable of one SRB between draws. Reported as N/A. nvrhi has `tex_mut_pc` (push constants) instead, which Diligent's renderer does not have. | `src/common/benchmark.h` `CombinationSupported` |
| 2.5 | **nvrhi records one more command list when multithreaded.** With N threads nvrhi executes N asteroid lists plus one list for the skybox (the main thread's list must be first and the skybox last), all in one `executeCommandLists`. Native D3D12 always has a pre and a post list around the subset lists. Diligent draws the skybox on the immediate context after executing the deferred lists. | `src/nvrhi/renderer.cpp` `Render` (`m_PostCommandList`); *sample* `asteroids_d3d12.cpp` `Render`; `asteroids_DE.cpp` `Render` |
| 2.6 | **nvrhi places two back-buffer barriers per command list.** Donut creates the swap-chain textures with `initialState = Present`, `keepInitialState = true`, so every command list that draws to the back buffer transitions it Present -> RenderTarget and back at close: 2 x (N + 1) transitions per frame with N threads, against 2 per frame in native D3D12 and Diligent. | `Donut/src/app/dx12/DeviceManager_DX12.cpp:483-484`, `Donut/src/app/vulkan/DeviceManager_VK.cpp:1042-1043`; *nvrhi* `src/common/state-tracking.cpp` (`keepInitialState`) |
| 2.7 | **Shader toolchains differ.** Native: FXC at build time (`*_5_0`, `asteroid_ps` as `ps_5_1`). Diligent: HLSL at run time, d3dcompiler for D3D, built-in glslang for SPIR-V. nvrhi: Donut's ShaderTool at build time, d3dcompiler (`*_5_0`) for D3D11, DXC `*_6_5` DXIL for D3D12, DXC `-spirv` for Vulkan. The shaders are small and the workload is CPU-bound, but GPU time is not identical. | `docs/BUILD.md` "Shader compilation per API"; `src/nvrhi/shaders/` |
| 2.8 | **Native D3D12 renders slightly less.** It skips the colour clear (the skybox overwrites everything) and its sampler uses CLAMP addressing; Diligent, nvrhi and native D3D11 clear colour and use WRAP. In the frame-30 captures this makes no visible difference (`report/parity/parity.txt`: the three D3D12 images differ by at most 2/255 in any channel, the two Vulkan images are identical, D3D12 versus Vulkan differs in 0.001 % of the pixels), but the GPU work is not identical. | *sample* `asteroids_d3d12.cpp` (`ClearRenderTargetView` commented out; sampler `D3D12_TEXTURE_ADDRESS_MODE_CLAMP`); `asteroids_DE.cpp`, `asteroids_d3d11.cpp`, `src/nvrhi/renderer.cpp` (Wrap) |
| 2.9 | **Native D3D12 in benchmark mode is not the stock sample.** Three deliberate changes, all only with `-benchmark`: (a) persistent worker threads with the Diligent renderer's scheme instead of `concurrency::parallel_for`; (b) the static vertex/index data lives in a DEFAULT heap instead of the UPLOAD heap (`-native_geometry_heap upload` restores the stock behaviour); (c) the swap chain is created with `ALLOW_TEARING` and presented with `DXGI_PRESENT_ALLOW_TEARING`, as Diligent and Donut do (`-native_present default` restores the stock behaviour; also applies to native D3D11). Reasons in item 3. `run.json` `extra` records `threading`, `geometry_heap`, `present_allow_tearing`. | *sample* `asteroids_d3d12.cpp` (`StartWorkers`, `WorkerMain`, `CreateMeshes`, `ResizeSwapChain`), `WinWrapper.cpp` (defaults), `benchmark_support.h` (`Options`) |
| 2.10 | **Worker dispatch is equivalent, not identical.** All three multi-threaded renderers use main + N-1 persistent workers, `ceil(50,000 / N)` asteroids per subset, the main thread spinning with `yield()` until the workers are done. Diligent wakes its workers with two `Threading::Signal` objects; native D3D12 and nvrhi use one mutex + condition variable + generation counter. The simulation update uses the same workers and is outside `render_ms`. | `asteroids_DE.cpp` `WorkerThreadFunc`; `asteroids_d3d12.cpp` `WorkerMain`; `src/nvrhi/renderer.cpp` `WorkerMain` |
| 2.11 | **RTTI.** The reference executable and the Diligent libraries are compiled with `/GR-`; `asteroids_nvrhi`, Donut and nvrhi keep RTTI because nvrhi's validation layer and Donut's scene graph use `dynamic_cast`. All other code-generation options are identical. | `docs/BUILD.md` "Release code generation"; root `CMakeLists.txt` |

## 3. Frame pacing: `frame_ms` is not comparable across renderers when GPU- or present-bound

Donut owns the nvrhi frame loop (`DeviceManager::RunMessageLoop`), so pacing and swap-chain modes differ
and cannot be aligned without editing Donut:

| Renderer | Frames the CPU may run ahead | Present mode | Where the CPU blocks (column) |
|---|---|---|---|
| native D3D12 | 3 (`NUM_FRAMES_TO_BUFFER`), a fence per frame slot | flip-discard, 5 buffers, tearing (benchmark mode) | `WaitForReadyToRender` before the frame (`wait_ms`) |
| native D3D11 | driver-defined | flip-sequential, 5 buffers, tearing (benchmark mode) | inside Present (`present_ms`) |
| Diligent D3D11/D3D12 | waitable swap chain, maximum frame latency = buffer count = 5 | flip-sequential, tearing when supported | inside `ISwapChain::Present` (`present_ms`); `wait_ms` is 0 |
| Diligent Vulkan | swap-chain images | **MAILBOX** preferred, then IMMEDIATE, then FIFO | inside Present / acquire (`present_ms`) |
| nvrhi D3D12 (Donut) | one fence per back buffer (5): waits until the frame that last used this buffer finished | flip-discard, 5 buffers, tearing when supported | `DeviceManager_DX12::BeginFrame` (`wait_ms`) |
| nvrhi Vulkan (Donut) | `maxFramesInFlight` = 3 | **IMMEDIATE** | the wait for the frame 3 frames ago is at the end of `DeviceManager_VK::Present` (`present_ms`); `vkAcquireNextImageKHR` is in `wait_ms` |

Sources: *sample* `asteroids_d3d12.cpp` `WaitForReadyToRender`, `settings.h`;
`DiligentEngine/DiligentCore/Graphics/GraphicsEngineD3DBase/include/SwapChainD3DBase.hpp:60,187,216-223`;
`DiligentCore/Graphics/GraphicsEngineVulkan/src/SwapChainVkImpl.cpp:331-348`;
`Donut/src/app/dx12/DeviceManager_DX12.cpp:540-588`; `Donut/src/app/vulkan/DeviceManager_VK.cpp:994,1327-1425`.

Consequences:

- When a configuration is GPU-bound or present-bound, `frame_ms`, `wait_ms` and `present_ms` measure the
  pacing scheme, not the layer. Only compare `frame_ms` between renderers for configurations that pass the
  CPU-bound check (`cpu_bound_check.csv`: `gpu_over_frame`, half-resolution frame time).
- `present_ms` of nvrhi Vulkan contains a GPU wait whenever the GPU is more than 3 frames behind, so
  `render + submit + present` is not pure CPU work there.
- At the highest frame rates the workload is close to the GPU's limit. Smoke runs of this phase (8 s,
  not measurements): native D3D12 with 8 threads has `frame_ms` 1.13-1.14 ms with `gpu_ms` 1.11 ms;
  Diligent D3D12 `tex_mut` with 8 threads had `gpu_ms` 1.17 ms at `frame_ms` 1.80 ms. Expect the fastest
  configurations (`bindless` and native at 8 and 15 threads) to be at or near GPU-bound. Their
  `render + submit` is still CPU work on the main thread, but with the GPU as the bottleneck the main
  thread is descheduled in the pacing wait between frames, which can change cache and frequency state.
- **Presentation pacing depends on whether the window is displayed (cause found, see 3.1).** A swap chain
  that presents without tearing is paced by the desktop compositor while its window is really on screen,
  and is not paced at all while the window is not displayed. Diligent Vulkan is the one benchmark
  configuration this applies to: its `frame_ms` and `present_ms` are the refresh period, not a property of
  the renderer.
- **Native D3D12 before the fixes** (review finding, confirmed): with the geometry in the UPLOAD heap
  `gpu_ms` was 2.42-2.43 ms per frame at 1 and at 8 threads, against 1.11-1.21 ms with a DEFAULT heap.
  At 8 threads the stock renderer was therefore GPU-bound (`wait_ms` 1.25-1.5 ms of a 2.4-2.7 ms frame).
  With DEFAULT-heap geometry and tearing, 8 threads: `wait_ms` 0.004 ms, `frame_ms` 1.13 ms.

### 3.1 Presentation pacing: what triggers it and what the tooling does about it

The panel (120 Hz) is driven by the integrated AMD GPU and the RTX 4050 presents through it, so every
windowed present goes through the desktop compositor (DWM). All numbers below are 2-4 s runs made while
investigating, not measurements.

**Trigger.** What decides is whether the compositor displays the window, not the display's power state
as such:

| State of the benchmark window | Diligent Vulkan (MAILBOX), `tex_mut` 8 threads | Native D3D12 without the tearing flag, 8 threads | Presents with tearing / IMMEDIATE |
|---|---|---|---|
| displayed: display on, session unlocked, window not covered | `frame_ms` 8.33 ms = one frame per refresh, in every run (more than 50 runs of 6 Diligent Vulkan configurations whose own frame time is below 8.3 ms) | 2.7-3.4 ms | unaffected (native D3D12 1.13 ms) |
| display off (`SC_MONITORPOWER`) | 1.60-1.70 ms | not run | unaffected |
| lock screen up, display on | 1.67 ms (2 of 2) | 1.12 ms | unaffected |
| covered by another window (topmost full-screen window) | 1.58-1.78 ms | 1.13 ms | unaffected (within 0-4 %) |

While the window is displayed, a non-tearing present has to wait for the compositor to release a buffer;
while it is not displayed, the presents are dropped and return at once. The unattended quick matrix had
15 paced Diligent Vulkan runs because the desktop was visible; the unpaced smoke run earlier that day
(`frame_ms` 1.16 ms) and the one unpaced short-frame run inside that matrix must have had a hidden window
(display asleep, or the window opened behind another one - see "window on top" below; which of the two
was not recorded at the time).

**Diligent Vulkan cannot be un-paced from the sample.** DiligentCore chooses the present mode itself:
with vsync off it takes MAILBOX when the surface offers it, then IMMEDIATE, then FIFO
(`DiligentCore/Graphics/GraphicsEngineVulkan/src/SwapChainVkImpl.cpp:331-348`). `SwapChainDesc`
(`DiligentCore/Graphics/GraphicsEngine/interface/GraphicsTypes.h`, fields `Width`, `Height`,
`ColorBufferFormat`, `DepthBufferFormat`, `Usage`, `PreTransform`, `BufferCount`, `DefaultDepthValue`,
`DefaultStencilValue`, `IsPrimary`) has no present-mode field, and `ISwapChain::Present(SyncInterval)`
only switches between the vsync list and the no-vsync list. The sample therefore cannot request IMMEDIATE;
DiligentCore is not modified. Donut uses IMMEDIATE (`Donut/src/app/vulkan/DeviceManager_VK.cpp:994`), so
**nvrhi Vulkan is not paced and Diligent Vulkan always is on a visible desktop**: `frame_ms`, `present_ms`
and `render + submit + present` of Diligent Vulkan must not be compared with anything, and the
frame-time-based CPU-bound check (half resolution, `gpu_ms / frame_ms`) cannot be made for it on a visible
desktop.

**Is `render + submit` affected?** Slightly, and not in one direction. Paired runs of the same
configuration, window displayed (paced) against window covered (unpaced), 3 runs of 4 s each, medians:

| Diligent Vulkan | paced | unpaced | paced / unpaced |
|---|---|---|---|
| `tex_mut` 1 thread | 3.904 ms | 3.803 ms | 1.027 |
| `tex_mut` 8 threads | 1.150 ms | 1.192 ms | 0.965 |
| `bindless` 1 thread | 2.083 ms | 2.034 ms | 1.024 |
| `bindless` 8 threads | 0.713 ms | 0.684 ms | 1.042 |
| `dyn` 15 threads | 2.838 ms | 3.065 ms | 0.926 |

The ranges of the three paced and the three unpaced runs did not overlap in any of the five rows, so the
differences of 2-7 % are real, but they are small and have no common sign; do not quote a Diligent Vulkan
`render + submit` difference below about 7 % as a finding. The
renderers that present with tearing showed no difference between a displayed and a covered window beyond
their spread (Diligent D3D12, nvrhi D3D12 and Vulkan, native D3D12: 0-4 %).

**What the tooling does.**

- *Window on top.* Both executables make their window topmost in benchmark mode
  (`Benchmark::KeepWindowOnTop`, `src/common/benchmark.h`; called in `src/nvrhi/main.cpp` and the sample's
  `WinWrapper.cpp` `CreateDemoWindow`). A window created by a background process is otherwise opened behind
  the active window whenever Windows denies it the foreground (it does for a while after any input), and a
  covered window is not paced: that made single runs of a session differ.
- *Recorded per run* (`run.json` `extra`, `Benchmark::AddDesktopFacts`): `display_refresh_hz`,
  `window_foreground`, `window_visible_fraction` (share of a 5 x 5 grid of points of the window at which it
  is the topmost window, sampled at the end of the run), `window_topmost`, and `present_mode` /
  `swap_chain_tearing`. The present mode is what the renderer can know: Diligent Vulkan reports the mode
  DiligentCore logged (`VK_PRESENT_MODE_MAILBOX_KHR` here); the DXGI renderers and Donut report their fixed
  scheme together with the result of `DXGI_FEATURE_PRESENT_ALLOW_TEARING`. nvrhi D3D11 presents without
  the tearing flag (`Donut/src/app/dx11/DeviceManager_DX11.cpp:426`); none of its runs in the quick matrix
  is flagged as paced (its frames take 6 ms or more), but it was not tested against a covered window.
- *Runner* (`scripts/run_matrix.ps1`): keeps the display and the system awake for the session
  (`SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED)`, restored at exit);
  refuses to start, and stops before the next run, when the display is off (`GUID_CONSOLE_DISPLAY_STATE`)
  or the session is locked (`LogonUI.exe` in the session, the lock screen app in the foreground, or an
  input desktop other than `Default`), unless `-AllowHiddenDesktop`; writes the desktop state of every run
  and its `window_visible_fraction` into `manifest.json` and warns when a window was not fully visible.
  Both refusals were exercised (display switched off, then the lock screen). `-CoverWindow` does the
  opposite on purpose: a topmost full-screen window covers every benchmark window, so that no renderer is
  paced; it is a session of its own (a resume that would mix it with displayed runs is refused) and the
  only way to get an unpaced `frame_ms` for Diligent Vulkan, e.g. for the CPU-bound check.
- *Analysis* (`scripts/analyze.py`, module docstring "paced runs"): a run is `paced` when its median
  `frame_ms` is within 2 % of a multiple (or half) of the refresh period, update + render + submit fill at
  most 95 % of the frame, and per frame `wait + present` falls when update + render + submit rise
  (correlation at most -0.4). The last condition is needed: nvrhi D3D12 `tex_mut` with 1 thread needs
  8.0-8.4 ms per frame by itself and would otherwise be flagged; in the recorded runs the paced ones have a
  correlation of -0.59 to -0.98 and the others of 0.0 to +0.5. Paced runs are excluded from `frame_ms`,
  `present_ms`, `wait_ms`, `render + submit + present` and the CPU-bound check, and stay in
  `render + submit`. The same exclusion applies to a run whose window was hidden in a session of displayed
  windows, or the other way round. `summary.csv` has `runs_paced`, `runs.csv` has `paced`,
  `pacing_multiple`, `window_visible_fraction`, `present_mode`; `validation.json` lists every paced run and
  every configuration without an unpaced run.

Not determined: why the stock native swap chain without the tearing flag settles at 2.7-3.4 ms per frame
rather than at a fraction of the refresh period (the earlier observation of exactly 240 frames per second
was not reproduced); benchmark mode uses the tearing flag, so no measured configuration depends on it.

## 4. nvrhi usage in `asteroids_nvrhi`

The renderer is written the way a competent nvrhi application would be for this workload; the following
choices all reduce nvrhi's cost relative to its defaults and should be named in the report
(`src/nvrhi/renderer.cpp`):

- **Permanent resource states** for the vertex, index, instance-id buffers and all textures
  (`setPermanentBufferState` / `setPermanentTextureState`), so command lists never track them.
- **`trackLiveness = false`** on every binding set (nvrhi's default is `true`): the sets outlive all
  command lists, so the command lists do not add a reference per use. Switch: `-track_liveness`.
- **Upload chunk size 1 MB** instead of the default 64 KB (`CommandListParameters::setUploadChunkSize`):
  on D3D12 a volatile constant buffer version is suballocated per draw, 64 KB would hold 256 draws.
- **Redundant binding-set filter in the application**: `setGraphicsState` is only called when the texture
  binding set differs from the bound one. Texture indices are random per asteroid, so with 10 textures
  about 90 % of the draws still change the set. Switch: `-always_set_state`.
- **Automatic barriers stay on** (nvrhi's default). Switch: `-no_auto_barriers` disables them after the
  first `setGraphicsState` of each command list.
- **The sampler is not part of the per-texture binding sets** (a D3D12 sampler heap holds 2048
  descriptors; `mut` needs 50,000 sets); it lives in the per-thread set with the constants.
- **One volatile constant buffer per thread**, `maxVersions = subset size x 8` (`kVolatileFrames`): on
  Vulkan every write claims a version with an atomic compare-exchange, and a version is only reusable
  after the GPU finished the list that used it. With 1 thread that is 400,000 versions of a 160-byte
  buffer (`run.json` `extra.volatile_cb_max_versions`). nvrhi's ProgrammingGuide states that a buffer with
  too few versions produces a runtime error rather than a slowdown.
- **Command lists are not "immediate"** when more than one is open (`setEnableImmediateExecution(false)`).

The sensitivity set (`run_matrix.ps1 -Set sensitivity`) measures `-always_set_state`, `-track_liveness`
and `-no_auto_barriers` singly, on D3D12 and Vulkan `tex_mut` at 1 and 8 threads, against a baseline in
the same session, and on D3D12 the two placement switches `-cb_page_split_avoid` and `-cb_page_split_force`
(item 5.1; they are a control of the benchmark, not an nvrhi usage choice). `analyze.py` rejects a run
whose recorded switches do not match its directory. A sensitivity run on D3D12 is only comparable with a
baseline of the same `cb_page_split` population: `cb_page_split.csv` shows which variant rows contain
affected runs.
The Diligent renderer is the stock sample's code: it calls `CommitShaderResources` before every draw
without filtering in the application and maps with DISCARD per draw. Its `DRAW_FLAG_VERIFY_ALL` and
`RESOURCE_STATE_TRANSITION_MODE_VERIFY` arguments request checks that Diligent only performs in development
builds; `run.json` records `diligent_development_build` (false in the Release build) and
`diligent_error_count` (a run with a non-zero count is rejected by `analyze.py`).

## 5. Environment and statistics

- **One machine, a laptop.** Ryzen 7 8845HS, RTX 4050 Laptop GPU, Windows 10 (`docs/ENVIRONMENT.md`).
  No claim generalises to other CPUs, GPUs, drivers or desktop operating systems without re-measuring.
- **The power plan is recorded, not enforced** (`run_matrix.ps1` `Get-Environment`: `power_plan`,
  `ac_line`; it was "Balanced" at setup). The runner refuses to start a full session on battery, nothing
  more. Nothing pins threads, changes priorities or disables core parking.
- **Thermal drift.** Runs are randomized and interleaved in repetition rounds and separated by a
  cool-down, which spreads slow drift over all configurations but does not remove it.
- **Two per-process cost levels in nvrhi D3D12 (cause found: a page-split store inside nvrhi, see 5.1).**
  Every nvrhi D3D12 configuration that calls `setGraphicsState` per draw (`tex_mut`, `tex_mut_pc`, `mut`)
  runs on one of two levels that are fixed for the life of a process. Each run records which one it hit
  (`run.json` `extra.cb_page_split`), the analysis never pools the two, and two switches force either.
- **Spread.** Treat the min-max spread of the run medians as part of every result and do not quote
  differences smaller than it.
- **Three or five repetitions** give a median and a range, not a confidence interval.
- **15 threads on 16 logical processors** leaves one logical processor for the OS, the driver threads
  and the compositor; results at 15 threads are the most exposed to interference.
- **Donut and nvrhi are the user's forks**, not upstream: Donut `git@github.com:HuazyYang/Donut.git` at
  `62eab21` and nvrhi `git@github.com:HuazyYang/NVRHI.git` at `c8d34b4` (branch `lithereal-dev`).
  Donut has replaced ShaderMake with its own ShaderTool. Results describe these commits.
  DiligentEngine is at `e24e719`, DiligentCore at `bcb8b11`, DiligentSamples at upstream `7c96699` plus
  the local branch `nvrhi-benchmark` (`patches/diligent-samples-asteroids.patch`).
- **Adapter selection.** Every run is pinned to the RTX 4050 by LUID and fails if the device ends up on
  another adapter. Diligent is pinned by matching PCI vendor/device id and verified afterwards
  (`asteroids_DE.cpp` `FindAdapterId`, `VerifyAdapter`); two identical GPUs would be ambiguous.
- **Windowed, 1080 x 720, no GUI.** Benchmark mode removes the sample's sprites and text, so numbers are
  not comparable with the figures on the Diligent Asteroids web page, which include them.
- **Frame capture (`-capture_frame`) is for parity only.** nvrhi renders the capture frame into an
  offscreen texture (a Donut Vulkan swap-chain image has no TRANSFER_SRC usage); the reference renderers
  read back the back buffer.

### 5.1 The page-split store in nvrhi's D3D12 command list

All numbers are 3-8 s runs made while investigating, not measurements.

**Observation.** `nvrhi d3d12 tex_mut`, 1 thread: `render + submit` is either 5.6-5.9 ms or 9.4-10.1 ms,
constant for the life of the process, different from process to process (12 of 29 runs in the previous
phase and 23 of 76 runs at the start of this investigation were on the high level).

**Cause.** On every `setGraphicsState` that changes a binding set, nvrhi overwrites the whole member
`m_CurrentGraphicsVolatileCBs` of its D3D12 `CommandList`:

- `Donut/nvrhi/src/d3d12/d3d12-resource-bindings.cpp:1185`, `CommandList::setGraphicsBindings`:
  `m_CurrentGraphicsVolatileCBs = newVolatileCBs;`
- the member is `static_vector<VolatileConstantBufferBinding, c_MaxVolatileConstantBuffers>`
  (`d3d12-backend.h:1189`; 32 elements of 24 bytes plus the count = 776 bytes) at offset 0xB48 of the
  object; the compiler copies it with unaligned 32- and 16-byte stores (`vmovups`).
- The `CommandList` is a heap object (`d3d12-device.cpp:614`, `new CommandList`) of more than 4 KB; where
  the heap puts it differs from process to process. When a 4 KB page boundary falls inside the member
  (48 of the 256 possible placements, 19 %), one store of the copy straddles two pages.
- On this CPU (Ryzen 7 8845HS) such a page-split store costs about 80 ns in this draw loop, i.e.
  45,000 state changes x 80 ns = 3.8 ms per frame. In isolation it costs 1.4-7 ns; a standalone
  microbenchmark (`tests/microbench/page_split_store.cpp`) reproduces the large penalty only when
  write-combined memory is written between the stores, as the per-draw `writeBuffer` does with the upload
  chunk (160 bytes into a 256-byte slot of
  write-combined memory, then one 32-byte store to ordinary memory: 19.2 ns per iteration when the store is
  aligned, 72.0 ns when it straddles a page; without the write-combined write 0.6 against 7.3 ns; a mapped
  D3D12 upload buffer is write-combined here, `extra.d3d12_upload_heap_page_protection`). `tex_mut_pc`
  writes no upload memory per draw through nvrhi and is affected just the same, so write-combining is a
  reproduced sufficient condition, not a proven necessary one; what the driver writes per draw was not
  examined.

**Evidence.**

1. Sampling the main thread (an in-process sampler in a temporary build, instruction pointer once per
   millisecond): in every high process 32-36 % of all samples sit behind one store instruction, in the low
   ones there is no such instruction. In 4 of 5 sampled high processes it is a store of that copy loop, and
   in each the stalled store is the one that contains the page boundary computed from the object's address.
   In the fifth the stalled store was the application's own 32-byte store into its `DrawConstants` on the
   stack, which straddled a page there (fixed, see below).
2. Address arithmetic over 106 runs of an instrumented build: "a page boundary lies inside the member, or
   the application's stack struct straddles a page" predicted the level of 105 (23 high, 82 low); the
   remaining run was at 7.97 ms, between the levels.
3. Control (the final build): with `-cb_page_split_avoid` 10 of 10 runs were at 5.68-5.80 ms, with
   `-cb_page_split_force` 10 of 10 at 9.43-9.62 ms; in 16 runs without a switch the recorded
   `cb_page_split` was `true` in 3 (9.47-9.66 ms) and `false` in 13 (5.72-5.86 ms, two at 6.3-6.4 ms).

Ruled out on the way: the cost of `writeBuffer` itself (the time inside that call is the same on both
levels; the extra time is inside `setGraphicsState`), the alignment of the source struct, the distance
between the application's `GraphicsState` and the command list (a sweep of 32 placements), the offset of the
stack (a sweep of 32 offsets), the core the main thread runs on, the process's memory footprint, and the
`unordered_map` lookups keyed by a pointer.

**Which configurations.** Measured with both switches (medians, 3-10 runs each):

| nvrhi D3D12 | no page split | page split | ratio |
|---|---|---|---|
| `tex_mut`, 1 thread | 5.70 ms | 9.55 ms | 1.67 |
| `tex_mut`, 8 threads (all 8 lists) | 1.46 ms | 1.84 ms | 1.26 |
| `tex_mut_pc`, 1 thread | 5.72 ms | 9.17 ms | 1.60 |
| `mut`, 1 thread | 9.16 ms | 13.21 ms | 1.44 |
| `bindless`, 1 thread | 1.148 ms | 1.147 ms | 1.00 |

`bindless` calls `setGraphicsState` once per command list and is not affected. Each recording thread has
its own command list with its own 19 % chance: with 8 threads 81 % of the processes have at least one
affected list, with 15 threads 96 %, and the cost grows with the number of affected lists (8 threads,
no switch: 1.48-1.50 ms with 0, 1.46-1.62 ms with 1, 1.54-1.73 ms with 2-3 affected lists). nvrhi D3D11
and Vulkan have other code; `extra.cb_page_split` is `n/a` there. Diligent D3D12 (3.80-4.01 ms in 9 of 10
runs, 4.34 ms once) and native D3D12 (0.90-0.92 ms in 9 of 10, 0.98 ms once) showed no second level.

**Which level is the honest one.** Both are real for an nvrhi application on this CPU: about one command
list in five pays the penalty, by chance. The unaffected level is the cost of nvrhi's code; the affected
level is that plus a hardware penalty that depends on where the heap placed an object. The report must
give both and must not let chance pick one.

**What the benchmark does.**

- *Application side, fixed.* The per-draw structs on the stack (`DrawConstants`, and `AsteroidData` of the
  push-constant path) are `alignas(64)` (`src/nvrhi/renderer.cpp` `RecordAsteroids`): their 32-byte stores
  can no longer straddle a page. The stack's offset inside its page differs per process, and this was the
  cause of 2 of the 23 high runs above.
- *nvrhi side, recorded, not changed.* After the first frame the renderer locates the member inside the
  command list object by its contents (no hard-coded offset; `ProbeCommandListPlacement`) and records
  `cb_page_split` (`true` when at least one recording thread's list is affected),
  `cb_page_split_command_lists` (how many), `cb_page_split_state_offset` (2888 in this build),
  `command_list_page_offsets` and `draw_constants_page_offset`.
- *Switches, default unchanged.* `-cb_page_split_avoid` / `-cb_page_split_force` (D3D12 only) create
  command lists until every recording thread has one without / with a page boundary inside the member
  (`cb_page_split_candidates` counts the extra ones; the chosen lists are used from the second frame on).
  This is a placement control for the benchmark, not something an nvrhi application would do; the default
  remains whatever the heap hands out.
- *Runner.* `-Set sensitivity` runs both switches as variants `__cb_page_split_avoid` and
  `__cb_page_split_force` for D3D12 `tex_mut` at 1 and 8 threads.
- *Analysis* (`scripts/analyze.py`, module docstring "cb_page_split"): the populations are never pooled. A
  configuration's metrics come from its unaffected runs when it has any (`summary.csv` column
  `population`, `runs_cb_page_split`, `cb_page_split_cpu_render_ms`, `cb_page_split_over_unaffected`);
  `cb_page_split.csv` lists both populations per configuration, and `validation.json` warns with
  `cb_page_split_mixed` / `cb_page_split_all`. With several threads an unaffected run is rare, so for thread
  scaling use the two variants, which are well defined at every thread count, rather than the default rows.

**Not explained.**

- About one unaffected run in six is 5-10 % higher (6.1-6.4 ms instead of 5.6-5.9 ms at `tex_mut` 1 thread;
  2 of 20 runs with `-cb_page_split_avoid`, 2 of 13 without a switch; 7.3 ms and 7.97 ms once each in the
  instrumented build). In the two such processes that were sampled the extra samples were inside
  `CommandList::setGraphicsState`, at different instructions; the page boundary of the command list object
  lay outside the member above (at offsets 0x800 and 0xFC0), and no single cause was identified. Treat a
  spread of up to 10 % between repetitions of these configurations as unexplained.
- nvrhi Vulkan `tex_mut` with 1 thread also has two groups (11.0-11.5 ms and 11.9-12.2 ms, 4 of 18 runs in
  the higher one). In two of the four the page boundary lay inside a 784-byte member at offset 0x660 of the
  Vulkan `CommandList` that `setGraphicsState` overwrites with 32-byte stores, and the sampler showed the
  stalled store there in the one that was sampled; the other two are not explained. Nothing is recorded or
  controlled for Vulkan beyond `command_list_page_offsets`.
- For the profiling phase, seen in passing and not examined further: in 9 of 10 sampled nvrhi Vulkan
  `tex_mut` 1-thread processes 19-22 % of the main thread's samples were behind the `lock cmpxchg` of
  `nvrhi::vulkan::CommandList::writeVolatileBuffer` (the atomic claim of a buffer version).

## 6. Validation state of this phase

- The quick matrix (`run_matrix.ps1 -Quick`: 2 s warmup, 3 s measured, one run per configuration) is a
  smoke pass that proves every scheduled configuration runs and the pipeline works end to end. Its
  numbers are not results and are not in `report/`.
- Numbers quoted in this file come from 8 s single runs made while fixing; they explain decisions and
  must not be used as findings.
- After the changes of items 3.1 and 5.1 the quick headline set (`run_matrix.ps1 -Quick -Set headline`,
  18 runs) and a quick D3D12 sensitivity set with 3 repetitions (36 runs) ran without a failed run; the
  analysis flagged the 4 Diligent Vulkan runs as paced and no other run, and split the nvrhi D3D12
  populations as recorded. The refusal of the runner with the display off and at the lock screen, and
  `-CoverWindow`, were each exercised once by hand. A full-length session has not been run with them.
