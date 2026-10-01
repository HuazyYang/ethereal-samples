# Build

> **Moved into ethereal.** This benchmark is now `EtherealSamples/benchmark/Asteroids`, built by the
> ethereal root CMake project (`build.ps1`, Ninja Multi-Config, output `build\bin`, static MSVC runtime).
> The DiligentEngine reference and its Visual Studio preset were removed; `asteroids_reference` is now
> `asteroids_native`. The rest of this page is the original text: the "Release code generation",
> "Vulkan" and "Shader compilation" sections still describe what the CMake files do for the nvrhi
> renderer, but the Diligent columns, `windows-release` paths and Diligent configure-time steps no longer apply.

## Commands

```powershell
# configure + build the default targets (Asteroids, benchmark_selftest, asteroids_nvrhi once src/nvrhi/main.cpp exists)
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build.ps1
# configure only / selected targets
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build.ps1 -ConfigureOnly
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build.ps1 -Target Asteroids,benchmark_selftest
# tests (header self-test, analysis unit tests once tests/analysis exists)
ctest --preset windows-release
```

`build.ps1` puts the Windows SDK `fxc.exe` on `PATH` (the reference sample compiles its native shaders
with it) and then runs `cmake --preset windows-release` and `cmake --build --preset windows-release --target ...`.
A full build takes well over the two-minute foreground shell limit: run it in the background and
redirect the output from outside the script (`... -File scripts\build.ps1 *> build\build.log`).

| Item | Value |
|---|---|
| Generator | Visual Studio 17 2022, x64, single configuration `Release` |
| Build tree | `build/windows-release` |
| Output directory | `build/windows-release/bin` (all executables, DLLs, assets, compiled shaders) |
| Reference executable | `build/windows-release/bin/asteroids_reference.exe` (CMake target `Asteroids`) |
| nvrhi executable | `build/windows-release/bin/asteroids_nvrhi.exe` (CMake target `asteroids_nvrhi`) |
| Header self-test / LUID tool | `build/windows-release/bin/benchmark_selftest.exe` |

## Release code generation (the same for both executables)

Both executables and every library they link statically are compiled with the same Release options.
DiligentCore sets them for its own targets and for the reference sample (`DiligentCore/CMakeLists.txt`:
`MSVC_RELEASE_COMPILE_OPTIONS`, `DILIGENT_MSVC_RELEASE_COMPILE_OPTIONS`; `BuildTools/CMake/BuildUtils.cmake`:
`set_common_target_properties`). Donut and nvrhi set none of them, so the root `CMakeLists.txt` applies
them from outside (`benchmark_match_reference_codegen`, no Donut/nvrhi source or CMake edits) to every
Donut/nvrhi target (`donut_core`, `donut_engine`, `donut_render`, `donut_app`, `nvrhi`, `nvrhi_d3d11`,
`nvrhi_d3d12`, `nvrhi_vk`, `glfw`, `imgui`, `jsoncpp_static`) and to `asteroids_nvrhi`.

Effective flags, read from the generated `.vcxproj` files (`build/windows-release/reference/Asteroids.vcxproj`,
`src/nvrhi/asteroids_nvrhi.vcxproj`, `Donut/nvrhi/nvrhi*.vcxproj`, `Donut/donut_*.vcxproj`,
`DiligentEngine/DiligentCore/Graphics/GraphicsEngineD3D12/Diligent-GraphicsEngineD3D12-static.vcxproj`):

| Option | `asteroids_reference` + Diligent libraries | `asteroids_nvrhi` + Donut + nvrhi libraries |
|---|---|---|
| Optimization | `/O2 /Ob2` | `/O2 /Ob2` |
| Favor speed, intrinsics | `/Ot /Oi` | `/Ot /Oi` |
| Instruction set | `/arch:AVX2` | `/arch:AVX2` |
| Whole program optimization | `/GL`, link `/LTCG`, lib `/LTCG` | `/GL`, link `/LTCG`, lib `/LTCG` |
| String pooling | `/GF` | `/GF` |
| RTTI | off (`/GR-`) | **on** (compiler default) |
| Debug information | `/Zi`, link `/DEBUG:FULL /OPT:REF /OPT:ICF /INCREMENTAL:NO` | the same |

RTTI is the one remaining difference: `/GR-` was tried and rejected because nvrhi's validation layer
(`Donut/nvrhi/src/validation/validation-device.cpp`, `validation-commandlist.cpp`) and Donut's scene
graph (`Donut/src/engine/SceneGraph.cpp`, `GltfImporter.cpp`, `AudioEngine.cpp`) use `dynamic_cast`
(warning C4541 with `/GR-`). Neither is on a benchmark code path, but the libraries need RTTI to be
correct, and `asteroids_nvrhi` shares their polymorphic types, so all three keep it. RTTI adds type
descriptors to the binary; it does not change the code of the draw loop.

`/Zi` and `/DEBUG:FULL /OPT:REF /OPT:ICF /INCREMENTAL:NO` give the profiling phase symbols without
changing code generation.

Check after the change (RTX 4050, 8 s runs, `update_ms` is the same `simulation.cpp` in both
executables): 1 thread 1.70 ms (Diligent D3D12) / 1.66 ms (nvrhi D3D12) / 1.68 ms (native D3D12);
8 threads 0.320 / 0.323 ms (Diligent / nvrhi). Before the change the two executables differed by about 10 %.

Known harmless build output: `'pwsh.exe' is not recognized` after linking the Diligent DLLs (an
optional Diligent post-build step; PowerShell 7 is not installed). ShaderTool and Diligent both copy a
`d3dcompiler_47.dll` into `bin`; whichever target builds last wins.

The unmodified reference sample returns exit code 1 when it leaves through `-close_after` (it breaks
out of the main loop into `return 1`); that is its normal path, not a crash.

## Configure-time side effects

The Diligent and Donut CMake scripts reach out to the network and to the Python installation:

- nvrhi fetches `Vulkan-Headers` (v1.4.352) and `DirectX-Headers` into `build/windows-release/_deps`.
- DiligentCore downloads NVAPI into `_deps/nvapi-src`.
- DiligentTools/RenderStateNotation runs `python -m pip install libclang==16.0.6 jinja2` with the
  Python that `find_package(Python3)` selects (Python 3.13 on this machine).
- ShaderTool unpacks its vendored DXC, d3dcompiler and slang packages into `_deps`.

## Vulkan

| Library | What it needs | Where it comes from |
|---|---|---|
| nvrhi (`NVRHI_WITH_VULKAN`, set through `DONUT_WITH_VULKAN=ON`) | `Vulkan::Headers` CMake target | fetched by nvrhi (`NVRHI_FETCH_VULKAN_HEADERS`, tag v1.4.352) |
| Donut `donut_app` | GLFW Vulkan surface, `vulkan-1.dll` at run time | `Donut/thirdparty/glfw`; loader installed by the GPU driver |
| DiligentCore (`DILIGENT_NO_VULKAN=OFF` -> `VULKAN_SUPPORTED`) | `Vulkan::Headers`, volk, SPIRV-Tools, SPIRV-Cross, glslang | headers: the target created by nvrhi (Donut is added first); the rest: `DiligentCore/ThirdParty` submodules |

No Vulkan SDK is required to build. DiligentCore additionally looks for the SDK
(`find_package(Vulkan)`) only to copy the SDK's `dxcompiler.dll` as `spv_dxcompiler.dll` next to the
executable; the reference sample does not select DXC, its HLSL goes through glslang.

## Shader compilation per API

| Renderer | D3D11 | D3D12 | Vulkan |
|---|---|---|---|
| native | `fxc` at build time (`*_5_0`), embedded headers | `fxc` at build time (`asteroid_ps` as `ps_5_1`), embedded headers | n/a |
| diligent | run time, HLSL -> DXBC with d3dcompiler | run time, HLSL -> DXBC with d3dcompiler | run time, HLSL -> SPIR-V with built-in glslang |
| nvrhi | build time, ShaderTool `dxbc` (d3dcompiler, `*_5_0`) | build time, ShaderTool `dxil` (DXC, `*_6_5`) | build time, ShaderTool `spirv` (DXC `-spirv`) |

### Recipe for `asteroids_nvrhi`

Donut replaced ShaderMake with `Donut/thirdparty/shadertool`. The CMake function
`shadertool_add_shader_objects` (from `Donut/thirdparty/shadertool/cmake/ShaderToolFunctions.cmake`,
already included by `Donut/shaders/CMakeLists.txt`, so it is available after `add_subdirectory(Donut)`)
turns a ShaderMake-style config file into one custom command per backend.

`src/nvrhi/shaders.cfg` (paths relative to the config file, `-T` is the stage without a shader model):

```
asteroid_vs.hlsl -T vs -E main
asteroid_ps.hlsl -T ps -E main
```

`src/nvrhi/CMakeLists.txt`:

```cmake
shadertool_add_shader_objects(
    TARGET asteroids_nvrhi_shaders
    CONFIG_FILE ${CMAKE_CURRENT_SOURCE_DIR}/shaders.cfg
    BACKENDS DXBC DXIL SPIRV
    OUTPUT_MODE BINARY_BLOB
    OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/asteroids_nvrhi
    SHADER_MODEL 6_5
    DEFINES_DXBC TARGET_D3D11
    DEFINES_DXIL TARGET_D3D12
    DEFINES_SPIRV SPIRV TARGET_VULKAN
    SOURCES asteroid_vs.hlsl asteroid_ps.hlsl)
benchmark_disable_vs_hlsl_rule(asteroids_nvrhi_shaders)   # defined in the root CMakeLists.txt, required
add_dependencies(asteroids_nvrhi asteroids_nvrhi_shaders)
```

`benchmark_disable_vs_hlsl_rule` is mandatory with the Visual Studio generator: the function lists the
`.hlsl` files as target sources and Visual Studio would otherwise compile them with its built-in FXC
rule and fail (`error X4502: invalid vs_2_0 output semantic`). The root CMakeLists.txt applies it to
`donut_shaders`. Do not add `.hlsl` files to the `asteroids_nvrhi` executable's own source list.

Verified in the setup phase: `scripts\build.ps1 -Target donut_libs` builds nvrhi (D3D11, D3D12,
Vulkan, validation), Donut and all 45 Donut shaders for each of DXBC, DXIL and SPIR-V.

Results: `bin/shaders/asteroids_nvrhi/{dxbc,dxil,spirv}/<name>.bin` (entry points other than `main`
append `_<entry>` to the name). Donut's own shaders are built by the `donut_shaders` target (a
dependency of `donut_app`) into `bin/shaders/framework/{dxbc,dxil,spirv}`.

Loading at run time (the pattern of every Donut application):

```cpp
auto root = donut::app::GetDirectoryWithExecutable();
donut::vfs::RootFileSystem* fs = ...; // this fork's ShaderFactory takes a raw vfs::IFileSystem*; see Donut/include/donut/core/vfs/VFS.h for ownership
fs->mount("/shaders/donut", root / "shaders/framework" / donut::app::GetShaderTypeName(device->getGraphicsAPI()));
fs->mount("/shaders/app",   root / "shaders/asteroids_nvrhi" / donut::app::GetShaderTypeName(device->getGraphicsAPI()));
donut::engine::ShaderFactory factory(device, fs, "/shaders");   // ShaderFactory(nvrhi::IDevice*, vfs::IFileSystem*, const path& basePath)
auto vs = factory.CreateShader("app/asteroid_vs.hlsl", "main", nullptr, nvrhi::ShaderType::Vertex);
```

Facts that matter for the HLSL:

- DXBC is always compiled as shader model 5_0; DXIL and SPIR-V use `SHADER_MODEL` (6_5).
  Unbounded descriptor arrays (bindless) therefore only exist in the DXIL and SPIR-V variants; guard
  them with `#if !defined(TARGET_D3D11)`.
- SPIR-V register shifts default to `t+0, s+128, b+256, u+384` for register spaces 0..7
  (`SPIRV_REGISTER_SHIFTS`). These are the offsets nvrhi's Vulkan backend expects by default
  (`nvrhi::VulkanBindingOffsets`), so the same `register(tN/sN/bN)` declarations work on all three APIs.
- Push constants: declare the constant buffer with `[[vk::push_constant]]` for SPIR-V (Donut's
  `VK_PUSH_CONSTANT` / `DECLARE_PUSH_CONSTANTS` macros in `donut/shaders/binding_helpers.hlsli`, which
  key on the `SPIRV` define) and as a normal `cbuffer` for D3D. Add
  `INCLUDE_DIRECTORIES ${DONUT_SHADER_INCLUDE_DIR}` to the CMake call to include Donut's `.hlsli` files.
- Per-row `-D NAME={0,1}` creates keyed permutations (loaded by passing `ShaderMacro`s to
  `CreateShader`); `DEFINES*` of the CMake call are global and do not create permutations.

The tool can also be run by hand, e.g.
`bin\ShaderTool.exe spirv -T ps_6_5 -E main -fvk-t-shift 0 0 -fvk-s-shift 128 0 -fvk-b-shift 256 0 -fvk-u-shift 384 0 -fspv-target-env=vulkan1.2 -Fo out.bin shader.hlsl`
(`dxbc` with `-T ps_5_0`, `dxil` with `-T ps_6_5`).

## Analysis environment

`scripts/analyze.py` and the unit tests need only the standard library; the charts need matplotlib.
A project virtual environment (git-ignored) holds the packages of `requirements.txt`:

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
.\.venv\Scripts\python.exe -m unittest discover -s tests/analysis
.\.venv\Scripts\python.exe scripts\analyze.py results\<session> --out report
```
