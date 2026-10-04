# gvdb_samples

Port of [NVIDIA GVDB Voxels 1.1.1](https://github.com/NVIDIA/gvdb-voxels) (reference checkout:
`D:\ps\repo\gvdb-voxels\source`) to the Donut / nvrhi stack of this repository. The GL, Jetson and
NanoVDB samples are not ported; OpenGL is replaced by nvrhi (D3D11 / D3D12 / Vulkan through
Donut's `DeviceManager`), OptiX 6 by OptiX 9 behind a COM-style interface.

```
gvdb/                      the library
  GPDevice*.h/.cpp         gp: COM-style compute abstraction. CUDA driver-API backend
                           (GPDeviceCUDAImpl), nvrhi interop (GPDeviceNVRHI), Thrust helpers
                           (GPDeviceCUDAUtils.cu), OptiX 9 backend (GPDeviceOptiX*, optional)
  GVDB.h                   the data structure and its operations as interfaces (see below)
  GVDBScene.h              GVDB volumes in Donut's scene graph
  GVDB.cuh, GVDBDDA.cuh    device headers shared by every kernel (node traversal, DDA)
  kernels/*.cu             library PTX modules: GVDBOperators (apron, fill, smooth, resample ...),
                           GVDBParticles (prefix sum, points, GPU topology, voxelization)
samples/
  sample-utils/            renderers (CUDA raycaster, OptiX), the app framework, OBJ meshes
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

Operation objects are created for one volume (`createGVDBVoxelOps(volume, &ops)`, ...) and use only
the public `IGVDBVolume` interface, so operations can be added without touching the data structure.
They share the library kernels through `IGVDBVolume::getKernel(name)`, which loads the two PTX
modules once.

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
  (`sample-utils/kernels/GVDBRaycast.cu`), compositing instances in order; also custom render
  kernels and `raytrace()` of ray bundles.
- `SampleUtils::OptixRenderer` (OptiX 9): one custom AABB primitive per volume instance, one
  triangle GAS per mesh, all instanced in one IAS so the node transforms apply directly.

### gp and the OptiX layer

`gp` (`gvdb/GPDevice.h`) is the compute abstraction the port was built on: `IDevice`, `IDeviceQueue`,
`IBuffer`, `ITexture`, `IModule` / `IKernel`, interop with nvrhi resources through shared handles and
keyed mutexes / timeline semaphores (`GPDeviceNVRHI.h`). Native handles are exposed where other
libraries need them (`IBuffer::getNativeHandle()` = `CUdeviceptr`, `IDeviceQueue::getNativeHandle()`
= `CUstream`, `IDevice::getNativeHandle()` = `CUcontext`).

`gvdb/GPDeviceOptiX.h` encapsulates the OptiX 7+/9 host API the same way: `IRTDevice` (a child of
the CUDA device) creates `IRTModule` (PTX / OptiX-IR), `IRTProgramGroup`, `IRTPipeline`,
`IRTAccelStruct` (triangles, AABBs, instances; built on a gp queue) and `IRTShaderBindingTable`,
and launches on a gp queue. The backend (`GPDeviceOptiXImpl.cpp`) is compiled when the SDK is found
(`ETHEREAL_WITH_OPTIX`, default ON; `OptiX_INSTALL_DIR` or the default install location).

Device code is shared between CUDA and OptiX: the kernels include `gvdb/GVDB.cuh` and
`sample-utils/kernels/GVDBScene.cuh`; `CUDA_PATHWAY` (plain kernels, `scn` is a `__constant__`) or
`OPTIX_PATHWAY` (programs, `scn` comes from the launch parameters) is defined by the CMake helper
`gvdb_add_ptx()`. `ScnInfo` and the launch parameters are defined once in
`sample-utils/kernels/optix/OptixLaunchParams.h` (host and device). On the OptiX path the per-volume
raycast settings (`SCN_THRESH`, steps, extinction, transfer function, colour channel) resolve to the
`OptixVolumeInstance` of the hit record being intersected, so one launch renders several volumes with
their own attributes; `rayCast()` takes the brick function as a template parameter so that OptiX calls
are direct. The OptiX renderer keeps the materials in a device table; the SBT holds, per scene
instance (volumes first, then mesh instances), one hit group record per ray type (radiance, shadow)
whose program group is chosen by the instance's intersection mode, so the instance's `sbtOffset` is
`2 * instanceIndex`.

## Conventions for code in this tree

- Donut fork object model: objects derive from `nvrhi::ObjectImpl<Interface>` (or
  `nvrhi::WeakReferenceSourceImpl`, `nvrhi::DelegatingObjectImpl`), declare their QueryInterface
  table (`NVRHI_BEGIN_INTERFACE_TABLE_INLINE` ... or `NVRHI_INHERIT_INTERFACE_TABLE()` when the base
  class's table suffices), and are created with `MAKE_RC_OBJ(T, args...)` (raw, refcount 1) or
  `MAKE_RC_OBJ_PTR(T, args...)` (`nvrhi::AutoPtr<T>`). `nvrhi::TakeOver(raw)` adopts a reference.
  No `std::shared_ptr`, no `new` for these objects.
- Interfaces: `NVRHI_IID(IName, "uuid")` before the struct, `NVRHI_DECLARE_UUID_TRAITS(IName)` inside,
  factory functions return `nvrhi::FRESULT` and an out pointer (`FS_OK`, `FE_*`).
- Errors from gp calls: check with `NVRHI_FAILED()`; the samples use `UT_V_GP(expr)`
  (`sample-utils/SampleTypes.h`) which logs and asserts.
- Math: Donut's `dm::` types (`float3`, `int3`, `affine3`, `float4x4`, `box3`). Row-vector convention:
  `p' = p * M`, `affine3::transformPoint()`. Angles in degrees where the reference used degrees.
- File names: PascalCase in `gvdb/` and `sample-utils/`; directories kebab-case
  (`docs/conventions/naming.md`). Macros `UPPER_SNAKE_CASE`.
- Logging: `donut::log::info/warning/error` (console samples also print to stdout).
- Runtime layout: executables in `build/bin`, PTX in `build/bin/ptx` (file systems mount it as
  `ptx`), shaders in `build/bin/shaders/<target>/<dxil|dxbc|spirv>`; assets are read in place from
  `GVDB_SAMPLES_ASSETS_DIR` (compile definition).
- Building: from the repository root, `.\build.ps1 -CMakeArgs '-DETHEREAL_BUILD_GVDB=ON','-DETHEREAL_WITH_OPTIX=ON'`
  (needs the CUDA toolkit; `build.ps1` sets up the Visual Studio environment). Single targets:
  `.\build.ps1 -Target <name>`. The Thrust helpers (`GPDeviceCUDAUtils.cu`) are compiled by nvcc
  without any nvrhi header (nvcc's host pass rejects the `NVRHI_IID` constexpr GUIDs); the gp-facing
  wrappers are in `GPDeviceCUDAUtils.cpp`.
- `IGVDBVolume::updateAtlas()` assigns atlas slots but does not initialise their voxels (the reference
  pre-filled a fixed atlas with `FillChannel`). Samples that accumulate into new bricks (point-fusion)
  fill them first (`IGVDBVoxelOps::fillChannel` after a full rebuild, or `copyAtlasBlock` per new brick).
- Interop textures take `SampleUtils::interopSharedFlags(device)`: D3D11 needs the NT handle + keyed
  mutex flags, D3D12 and Vulkan exactly `Shared` (nvrhi's Vulkan backend exports memory only then);
  the Vulkan device is created with `VK_KHR_external_memory_win32` / `VK_KHR_external_semaphore_win32`.
- Host read-backs: `IDevice::waitForQueue()` waits for the last `commitQueue()` sync point, so use
  `SampleUtils::syncQueue(device, queue)` (commit + wait) before mapping a staging buffer.
- Unattended runs: every GUI sample accepts `--screenshot <file.png> [--frames N]` (handled by
  `GVDBApp`: after N presented frames the back buffer is saved without the ImGui overlay and the
  window closes); `interactive-optix` also takes `--samples N` for the progressive accumulation.
  The console samples (`render-to-file`, `render-kernel`) write their PNG next to the executable.
  `--debug` enables the graphics debug runtime and nvrhi validation; `-d3d11`, `-d3d12`, `-vk`
  select the API.

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
| `fluid-surface` | gFluidSurface | SPH fluid (CUDA) surfaced through GVDB, OptiX rendering |
| `interactive-optix` | gInteractiveOptix | volume + polygons path traced with OptiX |

Not ported: gInteractiveGL (OpenGL), gJetsonTX (Jetson), gNanoVDB (NanoVDB), gImportVDB (needs the
OpenVDB library; `bunny.vdb` cannot be read without it).

## SPH fluid pool

`sph_fluid_pool/` is a port of the SPH simulator fluids5.0 (Rama Hoetzlein, reference checkout
`D:\ps\repo\fluid-dev\fluids3\fluids5.0`), migrated with the same strategy as the GVDB samples. It
does not use GVDB; it uses gp, the sample framework (`GVDBApp`) and the scene graph. Target and
executable: `sph_fluid_pool`.

| Reference | Port |
|---|---|
| class `Particles` (buffers, grid, parameters) | `sph::ISPHParticles` (data), `SPHFluid.h` / `SPHParticles.cpp` |
| `Particles::Run` and its stages | `sph::ISPHSolver` (operations), `SPHSolver.cpp` |
| `fluid.h`, device half of `datax.h` | `SPHFluidParams.h`, shared by host and kernels |
| `particles.cu` | `kernels/SPHFluidKernels.cu` (same kernels and physics) |
| CUDA driver API calls | gp (`IModule` / `IKernel`, constant uploads, buffer copies) |
| CUDA / OpenGL interop VBOs | nvrhi vertex buffers imported into gp (`SPHParticlesDesc::interop`) |
| `Particles::Draw`, point shader of nv_gui | `sph::SPHPointRenderer`, `shaders/Points.hlsl` |
| the application's single fluid | `sph::SPHFluidInstance`, a scene-graph leaf (`SPHFluidScene.h`) |
| `Camera3D`, nv_gui text, keys | camera leaf + orbit controller, ImGui Inspector, same keys |

The default scene is the reference's wave pool (example 2, "large beach front": a 500 x 200 x 500
domain with a sloped floor and a wave-making wall) with 4,000,000 particles. Options:
`--particles N`, `--example 0..3`, `--paused`, `--check N` (reads the particles back after step N
and logs a sanity report), plus the framework's `--screenshot <png> [--frames N]`, `--debug` and
`-d3d11` / `-d3d12` / `-vk`. Keys: Space pauses, R resets; right drag (or Alt + left drag) orbits,
middle drag pans, the wheel zooms.

Notes:
- On D3D12 and Vulkan the position, colour and velocity buffers are shared between the simulation
  and the rasterizer without a copy. D3D11 cannot create shareable vertex buffers through nvrhi,
  so there the three buffers are copied through the host every frame (`areRenderBuffersShared()`).
- The camera uses the reference's nominal setting literally: an 80 degree horizontal field of view
  at the window's aspect ratio. The reference executable shows a narrower view (45.5 degrees
  horizontally, 34.9 vertically, stretched) because its projection halves the angle and keeps a
  4:3 aspect, so the pool appears smaller here than there.
- The fill uses a deterministic `std::mt19937` instead of `rand()`, so runs are repeatable.
