# MultithreadedRendering benchmark

The CPU-side cost of nvrhi (through Donut) against hand-written D3D12 on the workload of the
MultithreadedRendering sample: the SquidRoom scene (1014 meshes, 1025 subset draws per pass) rendered
six times per frame, into one shadow map, into four stencil mirrors and as the direct view, with the
command recording done single-threaded, one command list per pass, or split across threads.

Ported from `D3D12-Trial/MultithreadedRendering` (itself a D3D12 port of the DirectX SDK's
MultithreadedRendering11). The native renderer is that sample's D3D12 code reduced to the frame work;
the nvrhi renderer renders exactly the same frame through Donut + nvrhi on D3D12 and Vulkan.

| Directory | Content |
|---|---|
| `common/` | The benchmark contract (`benchmark.h`: command line, timers, CSV/JSON writers) and the D3D12 debug-layer message drain; the Asteroids schema with `-mode` in place of `-binding` |
| `core/` | Shared by both renderers: the `.sdkmesh` reader, a DDS reader, the scene (lights, mirrors, camera, constant buffer layouts, per-pass parameters), the worker pool and the frame schedule of the three modes |
| `native/` | `mt_rendering_native`: the sample's D3D12 renderer (root signature, 34 pipeline states, shadow map, stencil mirrors) under a minimal Win32 host |
| `nvrhi/` | `mt_rendering_nvrhi`: the same workload on Donut + nvrhi, D3D12 and Vulkan |
| `shaders/` | `mt_scene.hlsl`, the sample's vertex and pixel shader; one source for fxc (native) and ShaderTool (nvrhi) |
| `scripts/` | `run_matrix.ps1`: the run matrix and its summary table |

Media: `SquidRoom/SquidRoom.sdkmesh` and its DDS textures (43 MB, from `directx-sdk-samples/Media`) in this
repository's `media/`, found as `<exe dir>/../media` like the Donut examples' media (`-media DIR` overrides).

## Build and run

Part of the ethereal aggregate (`-DETHEREAL_BUILD_BENCHMARKS=ON`, the default of `build.ps1`):

```powershell
..\..\..\build.ps1 -Target mt_rendering_native,mt_rendering_nvrhi
..\..\..\build\bin\mt_rendering_native.exe -benchmark -renderer native -api d3d12 -mode mt_chunk -threads 4 -output out\native
..\..\..\build\bin\mt_rendering_nvrhi.exe  -benchmark -renderer nvrhi  -api vk    -mode mt_chunk -threads 4 -output out\nvrhi
scripts\run_matrix.ps1                      # the whole matrix into results\<session>\, summary.md at the end
```

Options (`-help` prints them):

| Option | Meaning |
|---|---|
| `-benchmark` | scripted camera orbit driven by the wall clock (every renderer sees the same views over the measured window), auto-exit after `-warmup S` + `-duration S` (defaults 5 + 20), results in `-output DIR` |
| `-renderer native\|nvrhi`, `-api d3d12\|vk` | the native renderer is D3D12 only |
| `-mode st\|mt_scene\|mt_chunk`, `-threads N` | see below; `st` needs `-threads 1`, the others at least 2. `N` counts the main thread |
| `-window W H` | back buffer size, default 1280 x 720 |
| `-adapter_luid N` | fail unless the device is created on that adapter (the LUIDs are printed at start) |
| `-mirror_scissor` | the sample's "tight mirror stencil clipping space" option (off by default, as in its UI) |
| `-explicit_barriers` | nvrhi only, see "Barriers" |
| `-debug` | D3D12 debug layer / Vulkan validation and nvrhi's validation layer; their messages go to stderr |
| `-freeze T`, `-screenshot FILE.bmp` | render the scripted frame at time T; save the 30th frame and exit (parity checks) |

Interactive (no `-benchmark`): left mouse orbits, wheel zooms, keys 1/2/3 switch the mode (within the thread
count given), M toggles the mirror scissor, Escape quits. The window title shows the render and submit times.

Output: `DIR/frames.csv` (one row per measured frame: `update_ms, render_ms, submit_ms, wait_ms, present_ms,
frame_ms, draw_count, index_count, gc_ms`) and `DIR/run.json` (medians, the configuration, the adapter, a hash
of the loaded geometry and material list, and renderer facts under `extra`). The columns are the main thread's
wall-clock time: `render` from the start of recording until every command list of the frame is recorded (workers
joined), `submit` the Execute calls. `median_cpu_render_ms` = render + submit.

## The workload

Per frame, in this order, as the sample does it:

1. **Shadow pass**: the whole model into the 2048 x 2048 shadow map of light 0 (depth only, biased).
   Frame begin (clears of the back buffer and the depth/stencil buffer) is recorded at the head of this pass.
2. **Mirror passes** (4, back-facing ones skipped): a stencil quad marks the mirror, a second quad "clears"
   the depth inside it, the whole model is drawn reflected (stencil test, flipped winding, tint, clip plane),
   and the mirror surface is drawn to write its depth and clear the stencil bit.
3. **Direct pass**: the whole model.

Every pass binds two textures per subset (diffuse, normal), the shadow map, and three constant buffers
(object, lights, scene). From the default camera two of the four mirrors face it, so a frame records
4 x 1025 = 4100 subset draws plus 6 quad draws; along the benchmark's camera orbit one to two mirrors face the
camera (3075 or 4100 draws). Lights swing with time; in benchmark mode the scene time is the wall clock since the
first frame, so every renderer draws the same views over the same measured window whatever its frame rate
(`draws/frame` in the summary is the time-weighted mean and must match between renderers).

Modes (`core/frame_scheduler.h`, the same code for both renderers):

| Mode | Threads | Command lists per frame | Execute calls |
|---|---|---|---|
| `st` | 1 | 1 | 1 |
| `mt_scene` | N >= 2 (5 jobs + main) | 6: the shadow map and the 4 mirrors are jobs on the worker pool, the direct view is recorded on the main thread | 1, all six lists in pass order |
| `mt_chunk` | N >= 2 | 6 x N: every pass is split into N chunks of the mesh list (mesh `i` belongs to chunk `i % N`); chunk 0 records the pass prologue, chunk N-1 its epilogue | 6, one per pass as soon as its chunks are recorded |

The worker pool (`core/workers.h`) is shared: persistent threads woken through a condition variable, jobs
handed out by an atomic counter, the main thread spinning until the jobs are done. The sample itself used
a Win32 thread pool for `mt_scene` and dedicated event-driven threads for `mt_chunk`.

## What each renderer does

Native D3D12 (`native/renderer_d3d12.cpp`), the sample's approach kept:

- one root signature: root CBVs b0/b1/b2, three one-descriptor SRV tables (t0 diffuse, t1 normal, t2 shadow map),
  two static samplers; the 34 pipeline states of the sample (shadow, direct, 8 stencil-bit variants of the
  mirror stencil / depth clear / reflected scene / mirror surface pipelines);
- constant buffers pushed into a per-frame upload stack of 16 KB blocks (256-byte slices, one lock per push,
  shared by all threads of the frame), bound as root CBVs once per pass;
- per subset: `IASetPrimitiveTopology`, two `SetGraphicsRootDescriptorTable`, `DrawIndexedInstanced`;
  per mesh: `IASetVertexBuffers`, `IASetIndexBuffer` (one default-heap buffer per sdkmesh buffer, as DXUT creates them);
- three frames in flight, one command allocator per (frame, pass, chunk), the lists reused across frames;
  flip-discard swap chain with 3 buffers and tearing allowed, RGBA8 with an sRGB render target view, D24S8 depth.

Differences from the sample's code, all without effect on the recorded GPU work: the framework (DXUT camera,
ImGui, DirectXTex, D3D12MA, run-time shader compilation) is replaced by a minimal host, fxc-compiled shader
headers and the readers in `core/`; the sample's vertex-only pipelines declared no render target while the back
buffer stayed bound, which D3D12 treats as a format mismatch (the driver dropped those draws here), so they
declare the bound format with a zero colour write mask instead.

nvrhi (`nvrhi/renderer.cpp`):

- two binding layouts: layout 0 (per recording chunk) with three volatile constant buffers, the shadow map and
  the two samplers; layout 1 (one binding set per material, 50) with the diffuse and normal map. Depth-only
  pipelines use a layout-0 variant without the light buffer (nvrhi rejects a bound volatile buffer the list did
  not write). The material textures are `space1` in the shader: on Vulkan a binding layout is a descriptor set
  and DXC maps register spaces to sets (`registerSpaceIsDescriptorSet`);
- per subset: `setGraphicsState` (pipeline, framebuffer, viewport, binding sets, vertex/index buffers) and
  `drawIndexed`; nvrhi diffs the state and rebinds what changed (the material set, the buffers);
- constant buffers: `writeBuffer` into the chunk's volatile buffers once per pass (nvrhi suballocates them from
  the command list's upload manager);
- the same 34 pipelines; the stencil reference is baked into the 8 variants (nvrhi has no per-draw stencil
  reference without `dynamicStencilRef`). Vulkan: the shadow pass has no depth bias clamp (Donut's device does
  not enable the `depthBiasClamp` feature);
- Donut's DeviceManager owns the window, swap chain (3 buffers) and depth buffer (D24S8); the benchmark
  columns are taken at its pipeline callbacks as in the Asteroids benchmark (`wait` = frame fence / acquire
  plus Donut's per-frame garbage collection, reported as `gc_ms` too).

### Barriers

By default nvrhi's automatic barriers are on: every `setGraphicsState` checks the states of the bound resources,
each list transitions the back buffer and the depth buffer (and the shadow pass lists the shadow map) and
restores them at close. The native renderer places six barriers per frame by hand. `-explicit_barriers` turns
the automatic barriers off in the mirror and direct pass lists (the five passes that carry all but the shadow
map's draws): the framebuffer states are set once per list with `setResourceStatesForFramebuffer`, the
shadow pass keeps automatic barriers (its clears and transitions are backend specific). The default passes the
D3D12 debug layer and the Vulkan validation layer in every mode; the explicit variant does so on D3D12, while on
Vulkan the validation layer reports the shadow map's layout as `SHADER_READ_ONLY_OPTIMAL` where the descriptor
expects `DEPTH_STENCIL_READ_ONLY_OPTIMAL` (nvrhi restores the texture's initial state with the generic layout;
harmless on this driver, and absent with automatic barriers).

## Parity

`-freeze 2 -screenshot` renders the same scripted frame in every renderer; `shot_*.bmp` of the native D3D12
renderer, nvrhi D3D12 and nvrhi Vulkan, in every mode, were compared pixel by pixel: native and nvrhi D3D12 are
identical (largest difference 1 of 255), nvrhi Vulkan differs by at most 27 of 255 on isolated texels (PSNR 80 dB,
filtering / rounding), no pixel differs by more than 16 in any mode. `scene_hash` in `run.json` (the sdkmesh
bytes, material texture names and draw order) is the same for both executables.

## Results

See `results/<session>/summary.md` written by `scripts/run_matrix.ps1`. The table below is the session of
2026-10-17 on this machine (RTX 4050 Laptop GPU, 1280 x 720, 3 s warmup, 10 s measured, medians in ms; native
D3D12 and the nvrhi renderers built alike, `ASTEROIDS_MATCH_CODEGEN`).

| configuration | render | submit | wait | present | frame | frames | draws/frame |
|---|---:|---:|---:|---:|---:|---:|---:|
| native_d3d12_st_t01 | 0.248 | 0.030 | 0.000 | 0.198 | 0.556 | 16340 | 3845 |
| native_d3d12_mt_scene_t06 | 0.116 | 0.035 | 0.000 | 0.204 | 0.583 | 16813 | 3846 |
| native_d3d12_mt_chunk_t02 | 0.267 | 0.067 | 0.026 | 0.192 | 0.609 | 16513 | 3835 |
| native_d3d12_mt_chunk_t04 | 0.262 | 0.073 | 0.001 | 0.194 | 0.610 | 16464 | 3837 |
| native_d3d12_mt_chunk_t08 | 0.307 | 0.126 | 0.000 | 0.209 | 0.655 | 14707 | 3851 |
| nvrhi_d3d12_st_t01 | 1.312 | 0.039 | 0.033 | 0.192 | 1.583 | 6428 | 3826 |
| nvrhi_d3d12_st_t01__explicit_barriers | 1.306 | 0.041 | 0.033 | 0.196 | 1.584 | 6423 | 3827 |
| nvrhi_d3d12_mt_scene_t06 | 0.468 | 0.058 | 0.043 | 0.194 | 0.778 | 12489 | 3861 |
| nvrhi_d3d12_mt_chunk_t02 | 0.842 | 0.125 | 0.039 | 0.189 | 1.207 | 8308 | 3832 |
| nvrhi_d3d12_mt_chunk_t04 | 0.645 | 0.167 | 0.045 | 0.194 | 1.067 | 9342 | 3837 |
| nvrhi_d3d12_mt_chunk_t04__explicit_barriers | 0.609 | 0.135 | 0.045 | 0.202 | 1.004 | 9869 | 3844 |
| nvrhi_d3d12_mt_chunk_t08 | 0.611 | 0.282 | 0.053 | 0.219 | 1.179 | 8429 | 3846 |
| nvrhi_vk_st_t01 | 0.677 | 0.021 | 0.032 | 0.027 | 0.767 | 13113 | 3824 |
| nvrhi_vk_st_t01__explicit_barriers | 0.685 | 0.022 | 0.032 | 0.028 | 0.777 | 12995 | 3826 |
| nvrhi_vk_mt_scene_t06 | 0.296 | 0.049 | 0.045 | 0.212 | 0.627 | 13546 | 3833 |
| nvrhi_vk_mt_chunk_t02 | 0.547 | 0.105 | 0.042 | 0.054 | 0.831 | 11208 | 3839 |
| nvrhi_vk_mt_chunk_t04 | 0.430 | 0.146 | 0.051 | 0.215 | 0.872 | 10132 | 3839 |
| nvrhi_vk_mt_chunk_t04__explicit_barriers | 0.367 | 0.105 | 0.047 | 0.334 | 0.874 | 9957 | 3847 |
| nvrhi_vk_mt_chunk_t08 | 0.441 | 0.228 | 0.058 | 0.431 | 1.181 | 7739 | 3833 |

- Single-threaded, nvrhi records the frame in 1.31 ms on D3D12 and 0.68 ms on Vulkan against 0.25 ms for
  native D3D12: about 5.3x and 2.7x the native cost per draw (~290 ns vs ~65 ns per subset draw on D3D12).
- `mt_scene` (five lists on workers, the direct view on the main thread) brings the main thread's recording to
  0.47 ms (D3D12) / 0.30 ms (Vulkan); native drops to 0.12 ms. `mt_chunk` scales less for nvrhi (0.84 -> 0.65 ->
  0.61 ms for 2 / 4 / 8 threads on D3D12): the per-pass join plus six execute calls dominate, and the native
  renderer gains nothing from chunking at all (its single-threaded recording is already shorter than the
  synchronisation).
- `-explicit_barriers` saves about 5 % of the recording time; nvrhi's per-draw resource tracking is a minor part
  of its overhead on this workload. The rest is the state diffing, binding set and vertex/index buffer rebinding
  in `setGraphicsState` and the volatile constant buffer writes.
- `frame` of the native rows (0.56 ms, 1800 fps) is presentation bound (`present` 0.2 ms); the nvrhi Vulkan
  `present` column varies with the swap chain's pacing and is not a renderer cost.

Reading it: `render` is the CPU cost of recording ~4100 draws (plus the per-frame pass setup); `frame` is bounded
by the slowest of CPU, GPU and the compositor. The GPU takes well under a millisecond for this scene, so the
single-threaded rows are CPU bound and the `mt_chunk` rows show how far each API layer scales with threads.

## Limitations

- The benchmark compares the API layers on *this* sample's binding model: root CBVs, one-descriptor tables per
  texture, a draw per subset. It is not a general nvrhi-versus-native result; the Asteroids benchmark covers a
  bindless / instanced workload.
- The native renderer is D3D12 only; `nvrhi vk` rows have no native counterpart.
- `mt_scene` always records five jobs: more than six threads add nothing to that mode.
- The frame rate of the single-threaded rows is high enough for the compositor to pace presentation when the
  window is covered or not displayed; run the matrix from an interactive session with the window visible.
- Two material textures referenced by the sdkmesh (`default.dds`, `default-normalmap.dds`) do not exist in the
  SDK media; both renderers bind a 1x1 white / flat-normal texture for them (the sample skipped the binding).
