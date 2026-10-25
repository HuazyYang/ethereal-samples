# gvdb_samples

Port of [NVIDIA GVDB Voxels 1.1.1](https://github.com/NVIDIA/gvdb-voxels) (reference checkout:
`D:\ps\repo\gvdb-voxels\source`) to the Donut / nvrhi stack of this repository. The GL, Jetson and
NanoVDB samples are not ported; OpenGL is replaced by nvrhi (D3D12 / Vulkan through Donut's
`DeviceManager`), CUDA C++ and OptiX 6 by Slang kernels and OptiX programs on the CUDA backend of nvrhi
(ethereal-nvrhi ADR 0011 - 0013; samples ADR 0002).

```
gvdb/                      the library
  GVDBCompute.h/.cpp       the compute layer: IComputeContext (the nvrhi CUDA device, the command list
                           GVDB work is recorded into, kernels of shader containers) and IComputeKernel
  GVDB.h                   the data structure and its operations as interfaces (see below)
  GVDBScene.h              GVDB volumes in Donut's scene graph
  GVDBPrimitives.h/.cpp    device-wide primitives (prefix sum, radix sort, min / max, block copy; internal)
  shaders/GVDB.slangh      VDBInfo, node traversal, atlas access, shared by every kernel
  shaders/*.slang          library kernels: GVDBOperators (apron, fill, smooth, resample ...),
                           GVDBParticles (points, GPU topology, voxelization), GVDBPrimitives
samples/
  sample-utils/            renderers (CUDA raycaster, OptiX), the app framework, OBJ meshes
    kernels/               GVDBRaycast.slang(h) (raycaster), optix/OptixPrograms.slang
  <sample>/                one directory per sample (kebab-case)
  assets/                  the shared assets of the reference (lucy.obj, explosion.vbx, ...)
sph_fluid_pool/            port of the SPH fluid simulator fluids5.0 (see "SPH fluid pool" below)
```

## Interface design

The data structure and the algorithms that work on it are separate COM objects
(reference-counted nvrhi objects with `NVRHI_IID` interfaces and factory functions). All of them are
declared in `gvdb/GVDB.h`:

| Interface | Role | Reference counterpart |
|---|---|---|
| `IGVDBVolume` | the hierarchy: level configuration, node pools (host arrays + GPU mirrors), atlas map, channel textures, the `VDBInfo` block the kernels read. Host-side topology editing (`activateSpace`, `activateRegion`, ...) is part of it because it is structure maintenance. | `VolumeGVDB` + `Allocator` (data parts) |
| `IGVDBVoxelOps` | operators on channel data: fill / clear, `compute(ComputeOp)`, custom kernels, apron update, resample / downsample, reduction | `Compute`, `FillChannel`, `UpdateApron`, `Resample`, ... |
| `IGVDBPointOps` | point clouds: GPU topology rebuild (`rebuildTopology`, `accumulateTopology`), sub-cell insertion, gather density / level set, scatter, format conversion | `RebuildTopology`, `InsertPointsSubcell`, `Gather*`, `ScatterDensity`, ... |
| `IGVDBVoxelizer` | triangle mesh to topology and voxels | `SolidVoxelize` |
| `IGVDBSerializer` | VBX file I/O | `LoadVBX`, `SaveVBX` |

A volume is created on a compute context (`createGVDBVolume(context, &volume)`). Operation objects are
created for one volume (`createGVDBVoxelOps(volume, &ops)`, ...) and use only the public
`IGVDBVolume` interface, so operations can be added without touching the data structure. They share
the library kernels through `IGVDBVolume::getKernel(name)`.

### Scene graph

A scene may hold any number of volumes. `gvdb/GVDBScene.h` follows Donut's mesh design:

- `IGVDBVolume` is shared data, like `MeshInfo`.
- `GVDBVolumeInstance` is the `SceneGraphLeaf` that places a volume in the world, like
  `MeshInstance`. Its node transform maps the volume's index space (voxel units) to world space;
  its `VolumeRenderAttributes` hold what the reference kept in the global `Scene` (channel, shading,
  iso value / value range, steps, extinction, cutoff, transfer function, OptiX material).
- `GVDBSceneGraph` is the `SceneGraph` that tracks `GetVolumeInstances()` and the distinct
  `GetVolumes()` next to mesh instances, lights and cameras. Volume leaves report the content flag
  `SceneContentFlags_Volumes` (0x100, outside Donut's bits).

The samples build their scene as a `GVDBSceneGraph`: volume instances, OBJ models as Donut
`MeshInfo` / `MeshInstance` (`sample-utils/ObjMesh.h`), a `PerspectiveCamera` leaf and a `Light`
leaf. The renderers consume the graph:

- `SampleUtils::VolRenderer` (CUDA): renders each volume instance with the GVDB raycast kernels
  (`sample-utils/kernels/GVDBRaycast.slang`), compositing instances in order; also custom render
  kernels and `raytrace()` of ray bundles.
- `SampleUtils::OptixRenderer` (OptiX 9): one custom AABB primitive per volume instance, one
  triangle BLAS per mesh, all instanced in one TLAS so the node transforms apply directly.

### Compute layer and kernels

Everything runs on the CUDA device of nvrhi (`nvrhi::cuda::IDevice`, created by the graphics
factory with the graphics device as `DeviceDesc::graphicsDevice`, so both are on the same adapter).
`gvdb::IComputeContext` owns one open command list on it: GVDB records its work there, `flush()`
submits, `finish()` waits, `readBuffer()` reads back. A volume, its operation objects and the
renderers that draw it share one context, so their work is ordered on one CUDA stream. Kernels reach
buffers through device addresses and textures through descriptor handles, which command lists do not
track: `releaseAfterUse()` keeps a replaced resource alive until the recorded work is done.

Kernels are Slang, compiled offline by ShaderTool into NVRHI shader containers (PTX plus reflection;
`ethereal_compile_shader_containers`, `TARGETS ptx` / `optix`) in `bin/shaders/gvdb`, which the
samples mount as `shaders/gvdb`. A kernel's arguments are its entry point's uniform parameters:
`gvdb::launchKernel(commandList, kernel, groups, args...)` places them where the reflection says and
checks their sizes. Pointers are passed as `deviceAddress()` values; `VDBInfo` carries the texture and
surface handles of the atlas channels (`nvrhi::cuda::IDevice::getTextureHandle` / `getSurfaceHandle`),
which `shaders/GVDB.slangh` turns into `Sampler3D` / `RWTexture3D` with `Handle()`. `GVDBVolume`
checks the host `VDBInfo` against the reflection of the kernels. The library containers, the
raycaster and the samples' kernels are compiled with `-fp-mode fast` (NVRTC `--use_fast_math`), as the
reference's nvcc builds were.

The OptiX programs (`kernels/optix/OptixPrograms.slang`) run on nvrhi's ray tracing interfaces
(`rt::IPipeline`, `rt::IShaderTable`, `rt::IAccelStruct` of the CUDA device). They read everything
from a `[[vk::push_constant]]` launch parameter block (the TLAS, the tables of volumes, materials and
TLAS instances, camera, output); a hit finds its scene instance by `InstanceID()`, so the shader
records only select the hit groups: two per instance (radiance, shadow), a volume's by its
intersection mode, so the instance's hit group offset is `2 * instanceIndex`. The volume intersection
programs run the raycaster's `rayCast()`, generic over the brick function.

Interop with the graphics device goes through `nvrhi::IInteropDevice` (queried from the CUDA
device): `ScreenPresenter` imports its textures (`importTexture`) and orders the queues with
`computeQueueWaitForGraphicsQueue` / `graphicsQueueWaitForComputeQueue` around each frame's CUDA
work; `sph_fluid_pool` imports its vertex buffers (`importBuffer`). D3D12 and Vulkan only: the
samples refuse `-d3d11`.

## Conventions for code in this tree

- Donut fork object model: objects derive from `nvrhi::ObjectImpl<Interface>` (or
  `nvrhi::WeakReferenceSourceImpl`, `nvrhi::DelegatingObjectImpl`), declare their QueryInterface
  table (`NVRHI_BEGIN_INTERFACE_TABLE_INLINE` ... or `NVRHI_INHERIT_INTERFACE_TABLE()` when the base
  class's table suffices), and are created with `MAKE_RC_OBJ(T, args...)` (raw, refcount 1) or
  `MAKE_RC_OBJ_PTR(T, args...)` (`nvrhi::AutoPtr<T>`). `nvrhi::TakeOver(raw)` adopts a reference.
  No `std::shared_ptr`, no `new` for these objects.
- Interfaces: `NVRHI_IID(IName, "uuid")` before the struct, `NVRHI_DECLARE_UUID_TRAITS(IName)` inside,
  factory functions return `nvrhi::FRESULT` and an out pointer (`FS_OK`, `FE_*`).
- Errors: check `nvrhi::FRESULT` with `NVRHI_FAILED()`; the samples use `UT_CHECK(expr)`
  (`sample-utils/SampleTypes.h`) which logs and asserts.
- Math: Donut's `dm::` types (`float3`, `int3`, `affine3`, `float4x4`, `box3`). Row-vector convention:
  `p' = p * M`, `affine3::transformPoint()`. Angles in degrees where the reference used degrees.
- Structs shared with Slang code have host mirrors with `static_assert`s on their layout (CUDA layouts:
  `float3` is 12 bytes aligned to 4); structs used in arrays carry the padding of the reference's
  `__align__(16)` explicitly.
- Slang loops: read loop invariants behind pointers (bounds, texture handles, scales) into locals before
  the loop, keep reads out of `&&` loop conditions, and do a lookup once rather than test it and then
  repeat it. In the CUDA that Slang emits, such reads stay inside the loop; the raycaster ran up to 48%
  slower until `GVDBRaycast.slangh` did this. Details: `ethereal-nvrhi/doc/cuda-slang-performance.md`.
- File names: PascalCase in `gvdb/` and `sample-utils/`; directories kebab-case
  (`docs/conventions/naming.md`). Macros `UPPER_SNAKE_CASE`.
- Logging: `donut::log::info/warning/error` (console samples also print to stdout). Donut's error log
  breaks into the debugger: expected refusals are warnings.
- Runtime layout: executables in `build/bin`, shader containers in `build/bin/shaders/gvdb`, HLSL
  shaders in `build/bin/shaders/<target>/<dxil|spirv>`; assets are read in place from
  `GVDB_SAMPLES_ASSETS_DIR` (compile definition).
- Building: from the repository root,
  `.\build.ps1 -CMakeArgs '-DETHEREAL_WITH_CUDA=ON','-DETHEREAL_BUILD_GVDB=ON'` (needs the CUDA 12
  headers; OptiX with `NVRHI_WITH_OPTIX`, default ON, from the OptiX SDK 9.0 headers; `build.ps1` sets
  up the Visual Studio environment). Single targets: `.\build.ps1 -Target <name>`. The OptiX samples
  are skipped without OptiX.
- `IGVDBVolume::updateAtlas()` assigns atlas slots but does not initialise their voxels (the reference
  pre-filled a fixed atlas with `FillChannel`). Samples that accumulate into new bricks (point-fusion)
  fill them first (`IGVDBVoxelOps::fillChannel` after a full rebuild, or `copyAtlasBlock` per new brick).
- Interop resources take `SampleUtils::interopSharedFlags()` (`SharedResourceFlags::Shared`).
- Host read-backs: `IComputeContext::readBuffer()` (it finishes the context), or
  `SampleUtils::syncCompute(context)` before reading results some other way.
- Unattended runs: every GUI sample accepts `--screenshot <file.png> [--frames N]` (handled by
  `GVDBApp`: after N presented frames the back buffer is saved without the ImGui overlay and the
  window closes); `interactive-optix` also takes `--samples N` for the progressive accumulation.
  The console samples (`render-to-file`, `render-kernel`) write their PNG next to the executable.
  `--debug` enables the graphics debug runtime and nvrhi validation; `-d3d12`, `-vk` select the API.

## Samples

| Directory | Reference | What it shows |
|---|---|---|
| `render-to-file` | gRenderToFile | console: load explosion.vbx, volume render, write PNG |
| `render-kernel` | gRenderKernel | console: a user raycast kernel through `VolRenderer::renderCustom` |
| `3d-print` | g3DPrint | solid voxelization of lucy.obj at selectable voxel sizes, cross-section inset |
| `depth-map` | gDepthMap | volume composited against a rasterized depth buffer |
| `resample` | gResample | dense RAW volume resampled into a sparse topology (dense / sparse / halo) |
| `spray-deposit` | gSprayDeposit | ray bundles deposit material (points to voxels), smoothing |
| `point-cloud` | gPointCloud | point time series to level set, OptiX rendering, scene files |
| `point-fusion` | gPointFusion | simulated depth scans fused incrementally (accumulateTopology) |
| `fluid-surface` | gFluidSurface | SPH fluid (Slang kernels) surfaced through GVDB, OptiX rendering |
| `interactive-optix` | gInteractiveOptix | volume + polygons path traced with OptiX |

Not ported: gInteractiveGL (OpenGL), gJetsonTX (Jetson), gNanoVDB (NanoVDB), gImportVDB (needs the
OpenVDB library; `bunny.vdb` cannot be read without it).

Defects of the earlier CUDA C++ port fixed by the Slang port: the raycaster's hierarchical DDA read a
node's child list for the cell on the node's far face (`p == res`), past the list (another node's
children, or past the end of the pool; it crashed fluid-surface after a few dozen frames); and colour
channels were read with `tex3D<uchar4>` through texture objects created for linear filtering, which
returned float bit patterns instead of the colours (fluid-surface shows its particle colours now).

## SPH fluid pool

`sph_fluid_pool/` is a port of the SPH simulator fluids5.0 (Rama Hoetzlein, reference checkout
`D:\ps\repo\fluid-dev\fluids3\fluids5.0`), migrated with the same strategy as the GVDB samples. It
does not use GVDB; it uses the compute layer, the sample framework (`GVDBApp`) and the scene graph.
Target and executable: `sph_fluid_pool`.

| Reference | Port |
|---|---|
| class `Particles` (buffers, grid, parameters) | `sph::ISPHParticles` (data), `SPHFluid.h` / `SPHParticles.cpp` |
| `Particles::Run` and its stages | `sph::ISPHSolver` (operations), `SPHSolver.cpp` |
| `fluid.h`, device half of `datax.h` | `SPHFluidParams.h` (the kernel arguments; the kernels declare the same layouts) |
| `particles.cu` | `kernels/SPHFluidKernels.slang` (same kernels and physics; the parameter block and the buffer tables are kernel arguments instead of `__constant__` data) |
| CUDA driver API calls | the compute context (`gvdb::IComputeContext`, `gvdb::launchKernel`) |
| CUDA / OpenGL interop VBOs | vertex buffers of the graphics device imported into the CUDA device (`SPHParticlesDesc::interop`, `IInteropDevice::importBuffer`) |
| `Particles::Draw`, point shader of nv_gui | `sph::SPHPointRenderer`, `shaders/Points.hlsl` |
| the application's single fluid | `sph::SPHFluidInstance`, a scene-graph leaf (`SPHFluidScene.h`) |
| `Camera3D`, nv_gui text, keys | camera leaf + orbit controller, ImGui Inspector, same keys |

The default scene is the reference's wave pool (example 2, "large beach front": a 500 x 200 x 500
domain with a sloped floor and a wave-making wall) with 4,000,000 particles. Options:
`--particles N`, `--example 0..3`, `--paused`, `--check N` (reads the particles back after step N
and logs a sanity report), plus the framework's `--screenshot <png> [--frames N]`, `--debug` and
`-d3d12` / `-vk`. Keys: Space pauses, R resets; right drag (or Alt + left drag) orbits,
middle drag pans, the wheel zooms.

Notes:
- The position, colour and velocity buffers are shared between the simulation and the rasterizer
  without a copy; `GVDBApp`'s frame orders the CUDA and graphics queues around them.
- The camera uses the reference's nominal setting literally: an 80 degree horizontal field of view
  at the window's aspect ratio. The reference executable shows a narrower view (45.5 degrees
  horizontally, 34.9 vertically, stretched) because its projection halves the angle and keeps a
  4:3 aspect, so the pool appears smaller here than there.
- The fill uses a deterministic `std::mt19937` instead of `rand()`, so runs are repeatable.
