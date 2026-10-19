# Portable fluid-surface

The default scene simulates 1,500,000 particles using FLUIDS-v3 SPH and reconstructs their radius-one level set every simulation frame. This is the original sample's solver family, not a FLIP replacement. All compute kernels run through Slang and NVRHI on D3D11, D3D12, Vulkan or CUDA. Mixed fluid/ground ray tracing uses the shared SlangSceneRT renderer on D3D12, Vulkan RT or CUDA/OptiX. The application includes no CUDA or OptiX SDK headers.

```powershell
fluid-surface -dx12 --frames 1 --samples 8 --screenshot fluid.png
fluid-surface -vk --headless --frames 2 --samples 2 --verify-volume
fluid-surface -cuda --headless --frames 1 --samples 8
fluid-surface -dx11 --renderer compute --frames 1 --samples 1 --screenshot fluid-compute.png
```

`--frames N --samples M` simulates N fixed .003-second steps, then accumulates M samples of the final state. Window captures close after N+M-1 presentations. `--paused` renders the initial state. `--particles` changes the requested count explicitly; `--test-scene` selects a separate compact 4,096-particle regression scene and camera. Default assets are the original ground.obj and sky.png; `--assets` changes their directory.

The default RT path preserves the original fluid and ground material, five shading selections, point light, camera/light orbit, rotated pretranslated ground, and environment. `--renderer compute` explicitly selects a level-set-only preview without ground polygons, recursive material effects or environment lighting; it supports off and levelset, and rejects other initial shading modes. RT fails explicitly when the selected device lacks the required feature.

Controls are Space for simulation, 1 for topology, 2 for every eighth particle's velocity line, 3 for shading, 4 for particle color, R for reset and grave accent for the Inspector/debug overlays. Left drag orbits, middle drag pans, right drag changes distance; Shift-left/right controls the light. The Inspector retains the simulation, topology, fluid, color, shading and reset controls. Generic viewer mode/section shortcuts are consumed.

Preserved numerical contracts include the 400 x 200 x 400 domain, original initialization block, density 400, mass .00020543, simulation scale .005, physical smoothing radius .015, viscosity .5, slope .1, wave force 100/frequency 6, acceleration 150 and velocity 3 limits. Opt-in `SphParameters.wall.w=1` implements both original moving X walls and insertion cells [1, resolution - 3]; mode 0 keeps the existing SPH pool rules. The retained call `fromBranching(3,3,3,3,5)` takes arguments from root to leaf. The portable hierarchy therefore uses leaf-first log dimensions `{5,3,3,3,3}`, with 32-voxel leaves and cumulative widths 32, 256, 2048, 16384 and 131072. All five level descriptors remain present; unused upper levels contain no active nodes. Reconstruction uses radius-two support, four-voxel subcells, unquantized positions, scalar background/aprons 3, and the exact shared atlas slots for raw RGBA8 colors. SPH, point conversion, gather and atlas/apron updates run on the device. Public preparation builds stable bins and host topology from explicit position readback; all GPU volume storage belongs to public publications.

The RT transfer table has 16,384 entries and inclusive endpoint 16,300. The retained threshold {iso 0, min 3, max -1} deliberately has a negative denominator. Combined with the original positive scalar cutoff and black/transparent TF first entry, the legacy deep shading choice can produce transparent fluid; it is retained rather than silently replaced with a different density mapping. Empty-skip is the original first-allocated-leaf debug mode.

Intentional corrections/extensions:

- The retained CUDA velocity clamp overwrites the measured speed before computing its factor, making the factor one. The portable solver clamps the actual incoming velocity magnitude; an independent 3-4-5-vector test covers this correction.
- Stable particle indices replace deep particle-buffer sorting, so colors keep their identity but floating-point neighbor summation order can differ.
- Initialization uses a fixed MSVC-rand-compatible seed 6572 and explicit z/y/x draws, matching the retained Windows argument order. Reset restarts that seed rather than continuing global rand state.
- Negative coordinates use signed sparse support instead of the retained kernel's rejection. Off removes the fluid instance while leaving the ground; reenabling rebuilds the current particle state.
- The shared renderer corrects the old shared RNG race and progressive sample-one reset, and uses pixel-center primary rays. See [SLANG_RT.md](../sample-utils/SLANG_RT.md).

Regression targets cover independent wall equations, guard cells, velocity clamp, signed-coordinate scalar/color gathers and aprons across 32-voxel leaf boundaries, simulation/capture/reset/input state, and signed transfer normalization against a packed-alpha Beer-law reference. The device-free `--topology-test` mode (`gvdb_fluid_topology_cpu` in CTest) checks the complete 1,500,000-particle initial preset against the retained run: 196 leaves (7 x 4 x 7), with bounds (96,64,96) to (320,192,320). The topology expands integer particle positions by [-2,+4], as the retained full rebuild does. This support expansion does not add a leaf to that particular initial preset because it remains inside those same aligned bounds; other inputs near a leaf boundary can activate extra leaves. The portable path rebuilds topology each frame rather than retaining the old incremental topology schedule.

An initial migration incorrectly copied the root-first argument list directly into the leaf-first builder, producing 8-voxel leaves and 10,923 bricks. This was a parity bug: the old gather tests checked that layout internally rather than checking the legacy configuration. It is corrected in source, with explicit legacy widths, bounds and leaf-count regressions. Earlier capture statistics for the 8-voxel layout do not establish parity for the corrected hierarchy. The corrected 32-voxel configuration passes all 19 fluid CTests: the CPU golden, seven GPU analytic cases and eleven compact/full-preset captures. The full preset reports 196 bricks on every API; the compact fixture reports eight.

Full-preset tests exercise the 1.5M-particle path, while RT contribution checks compare full-scene and ground-only depth so ground pixels alone cannot pass. Native validation passes without warnings/errors on D3D11, D3D12 and Vulkan. CUDA Compute Sanitizer reports zero errors for analytic compute, analytic RT, full-preset compute and full-preset RT; the updated shared mixed-scene RT suite also passes native validation and memcheck. Corrected compact compute images differ by at most one byte in two pixels across APIs; full-preset RT differences are at most two bytes in 28 pixels relative to D3D12. These cross-API comparisons check the same portable implementation, not exact legacy image parity. Two-sample mixed RT captures remain visibly noisy; increasing --samples reduces stochastic noise.

The legacy color texture has a separate retained bug: its RGBA8_UINT array uses the default normalized-float read descriptor, while the shader fetches `tex3D<uchar4>`. CUDA documents that integer read flags control this promotion ([CUDA 12.9 Driver API](https://docs.nvidia.com/cuda/archive/12.9.1/cuda-driver-api/group__CUDA__TEXOBJECT.html)). A measured constant stored RGBA (153,51,204,255) returns (154,205,205,0) through that legacy byte fetch, but returns the exact stored bytes with integer reads and point filtering. The portable path deliberately reads the actual RGBA8 bytes and divides by 255; it does not reproduce the invalid descriptor/fetch combination. A temporary corrected-legacy reference was built with those two descriptor changes, then the original source bytes were restored. The corrected legacy capture has the same smooth color gradient as the portable image. At 1024 x 768, one simulation step and eight samples, the fluid-region mean absolute RGB difference after Gaussian filtering (sigma 4 pixels, ROI x=378..649/y=265..474) drops from 5.751 to 1.255 byte values out of 255. The full-image filtered error is 1.142/255. Unfiltered error remains 13.732/255 with the distinct stochastic seed sequences and only eight samples; this is not an exact pixel-parity claim. Both captures report 1,500,000 particles and 196 bricks.

Validation evidence: build/fluid-topology-cpu.log, build/fluid-topology-compute-tests.log, build/fluid-topology-rt-full-tests.log, build/fluid-topology-*-validation.log, build/fluid-topology-*-memcheck.log, build/fluid-topology-cross-api.json build/fluid-legacy-color-probe.log and build/fluid-legacy-color-corrected-comparison.json. The matching reference images are build/fluid-new.png and build/fluid-legacy-color-corrected.png; build/fluid-legacy.png preserves the uncorrected reference.

The prior application remains in legacy-main.cpp with its original FluidSystem and CUDA kernels. Non-Slang legacy builds select it explicitly.

## Public reconstruction ownership

FluidSurface now creates authenticated public points, activates the exact signed
floor-support leaf origins, and publishes owned scalar/color gather and scalar
aprons. A private replacement owner is made visible only after all stages succeed.
Invalid SPH sentinels are filtered with their associated colors; failed early and
late rebuilds preserve prior channel bytes, and retained old snapshots survive
replacement. Compute surface rendering and mixed RT bind the same raw snapshot.
Topology overlays/statistics use public host pools; diagnostic readback is explicit.
The retained CPU topology builder and legacy GPU helpers remain reference tests.

The 19 fluid tests, including full 1.5M-particle cases, pass. Added rollback/filter/
lifetime cases pass all seven compute/RT analytic variants. Both graphics adapters
pass native validation, CUDA memcheck reports zero errors, and graphics-only fluid
plus interactive acceptance passes 24/24. Tests-disabled/CUDA-discovery-disabled
production renders on AMD D3D11. Compared with preserved portable captures, full
RT images are identical; compact compute changes at most four pixels by one byte,
compact RT changes one byte, and full D3D11 changes one byte. Evidence is under
`build/m5-fluid-owned`; these comparisons are not a broad performance guarantee.
