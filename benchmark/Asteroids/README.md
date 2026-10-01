# Asteroids benchmark

CPU-side cost of nvrhi (through Donut) against hand-written D3D11 / D3D12 on the Asteroids workload
(50,000 asteroids, 1000 meshes, 10 textures, CPU-bound). Derived from the former `nvrhi-benchmark`
repository.

| Directory | Content |
|---|---|
| `common/` | The benchmark contract: command line, timers, CSV/JSON writers, scene hash (`benchmark.h`), self-test |
| `core/` | Simulation, meshes, procedural textures, camera and DDS loader of the Intel sample, shared by both renderers |
| `native/` | `asteroids_native`: the stock Intel renderers, `d3d11/` and `d3d12/`, plus the window/main loop |
| `nvrhi/` | `asteroids_nvrhi`: Donut + nvrhi renderer (D3D11, D3D12, Vulkan) and its HLSL |
| `assets/` | Skybox / sprite textures and the native renderers' HLSL (`.vsh`/`.psh`) |
| `SDK/` | `d3dx12.h` |
| `docs/` | Plan, build notes, environment, limitations, profiling |
| `scripts/`, `tests/` | Run matrix, profiling, analysis and its unit tests |
| `report/` | The published report, charts and aggregated data |

`license.txt` is the Intel code-sample license of the sample sources in `core/`, `native/` and `assets/`.

## Build and run

```powershell
..\..\..\build.ps1 -Target asteroids_native,asteroids_nvrhi,benchmark_selftest   # or scripts\build.ps1
..\..\..\build\bin\asteroids_nvrhi.exe -benchmark -renderer nvrhi -api d3d12 -binding tex_mut -threads 1 -output out
..\..\..\build\bin\asteroids_native.exe -benchmark -renderer native -api d3d12 -threads 1 -output out
```

Release builds apply the same code generation to the benchmark and to all of Donut/nvrhi (option
`ASTEROIDS_MATCH_CODEGEN`, default ON; see `CMakeLists.txt`). Turn it off if you only want the
other samples built with stock flags.

## What changed from nvrhi-benchmark

- The DiligentEngine renderers (D3D11/D3D12/Vulkan) and the `diligent-samples-asteroids.patch` are gone; the
  native D3D11/D3D12 renderers they were patched into now live in `native/` as a standalone executable
  (`asteroids_reference` became `asteroids_native`). `-renderer` is `native` or `nvrhi`.
- Donut is the repository's shared `donut/` submodule; the build is part of the ethereal CMake project
  (Ninja Multi-Config, output in `build\bin`, static MSVC runtime) instead of a Visual Studio preset.
- `docs/` and `report/` are the original write-up and measurements and are kept as they were; they still describe
  the Diligent comparison. The scripts (`analyze.py`, `run_matrix.ps1`, `profile.ps1`, `hotspots*.py`) and their
  tests only know the `native` and `nvrhi` renderers, so re-running them on an old session yields no Diligent rows.
- The raw `results/` sessions (git-ignored) stay in the `nvrhi-benchmark` working copy.
