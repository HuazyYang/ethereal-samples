# Slang SPH pool

The four original SPH presets run through NVRHI compute on D3D11, D3D12,
Vulkan and CUDA. Graphics rendering reads the same simulation buffers directly.
CUDA supports headless simulation and particle CSV output.

```powershell
sph_fluid_pool -dx12 --example 2
sph_fluid_pool -vk --particles 257 --example 3 --frames 4 --validation
sph_fluid_pool -dx11 --particles 257 --example 3 --screenshot pool.png --frames 8
sph_fluid_pool -cuda --particles 4096 --example 1 --frames 20 --output particles.csv
```

The Inspector shows particle count, simulation time and step, pause/resume/reset,
and optional compute timing. Timing explicitly waits for the GPU and runs only
while the Inspector is visible. Reset is applied during the next animation phase,
so GUI event handling never opens a simultaneous immediate command list.

Controls are Space to pause, R to reset, right drag or Alt+left drag to orbit,
middle drag to pan, wheel to zoom, and the backtick key to hide/show the Inspector.
Arrow-key orbit remains available. Key repeat does not repeatedly toggle pause,
reset or visibility. Hidden UI does not capture scene input; releasing a drag over
the Inspector cancels the scene drag. `--no-ui` omits the Inspector.

`--frames N` bounds a window by rendered frames, including paused frames.
Without an explicit frame limit, an interactive window remains open.
Headless runs use the requested number of simulation steps; `--screenshot`
selects headless graphics rendering at 640x480. Registered render passes are
removed and submitted work is drained before pass destruction, including errors.

The current display uses automatically framed particle billboards. The portable
scene graph, fixed camera, one-pixel depth-tested points and ground grid are the
supported presentation; exact historical image parity is not claimed.

Validation: all seven full-build SPH tests passed on 2026-10-18 (4.63 seconds).
Four tests exercise simulation and graphics readback/CUDA completion. Three
native-window tests inspect actual Inspector draw geometry, visible/hidden input
capture, orbit/pan/zoom, pause and deterministic deferred resets, then run two
paused window frames. Native D3D11/D3D12/Vulkan validation logs contain no errors
or warnings. Results: `build/sph-inspector-tests.log` and
`build/sph-inspector-tests.xml`. The GUI fixture is available with
`--particles 257 --example 3 --paused --gui-input-test --validation`.

The isolated graphics-only build, with CUDA Toolkit discovery disabled, passed
all six SPH tests (3.26 seconds), including the native Inspector cases. Logs are
`build/sph-inspector-graphics-tests.log` and
`build/sph-inspector-graphics-native-detail.log`.
