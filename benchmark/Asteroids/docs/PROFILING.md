# Profiling: CPU sampling and hot-spot attribution

How the hot-spot tables in `report/data/hotspots.md` / `report/data/hotspots_tables.md` were produced, and how to
reproduce them. Raw captures live under `results/profiles-01/` (git-ignored).

## Tools that were available, and what was used

| Tool | Status on the measurement machine | Used |
|---|---|---|
| `wpr` (Windows Performance Recorder) | present, but needs an elevated shell; the session was not elevated and nothing was elevated for this phase | no |
| `VSDiagnostics.exe` (Visual Studio 2022 Community, `Team Tools\DiagnosticsHub\Collector`) | works without elevation: its standard-collector service records a kernel ETW session with sampled profiles and stacks for the whole machine | **capture** |
| `xperf` (Windows Performance Toolkit, Windows Kits 10) | present; processes the ETL inside the `.diagsession` without elevation, resolves symbols | **symbolisation / dump** |
| `dbghelp.dll` + `symsrv.dll` (Windows Kits 10 `Debuggers\x64`) | present (the debugger executables themselves are not installed) | **source lines and inline chains** (through `ctypes`) |
| Visual Studio GUI | not used: everything is scripted, no interactive sessions |

A `.diagsession` written by VSDiagnostics is a zip-like package (`VSDiagnostics.exe expandDiagSession <file>`
expands it next to the file). With the `CpuUsageHigh.json` agent configuration it contains one kernel ETL file
(`sc.user_aux.etl`, about 33 MB for 20 s) with `SampledProfile` events at **4 kHz per logical processor** and the
`Stack` events (kernel and user fragments, with the image rundown that symbol decoding needs), plus a `.counters` file
that is irrelevant here. There is no SQLite in it and no CLI export; `xperf -i <etl> -symbols -a dumper` is the export.

## Pipeline

```
scripts\hotspots_capture.ps1 -Session profiles-01          # all 15 configurations, ~2 min each
scripts\hotspots_capture.ps1 -Session profiles-01 -Only <name>[,<name>] [-SkipCapture]
python scripts\hotspots.py results\profiles-01 --out report\data
```

Per configuration `hotspots_capture.ps1` runs:

1. `scripts\profile.ps1 -Tool vsdiag -Seconds 20` (5 s warmup unprofiled, then VSDiagnostics attaches for 20 s; the
   benchmark process runs with `-duration 48` so that the capture lies entirely inside the measured phase; the
   profiled run's `run.json`/`frames.csv` are written to `<name>_run/` and are **not measurements**: at 4 kHz the
   sampling interrupts cost 5-20 % of render + submit, see the `per_frame_ms` column of the tables against
   `report/data/summary.csv`).
   nvrhi D3D12 configurations that call `setGraphicsState` per draw are captured with `-cb_page_split_avoid` or
   `-cb_page_split_force` (`docs/LIMITATIONS.md` 5.1) and the outputs renamed to `<config>__<switch>`.
2. `VSDiagnostics.exe expandDiagSession` and `xperf -i <etl> -tle -tti -symbols -o <dump> -a dumper`
   with `_NT_SYMBOL_PATH=<repo>\build\windows-release\bin;srv*<repo>\results\<session>\symbols` and
   `_NT_SYMCACHE_PATH=<repo>\results\<session>\symcache`. The local store is deliberately **without** a symbol
   server: the trace contains every process on the machine and xperf would otherwise download the PDB of every image
   it sees (it tried a 676 MB `msedge.dll.pdb`). Run once with `-FetchSymbols` (adds
   `*https://msdl.microsoft.com/download/symbols`) to fill the store; the Microsoft PDBs needed here are
   `ntdll`, `kernelbase`, `kernel32`, `ntkrnlmp`, `d3d12`, `d3d12core`, `dxgi`, `dxgkrnl`, `dxgmms2`, `win32u`,
   `ucrtbase`, `vcruntime140`, `msvcp_win`. The NVIDIA user-mode drivers (`nvwgf2umx.dll`, `nvoglv64.dll`) and the
   Vulkan loader have no public symbols: their frames stay `module!0xaddress` and are reported at module level.
   Also `xperf -a process -image` is written to `<name>.images.txt` (image base addresses, needed for dbghelp).
   Do not use `-add_inline`: it appends the list of *every* function inlined anywhere into the frame's function,
   not the inline chain at the sampled address.
3. `scripts\hotspots_collapse.py <dump> <process.exe> <name>.stacks.txt`: keeps the `SampledProfile` events of the
   benchmark process and the `Stack` fragments that follow each (kernel fragment first, then the user fragment, each
   numbered from 1; they are concatenated leaf to root), and writes one line per distinct stack with its count and
   thread. Frames of the application images (`asteroids_*.exe`, `GraphicsEngine*_64r.dll`) keep their address
   (`module!function@0x...`). xperf writes `,` inside symbol names as `;` in `Stack` lines; the script maps them back.
   The 700 MB text dump is deleted afterwards (`-KeepDump` keeps it).

`scripts\hotspots.py` then, per configuration:

- loads the PDBs of the application images at the trace's load addresses with dbghelp
  (`SymLoadModuleExW`, `SymGetLineFromAddrW64`, `SymAddrIncludeInlineTrace` / `SymQueryInlineTrace` /
  `SymFromInlineContextW` / `SymGetLineFromInlineContextW`);
- selects the main thread (start function `mainCRTStartup`) and, for multi-threaded runs, the worker thread with the
  most samples in the renderer's worker function;
- classifies every main-thread sample into the benchmark's columns by the **source line of the renderer's `Render`
  frame** (the return address into `AsteroidsD3D12::Asteroids::Render` / `AsteroidsDE::Asteroids::Render`, mapped
  against the `lap.To(...)` boundaries in the sources; for nvrhi the whole `AsteroidsRenderer::Render` is
  render + submit because update is in `Animate` and Present in Donut's `DeviceManager`). Check: at 1 thread
  `render + submit samples x 0.25 ms / frames` agrees with the run's `median_cpu_render_ms` to within 2 %
  (effective 0.245-0.257 ms per sample);
- groups samples by leaf module, by leaf category, and **charged**: the first frame from the leaf upward that belongs
  to the application, the abstraction layer (`nvrhi::*`, `Diligent::*`), the API runtime (`d3d12.dll`,
  `D3D12Core.dll`, `dxgi.dll`, `vulkan-1.dll`) or the driver (`nv*.dll`, `nvlddmkm.sys`). OS (kernel, `ntdll`,
  `kernelbase`) and CRT (`vcruntime140` memcpy/memset, `std::` containers linked into the image, stack cookies) time is
  thereby charged to the code that called it;
- partitions the render + submit samples by **entry point**: the outermost layer frame below `Render`, else the
  outermost runtime frame (native), with the split of each entry point into layer-own / runtime / driver samples;
- lists top functions (exclusive and inclusive; inclusive counts a function once per stack), the layer's functions
  by inclusive samples, the runtime entry points, and the **hot source lines** (sampled instruction of the
  application and layer images with its inline chain; CRT leaves are attributed to the calling line);
- converts to ns per draw: at 1 thread `share of render + submit samples x median_cpu_render_ms of the profiled run
  x 1e6 / draw_count` (50,001). At 8 threads the main thread yields (`SwitchToThread`) while it waits for the workers
  and is not sampled then, so samples are converted at the nominal 0.25 ms and the remainder of `median_cpu_render_ms`
  is reported as off-CPU; the worker's samples in its recording function are converted the same way and divided by
  its 6,250 draws.

## Reading the numbers

- Everything is **main-thread (or one worker's) CPU time attributed by sampled stacks**; a sample lands on the
  instruction that was executing when the 4 kHz interrupt fired. A stall is attributed to the instruction that waits
  (for a store-buffer stall that is the store that cannot allocate an entry, not the store that is slow).
- `/GL` + `/LTCG` inline aggressively: a frame is the outermost non-inlined function (`setGraphicsBindings` and
  `writeBuffer` survive as frames; `updateGraphicsVolatileBuffers`, `bindBindingSets`, `commitBarriers` do too), the
  hot-line table shows the inline chain at the sampled address.
- `/OPT:ICF` folds identical functions: a trivial virtual getter is reported under whichever identical function the
  linker kept (`nvrhi::d3d12::Sampler::getDesc` and `nvrhi::d3d11::Sampler::getDesc` in the tables are the
  `IBindingSet::getDesc` calls of `setGraphicsBindings` / `bindBindingSets`; Diligent's
  `EngineFactoryBase::GetReferenceCounters` under `MapBuffer` is another folded getter).
- Driver and runtime samples are at module granularity for NVIDIA code; the D3D12 runtime (`D3D12Core.dll`) is
  symbolised, so "driver below `SetGraphicsRootConstantBufferView`" is visible through the runtime entry point.
- The system-wide trace also contains the driver's and the compositor's threads; only the benchmark process's threads
  are analysed. The 8-thread runs have 8 recording threads on 8 cores / 16 logical processors: per-draw costs on a
  worker are roughly double the 1-thread values for every renderer (SMT sharing, lower all-core clocks), so compare
  workers with workers.

## Known limits of this procedure

- No elevation: `wpr`/WPA were not used; VSDiagnostics' CPU agent is the same kernel sampled-profile provider, so the
  data is equivalent, but there are no context-switch events (off-CPU time is inferred as the remainder).
- 4 kHz sampling perturbs the profiled run (its `render + submit` is 5-20 % above the measured value); shares are what
  the tables are about, the ns/draw columns inherit the perturbed per-frame time.
- One capture per configuration (two populations for nvrhi D3D12 `tex_mut`); spreads between repetitions are those of
  `docs/LIMITATIONS.md` 5, not re-measured here.
