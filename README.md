# ethereal-samples

Samples and benchmarks built on the [Donut](../donut) graphics framework (nvrhi, ShaderTool).
This repository is a sub-project of the `ethereal` aggregate and is configured by its root
`CMakeLists.txt`; it is not a standalone CMake project.

| Path | Content |
|---|---|
| `src/DDGISample`, `src/VXGISample` | Dynamic diffuse GI (RTXGI DDGI) and voxel GI (VXGI) samples |
| `src/GVDBSamples` | GVDB sparse-voxel library (COM-style interfaces, Donut scene graph) and the ported GVDB samples, CUDA + optional OptiX 9 (`-DETHEREAL_BUILD_GVDB=ON`, `-DETHEREAL_WITH_OPTIX=ON`; see `src/GVDBSamples/README.md`) |
| `src/<example>` | The Donut examples from `Donut-Samples/examples` (`basic_triangle`, `rt_shadows`, `meshlets`, ...) |
| `assets/` | Scenes, configs and textures of the DDGI / VXGI samples |
| `benchmark/Asteroids` | nvrhi vs. native D3D11/D3D12 CPU-overhead benchmark, see its README |

## Build

From the `ethereal` root (`build.ps1` sets up the Visual Studio environment):

```powershell
.\build.ps1                                  # Release, everything
.\build.ps1 -Config Debug -Target VXGISample
```

## Run

Run from anywhere; the executables find their shaders next to themselves (`build\bin`).

```powershell
build\bin\DDGISample.exe --config ethereal-samples\assets\config\cornell.cfg.json   # also: sponza, tunnel, furnace, ...
build\bin\VXGISample.exe                                                          # Sponza; --scene <file under assets/vxgi>
```

Add `--debug` to either to turn on the D3D12 debug layer and nvrhi's validation layer (errors are shown in a
message box and sent to the debugger output).

- VXGISample finds its scenes through the `ETHEREAL_ASSETS_DIR` compile definition (this repository's `assets/`).
- The ray tracing samples (DDGI and `rt_*`) load the Agility SDK D3D12 runtime from `build\bin\D3D12` through
  `ethereal_use_agility_sdk()` in `src/CMakeLists.txt`: nvrhi marks acceleration-structure buffers with a resource
  flag that the Windows 10 system runtime rejects. The default is a stable release (`ETHEREAL_AGILITY_SDK_VERSION`,
  1.619.6); a `-preview` release only loads with Windows Developer Mode on.
- GVDBSamples needs the CUDA toolkit (PTX for `sm_75`); the OptiX renderer and the samples that use it
  (`point-cloud`, `fluid-surface`, `interactive-optix`) need the OptiX 9 SDK (`OptiX_INSTALL_DIR` or the default
  install location; `-DETHEREAL_WITH_OPTIX=OFF` drops them). PTX modules land in `buildin\ptx`, assets are
  read in place from `src/GVDBSamples/samples/assets`.

Executables and shaders land in `build\bin`. Shaders are compiled by ShaderTool through
`ethereal_compile_shaders()` (`cmake/EtherealShaders.cmake`), which replaces Donut's removed
`donut_compile_shaders()`.

## About the Donut examples

They were copied from `Donut-Samples/examples`, which is written against upstream Donut. This repository's
Donut is the `ethereal-dev` fork, whose object model and shader build differ, so the examples were ported:

- `CMakeLists.txt` call `ethereal_compile_shaders()` instead of the removed ShaderMake based `donut_compile_shaders*()`.
- Donut objects are reference counted: `std::shared_ptr`/`make_shared` became `AutoPtr`/`MAKE_RC_OBJ_PTR`
  (`TakeOver` for the device manager, no `delete`), `LoadScene` takes a `vfs::IFileSystem*`, `ImGui_Renderer`
  is `ImGuiRenderPass` (`BuildUI`), and `RenderCompositeView` takes the framebuffer factory by pointer.
- `rt_bindless`: the compute entry point is `main_cs`, because ShaderTool names its output by source and entry point.
- All of them compile and link in Release, but none of the ported examples has been run on a GPU yet. `aftermath` needs
  `-DDONUT_WITH_AFTERMATH=ON` and was ported but not compiled.

## Shader permutations

ShaderTool builds one permutation blob per source file and entry point; several permutations come from brace sets
in a single `shaders.cfg` row (`-DNAME={a,b}`), and a lookup needs the exact set of macro names and values.
The VXGI and DDGI configs were rewritten that way (some unused combinations are built as a side effect).
