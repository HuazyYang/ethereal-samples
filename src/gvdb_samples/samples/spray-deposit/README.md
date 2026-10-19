# Slang spray deposition

`spray-deposit` runs the retained metal-part deposition example through NVRHI on D3D11, D3D12, Vulkan and CUDA. Graphics backends provide a window and Inspector; CUDA runs headlessly. The original application remains in `legacy-main.cpp` for the separate legacy build.

```powershell
spray-deposit.exe -dx12
spray-deposit.exe -cuda --headless --frames 3 --width 128 --height 96 --output spray.png
spray-deposit.exe -vk --validation --frames 2 --width 640 --height 480 --screenshot spray-window.png
spray-deposit.exe -dx11 --self-test
```

`--shade` selects 0 Off, 1 Voxel, 2 Surface, 3 Section or 4 Volume. `--wand` selects 0 Rotate, 1 Sweep or 2 Wave. `--paused`, `--no-rays` and `--topology` control the initial state. Keys 1/Space toggle simulation, 2 topology, 3 ray overlays, 4 shading and 5 wand style. Grave accent hides/restores the Inspector. Left drag orbits, middle drag pans, right drag/wheel changes distance, and Shift-left/right adjusts the light. The Inspector exposes the same simulation, topology, ray, shading and wand controls. Generic viewer V/I shortcuts are consumed so they cannot replace the sample renderer.

The part uses the original `metal.obj` scale 200 and translation (200,200,200), leaf-first log dimensions `{5,3,3,3,3}` (legacy root-first `fromBranching(3,3,3,3,5)`), density 1 for both surface and interior, RGBA8 gray and two initial smoothing passes with center weight 4. The real asset produces 651 allocated 32-voxel leaves, with one-voxel aprons. Deposition never grows that topology.

Each animation step generates the original 1,000 rays using MT19937 seed 6572 and the original rotate/sweep/wave equations and random draw order. Ray queries use step .25, threshold .3, epsilon .01 and hit bias -.0001. Four-voxel subcells gather radius-one density as `max(existing,1-distance)` and average the prior color with all contributing subcell point colors. Both channel aprons update each step; a center-weight-four smoothing pass runs every twentieth step. Public point, gather, apron and filter operators publish immutable owned snapshots. Ray queries and all display modes bind their raw channel ranges directly. CPU code classifies initial voxelizer output for sparse topology and reads ray hits/colors for canonical point preparation and the debug overlay; those readbacks remain explicit. No helper volume owns a second GPU copy of the production channels.

The sample renderer preserves all five formulas: colored voxel and trilinear Phong with shadows, the fixed X=50 section, and the original Beer-Lambert accumulation followed by its second alpha blend. Transfer tables contain 16,384 entries with lookup endpoint 16,300. The original camera is yaw/pitch 45/40, target (200,200,200), distance 1000 and horizontal FOV 50; the light is 80/50 at distance 800. Native captures contain the computed image; Inspector widgets are displayed in the window but are not baked into that image.

Ray traversal uses buffer-backed geometric leaf/voxel DDA instead of CUDA pointer HDDA. Sampling retains the epsilon applied at each hierarchy descent, including thin first-voxel selection. Cell selection is separate from sampling bias, so large unnormalized directions and zero epsilon do not skip cells. Rounded box-entry positions can produce a zero-length cell immediately before a face; the traversal advances through up to three such face corrections without sampling or consuming the nonempty-cell budget. Exact negative-direction voxel faces also advance. These are deliberate numerical repairs, not claims of reproducing pathological legacy traversal. Query status distinguishes a miss from exhaustion, and renderer exhaustion produces an invalid depth that application validation rejects.

The portable color path reads the actual RGBA8 bytes and normalizes by 255. The retained CUDA color texture descriptor/fetch mismatch documented in [fluid-surface](../fluid-surface/README.md) is not reproduced. Debug rays use the shared viewer overlay, whose line rasterization differs from the retained DebugDraw implementation. The shared overlay composites every covering line in draw order with source alpha, preserving the original .2 ray opacity. Exact legacy screenshot parity is a separate comparison from cross-backend consistency.

All nineteen analytic, asset, application, control and bounded-window CTests passed. Seven additional control/native-window checks passed with Inspector input tests and native validation enabled. CUDA memory checks of the analytic/real-asset suite, controls and three-step 1024x768 application each reported zero errors. The final 128x96 three-step pixel bytes are identical across all four APIs, contain 2,744 surface pixels, and report 1,000 spray hits on the final step. The 64x48 real-camera query has 686 hits and zero traversal failures on every backend. The suite includes complete CPU scalar/RGBA deposition references, a twenty-frame CPU smoothing stencil, 257 oblique entry rays, signed/parallel/invalid/large-direction/zero-epsilon rays, exhausted versus completed budgets, thin-voxel selection, independent five-mode equations, controls and Inspector construction. The final shared overlay correction is included in the full 231-test passing matrix, and the 1024x768 application memory check passed again with zero errors and 175,726 image hits. Evidence: `build/spray-final-tests.log`, `build/spray-inspector-tests.log`, `build/spray-{asset,controls,application}-memcheck.log`, `build/spray-application-alpha-memcheck.log`, `build/spray-alpha-cross-api.json`.

A fresh 1024x768 three-step legacy comparison has RGB mean absolute error 0.392708/255, maximum channel difference 93, 5.7715% of pixels differing by more than one and 0.9982% by more than eight. After Gaussian sigma-4 filtering, RGB mean absolute error is 0.282041/255. Geometry, camera and the translucent ray fan match closely. Fine line rasterization and a faint face grid in the legacy image remain visibly different; exact legacy screenshot parity is not claimed. Captures and metrics: `build/spray-legacy.png`, `build/spray-new-alpha.png`, `build/spray-legacy-alpha-comparison.json`.

## Public snapshot ownership acceptance (2026-10-19)

Spray now uses the public mesh voxelizer, topology owner, dense resample, point
factory, gather, apron and smoothing APIs. The scene exposes retained snapshots
and public host topology. Raw query/render views validate device, format, offset,
range and level metadata; root-only topology supplies a valid unused child buffer.

All 19 affected tests and eight expanded analytic/control checks pass on Windows.
The expanded tests compare structured/reference and raw snapshot queries, use a
nonuniform offset fixture, retain snapshots after owner destruction, reject
malformed views, and preserve visible generations on failed preparation. Six
explicit graphics API/adapter combinations pass native validation. CUDA memcheck
reports zero errors. The graphics-only build passes 15/15 tests with CUDA toolkit
discovery disabled, and a tests-disabled production build runs the real asset on
AMD D3D12 (651 bricks, 1,000 hits, 2,744 image hits).

Twenty 128x96 three-frame captures were compared with the previous portable
implementation. Modes 0?3 are byte-identical on all four APIs. Mode 4 differs by
at most one channel byte in 177 CUDA/D3D12, 180 D3D11 and 179 Vulkan pixels; the
cause has not been isolated. This does not establish exact legacy-image parity.
Evidence is under `build/m5-spray-owned`, including `image-comparison.json`.
