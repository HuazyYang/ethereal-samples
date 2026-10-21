# ethereal-samples

Graphics samples, complete demos and CPU-overhead benchmarks built on
[Ethereal Donut](../ethereal-donut) and [NVRHI](../ethereal-nvrhi), with shaders compiled by ShaderTool.
This repository is built from the parent **Ethereal** project, rather than as a standalone CMake project.

| Collection | What to explore |
|---|---|
| [Demos](#demos) | Complete applications: the NVIDIA Asteroids meshlet demo and WaveWorks Ocean, Beach and Cove. |
| [Samples](#samples) | Focused examples of rasterization, ray tracing, lighting, GPU work graphs, sparse volumes and fluid simulation. |
| [Benchmarks](#benchmarks) | Shared rendering workloads for comparing NVRHI with native Direct3D and studying CPU recording overhead. |

The gallery contains actual application renders captured on Windows with an NVIDIA GeForce RTX 4050 Laptop GPU.
Click an image to open the full PNG. Images and [capture commands](doc/images/captures.json) are stored in
[`doc/images`](doc/images); these are visual examples, not benchmark scores or an image-equivalence certification.

## Demos

The Asteroids demo requires an installation of the original application for its media and third-party DLLs.
WaveWorks builds its SDK, shaders and examples from source alongside Ethereal, uses Donut's device manager and
render passes directly, and loads the source-built SDK through normal DLL linking. Its required media is included.

| | |
|---|---|
| [**NVIDIA Asteroids demo**](demos/nv_asteroids)<br>The 2018 meshlet demo: an animated asteroid field, planetary sky, shadows, atmospheric effects and post-processing. D3D12.<br>[<img src="doc/images/nv_asteroids.png" width="380" alt="NVIDIA Asteroids demo rendered output">](doc/images/nv_asteroids.png) | [**WaveWorks Ocean**](demos/waveworks)<br>Spectral FFT ocean simulation, displacement, foam and ocean shading. CPU or GPU simulation; D3D11, D3D12 and Vulkan rendering.<br>[<img src="doc/images/waveworks_ocean.png" width="380" alt="WaveWorks Ocean rendered output">](doc/images/waveworks_ocean.png) |
| [**WaveWorks Beach**](demos/waveworks)<br>A surveyed coastline with shallow-water simulation, breaking waves, terrain and beach lighting.<br>[<img src="doc/images/waveworks_beach.png" width="380" alt="WaveWorks Beach rendered output">](doc/images/waveworks_beach.png) | [**WaveWorks Cove**](demos/waveworks)<br>The Beach executable with a procedural cove: select it with <code>--beach-terrain=cove</code>.<br>[<img src="doc/images/waveworks_cove.png" width="380" alt="WaveWorks Cove rendered output">](doc/images/waveworks_cove.png) |

See the [Asteroids build guide](demos/nv_asteroids/README.md), [WaveWorks build guide](demos/waveworks/README.md)
and [WaveWorks FFT simulation wiki](demos/waveworks/waveworks-fft-simulation-wiki.md).

## Samples

### Global illumination

| | |
|---|---|
| [**DDGISample**](src/ddgi_samples)<br>RTXGI probe-based dynamic diffuse global illumination, with ray traced ambient occlusion and a path tracing mode. Cornell scene shown.<br>[<img src="doc/images/DDGISample.png" width="380" alt="DDGISample rendered output">](doc/images/DDGISample.png) | [**VXGISample**](src/vxgi_samples)<br>Voxel global illumination with diffuse and specular tracing, multibounce lighting and voxel debug views. Sponza scene shown.<br>[<img src="doc/images/VXGISample.png" width="380" alt="VXGISample rendered output">](doc/images/VXGISample.png) |

### Graphics features

These examples demonstrate individual rendering and API features. D3D11, D3D12 and Vulkan targets depend on
the enabled backends and the feature requirements of each example. Ray tracing examples require a GPU and
driver that support the requested ray tracing features; mesh shaders, VRS and work graphs have their own
hardware requirements. The specialization capture uses Vulkan; the other images below use D3D12.

| | |
|---|---|
| [**basic_triangle**](src/basic_triangle)<br>The smallest raster pipeline: a triangle generated from vertex IDs.<br>[<img src="doc/images/basic_triangle.png" width="380" alt="basic_triangle rendered output">](doc/images/basic_triangle.png) | [**vertex_buffer**](src/vertex_buffer)<br>Textured cubes using vertex/index buffers, constant buffers and resource bindings.<br>[<img src="doc/images/vertex_buffer.png" width="380" alt="vertex_buffer rendered output">](doc/images/vertex_buffer.png) |
| [**deferred_shading**](src/deferred_shading)<br>A textured cube rendered through a G-buffer and deferred lighting pass.<br>[<img src="doc/images/deferred_shading.png" width="380" alt="deferred_shading rendered output">](doc/images/deferred_shading.png) | [**bindless_rendering**](src/bindless_rendering)<br>Render Sponza with descriptor-table access to mesh and material resources.<br>[<img src="doc/images/bindless_rendering.png" width="380" alt="bindless_rendering rendered output">](doc/images/bindless_rendering.png) |
| [**async_compute**](src/async_compute)<br>Generate animated noise on a compute queue and display it on the graphics queue.<br>[<img src="doc/images/async_compute.png" width="380" alt="async_compute rendered output">](doc/images/async_compute.png) | [**threaded_rendering**](src/threaded_rendering)<br>Record scene rendering on multiple CPU threads; a cross-shaped multiview layout shows the result.<br>[<img src="doc/images/threaded_rendering.png" width="380" alt="threaded_rendering rendered output">](doc/images/threaded_rendering.png) |
| [**meshlets**](src/meshlets)<br>A minimal amplification/mesh shader pipeline that generates a colored triangle.<br>[<img src="doc/images/meshlets.png" width="380" alt="meshlets rendered output">](doc/images/meshlets.png) | [**variable_shading**](src/variable_shading)<br>Variable rate shading in a scene renderer with temporal antialiasing. Requires VRS support.<br>[<img src="doc/images/variable_shading.png" width="380" alt="variable_shading rendered output">](doc/images/variable_shading.png) |
| [**shader_specializations**](src/shader_specializations)<br>Vulkan specialization constants choose the positions and colors of four triangles.<br>[<img src="doc/images/shader_specializations.png" width="380" alt="shader_specializations rendered output">](doc/images/shader_specializations.png) | [**work_graphs_d3d12**](src/work_graphs)<br>GPU work graph dispatch for tiled lighting and material shading in a procedural animated scene. D3D12 work graph support required.<br>[<img src="doc/images/work_graphs_d3d12.png" width="380" alt="work_graphs_d3d12 rendered output">](doc/images/work_graphs_d3d12.png) |
| [**rt_triangle**](src/rt_triangle)<br>Build acceleration structures and trace primary rays against a triangle.<br>[<img src="doc/images/rt_triangle.png" width="380" alt="rt_triangle rendered output">](doc/images/rt_triangle.png) | [**rt_shadows**](src/rt_shadows)<br>Combine a rasterized G-buffer with ray traced visibility for directional lighting.<br>[<img src="doc/images/rt_shadows.png" width="380" alt="rt_shadows rendered output">](doc/images/rt_shadows.png) |
| [**rt_bindless**](src/rt_bindless)<br>Ray trace scene geometry and materials through bindless resource tables.<br>[<img src="doc/images/rt_bindless.png" width="380" alt="rt_bindless rendered output">](doc/images/rt_bindless.png) | [**rt_reflections**](src/rt_reflections)<br>Ray traced reflections using local shader bindings. This sample targets D3D12.<br>[<img src="doc/images/rt_reflections.png" width="380" alt="rt_reflections rendered output">](doc/images/rt_reflections.png) |
| [**rt_particles**](src/rt_particles)<br>Ray traced transparent particles and smoke, with controls for particle representation and rendering.<br>[<img src="doc/images/rt_particles.png" width="380" alt="rt_particles rendered output">](doc/images/rt_particles.png) | [**hello_dxr**](src/hello_dxr)<br>Animated tetrahedra and a Menger sponge with shadow rays and per-frame TLAS refitting. D3D12 and Vulkan.<br>[<img src="doc/images/hello_dxr.png" width="380" alt="hello_dxr rendered output">](doc/images/hello_dxr.png) |
| [**aftermath_sample**](src/aftermath)<br>Nsight Aftermath integration and controls for GPU crash diagnostics. The image shows normal rendering.<br>[<img src="doc/images/aftermath_sample.png" width="380" alt="aftermath_sample rendered output">](doc/images/aftermath_sample.png) |  |

[`headless`](src/headless) demonstrates compute dispatch and GPU readback without a window or framebuffer,
so it has no rendered image. Its D3D12 run produced:

```text
Expected result: 32896, computed result: 32896
Test PASSED
```

### Sparse volumes and fluids

The [GVDB collection](src/gvdb_samples/README.md) provides portable sparse-volume operations and examples
through Slang and NVRHI. CUDA and OptiX are runtime backends for the migrated applications. The SPH fluid
pool uses the same compute and application framework, with a particle renderer rather than a GVDB volume.

| | |
|---|---|
| [**render-to-file**](src/gvdb_samples/samples/render-to-file)<br>Load a sparse explosion volume, render it through the shared Slang path and write a PNG without a window.<br>[<img src="doc/images/render-to-file.png" width="380" alt="render-to-file rendered output">](doc/images/render-to-file.png) | [**render-kernel**](src/gvdb_samples/samples/render-kernel)<br>Render the same sparse volume through the shared Slang ray-tracing path.<br>[<img src="doc/images/render-kernel.png" width="380" alt="render-kernel rendered output">](doc/images/render-kernel.png) |
| [**3d-print**](src/gvdb_samples/samples/3d-print)<br>Solid voxelization of the Lucy mesh, with selectable voxel size and a cross-section inset.<br>[<img src="doc/images/3d-print.png" width="380" alt="3d-print rendered output">](doc/images/3d-print.png) | [**depth-map**](src/gvdb_samples/samples/depth-map)<br>Composite a raycast volume with polygon geometry using the rasterized depth buffer.<br>[<img src="doc/images/depth-map.png" width="380" alt="depth-map rendered output">](doc/images/depth-map.png) |
| [**resample**](src/gvdb_samples/samples/resample)<br>Resample a dense RAW volume into a sparse GVDB topology.<br>[<img src="doc/images/resample.png" width="380" alt="resample rendered output">](doc/images/resample.png) | [**spray-deposit**](src/gvdb_samples/samples/spray-deposit)<br>Use ray bundles to deposit material into voxels, followed by smoothing.<br>[<img src="doc/images/spray-deposit.png" width="380" alt="spray-deposit rendered output">](doc/images/spray-deposit.png) |
| [**point-fusion**](src/gvdb_samples/samples/point-fusion)<br>Fuse simulated depth scans into a growing sparse volume of a procedural city.<br>[<img src="doc/images/point-fusion.png" width="380" alt="point-fusion rendered output">](doc/images/point-fusion.png) | [**point-cloud**](src/gvdb_samples/samples/point-cloud)<br>Convert point data to a level set and render the resulting surface with OptiX.<br>[<img src="doc/images/point-cloud.png" width="380" alt="point-cloud rendered output">](doc/images/point-cloud.png) |
| [**fluid-surface**](src/gvdb_samples/samples/fluid-surface)<br>Simulate SPH particles in CUDA, build a GVDB surface and render it with OptiX.<br>[<img src="doc/images/fluid-surface.png" width="380" alt="fluid-surface rendered output">](doc/images/fluid-surface.png) | [**interactive-optix**](src/gvdb_samples/samples/interactive-optix)<br>Progressively path trace a scene containing both sparse volumes and polygon meshes.<br>[<img src="doc/images/interactive-optix.png" width="380" alt="interactive-optix rendered output">](doc/images/interactive-optix.png) |
| [**sph_fluid_pool**](src/gvdb_samples/sph_fluid_pool)<br>A CUDA SPH wave pool using the shared sample framework and NVRHI particle rendering. This capture uses 262,144 particles.<br>[<img src="doc/images/sph_fluid_pool.png" width="380" alt="sph_fluid_pool rendered output">](doc/images/sph_fluid_pool.png) |  |

## Benchmarks

These are separate workloads from the NVIDIA Asteroids demo above. Each benchmark shares its scene and
frame schedule between the native and NVRHI renderers and writes frame timings to CSV plus a JSON run summary.
The images show the rendered workloads; use the linked benchmark guides for timing definitions and run matrices.

| | |
|---|---|
| [**Asteroids: NVRHI**](benchmark/Asteroids)<br>A CPU-overhead workload with 50,000 asteroids, several binding strategies and threaded recording. D3D12 capture.<br>[<img src="doc/images/asteroids_nvrhi.png" width="380" alt="Asteroids: NVRHI rendered output">](doc/images/asteroids_nvrhi.png) | [**MultithreadedRendering: NVRHI**](benchmark/MultithreadedRendering)<br>SquidRoom rendered with shadows and stencil mirrors; compares single-thread, per-scene and per-chunk recording. D3D12 capture.<br>[<img src="doc/images/mt_rendering_nvrhi.png" width="380" alt="MultithreadedRendering: NVRHI rendered output">](doc/images/mt_rendering_nvrhi.png) |

The [Asteroids benchmark](benchmark/Asteroids/README.md) supports native D3D11/D3D12 and NVRHI
D3D11/D3D12/Vulkan. The [MultithreadedRendering benchmark](benchmark/MultithreadedRendering/README.md)
compares native D3D12 with NVRHI D3D12/Vulkan. Native baselines make the benchmark build Windows-only.
Both gallery images use the NVRHI D3D12 renderer. The existing native Asteroids executable failed during
initialization on this capture machine; capture notes and the SquidRoom missing-texture warnings are recorded
in the [manifest](doc/images/captures.json).

## Build and run

Run these commands from the parent `ethereal` directory. On Windows, `build.ps1` sets up the Visual Studio
C++ environment and uses the repository's CMake preset. Executables, runtime DLLs and compiled shaders land
in `build/bin`; all configurations share that output directory.

```powershell
# Core samples and both benchmark workloads
.\build.ps1 -Config Release -CMakeArgs '-DETHEREAL_BUILD_SAMPLES=ON','-DETHEREAL_BUILD_BENCHMARKS=ON'

# Rebuild selected targets after configuration
.\build.ps1 -Config Release -Target basic_triangle,DDGISample,VXGISample

# GVDB Slang/NVRHI collection (CUDA and OptiX are optional runtime backends)
.\build.ps1 -CMakeArgs '-DETHEREAL_BUILD_SAMPLES=ON','-DETHEREAL_BUILD_GVDB=ON'

# WaveWorks: ISPC must be on PATH
.\build.ps1 -Target waveworks_ocean,waveworks_beach -CMakeArgs '-DETHEREAL_BUILD_WAVEWORKS=ON'

# NVIDIA Asteroids demo: supply the original installation
.\build.ps1 -Target nv_asteroids -CMakeArgs '-DETHEREAL_BUILD_NV_ASTEROIDS=ON','-DNV_ASTEROIDS_ORIGINAL_DIR=<original installation>'

# Optional Nsight Aftermath sample
.\build.ps1 -Target aftermath_sample -CMakeArgs '-DETHEREAL_BUILD_SAMPLES=ON','-DDONUT_WITH_AFTERMATH=ON'
```

The portable GVDB targets do not require NVCC or CUDA Toolkit discovery. CUDA and OptiX execution requires
the corresponding runtime backend and driver; OptiX samples also need the OptiX 9 SDK, discoverable through
`OptiX_INSTALL_DIR` or the supported default installation path. Without that SDK the OptiX targets are skipped.
WaveWorks currently targets Windows x64 and needs
ISPC plus the D3D and Vulkan shader toolchains described in its build guide.

```powershell
build\bin\basic_triangle.exe -d3d12
build\bin\shader_specializations.exe -vk
build\bin\DDGISample.exe --config ethereal-samples\assets\config\cornell.cfg.json
build\bin\VXGISample.exe
build\bin\waveworks_ocean.exe --rhi=d3d12 --simulation-dev=gpu
build\bin\waveworks_beach.exe --rhi=vk --beach-terrain=cove
build\bin\nv_asteroids.exe -nodialog -adapter NVIDIA -view 0
build\bin\asteroids_nvrhi.exe -benchmark -renderer nvrhi -api d3d12 -binding tex_mut -threads 4 -output out\asteroids
build\bin\mt_rendering_nvrhi.exe -benchmark -renderer nvrhi -api d3d12 -mode mt_chunk -threads 4 -output out\squidroom
```

Most Donut examples select a backend with `-d3d11`, `-d3d12` or `-vk`. WaveWorks uses `--rhi`, and the
benchmarks use `-api`. Run the native Asteroids benchmark with `build/bin` as its working directory so its
relative `media/` paths resolve. Beach pre-simulates shallow water before its first frame, so startup can take
roughly 30 seconds.

## Source and asset layout

| Path | Contents |
|---|---|
| `src/ddgi_samples`, `src/vxgi_samples` | Global illumination sample sources and shaders. |
| `src/gvdb_samples` | GVDB library, shared sample framework, CUDA/OptiX examples and SPH fluid pool. |
| `src/<example>` | Donut feature examples and the HelloDXR port. |
| `demos/nv_asteroids`, `demos/waveworks` | Complete demo applications and their build guides. |
| `benchmark/Asteroids`, `benchmark/MultithreadedRendering` | Native and NVRHI renderers, shared workloads, measurement scripts and reports. |
| `assets` | DDGI/VXGI scene descriptions, models and textures. |
| `media` | Shared Donut media, including Sponza, particle textures and SquidRoom. |
| `doc/images` | Gallery PNGs and capture provenance. |

The build links `media` to `build/media` for the Donut examples. DDGI resolves scenes relative to its configuration;
VXGI uses the configured `assets/vxgi` path. GVDB assets stay under `src/gvdb_samples/samples/assets`, and WaveWorks
media stays with the demo. Keep these source asset directories available when running the executables.

ShaderTool builds shaders through the parent project's `ethereal_compile_shaders()` helper. Shader outputs
are organized under `build/bin/shaders/<target>/<dxbc|dxil|spirv>`; CUDA/OptiX modules are under `build/bin/ptx`.
The D3D12 sample build also deploys the Agility SDK under `build/bin/D3D12` for the features that need it.

The imported examples use this fork's reference-counted interfaces, out-parameter resource factories and
ShaderTool permutation blobs. See the individual source directories and [architecture notes](doc/adr/README.md)
for implementation details.
