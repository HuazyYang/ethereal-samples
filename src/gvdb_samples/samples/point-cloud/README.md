# Slang point-cloud sample

`point-cloud` loads the original `teapot.scn` and `points.dat`, reconstructs each packed point frame with Slang, and renders the point volume plus the scene meshes through NVRHI. D3D12, Vulkan RT and CUDA/OptiX use the shared Slang mixed renderer. D3D11 and the explicit `--renderer compute` path render the point volume through the portable compute surface renderer. Compute rendering does not include polygon meshes, recursive material effects or environment lighting.

```powershell
point-cloud -dx12 --samples 2 --screenshot point-cloud.png
point-cloud -vk --headless --samples 2 --verify-volume --width 128 --height 96
point-cloud -cuda --headless --samples 2 --verify-volume
point-cloud -dx11 --renderer compute --frames 2 --screenshot point-cloud-compute.png
```

The default renderer is ray tracing and fails explicitly when unavailable. `-cuda` selects the unified CUDA backend; the retained `--cuda` spelling explicitly selects compute rendering. `--headless` writes `point-cloud.png` unless `--output` or `--screenshot` supplies another filename. Window screenshots close after the requested sample/frame count. `--frames` counts rendered samples, while `--samples` also keeps the current dataset fixed throughout capture.

Scene inputs retain `--in`, `--frame`, `--scale`, `--key` and `--info`. Packed point frames, model transforms, material records, environment images, camera/light orbits, polygon time series, smoothing and the 16,384-entry transfer table with lookup endpoint 16,300 are mapped explicitly. Dataset and polygon frames advance at the original `maxSamples` cadence; each point reconstruction replaces the previous volume. Missing or malformed next-frame files fail explicitly. The original keyframe interpolation applies to frames 1100 through 1340.

Controls:

- `1`: point glyphs; `2`: all-level topology.
- Left drag: camera orbit; middle drag: pan; right drag or wheel: distance.
- Shift-left/right drag: light orbit/distance.
- Generic viewer mode/section shortcuts are consumed because this application uses its scene-selected surface renderer.

The scene parser and packed ushort3 input tests are described in [RECONSTRUCTION.md](RECONSTRUCTION.md). Negative-coordinate point frames are intentionally retained using signed padded quantization bounds. Original nonnegative assets retain the padded/clamped behavior. Random ray samples are independent per pixel; the old shared-pixel random-state race and sample-1 accumulation reset are corrected in `SlangSceneRT`. The new camera uses pixel centers; legacy primary rays used pixel corners. Ray tracing preserves the legacy primary range `[0,1e16]`, independently of the raster camera near/far planes; near/far regression meshes and resize/sample progression are tested on all three RT APIs. The Inspector exposes point/topology toggles and frame, sample and atlas statistics.

A freshly rebuilt legacy executable was compared at 800x600 with eight samples. The corrected scene has 480,000 scene-hit pixels and 14,583 leaf16 bricks. Mean absolute RGB difference is 15.139/255, including the visibly independent low-sample ray noise; exact legacy image parity is not claimed. The captured images are `build/point-cloud-legacy.png` and `build/point-cloud-new.png`.

Validation on 2026-10-18:

- Nine helper tests: CPU input/parser plus analytic and full-asset reconstruction on all four APIs.
- Fourteen application tests (including three RT camera/resize regressions): input, camera/light, key animation, point/polygon frame replacement and capture freeze on all four APIs; real-asset compute rendering on all four APIs; real-asset mixed rendering on D3D12, Vulkan and CUDA.
- At 128x96, compute renders contain 479 hit pixels on every API. Mixed renders contain 12,288 scene-hit pixels and 470 point-volume contribution pixels on all three APIs. The contribution check compares identical sample-0 full-scene and mesh-only depth, so a visible ground plane alone cannot pass. Finite legacy miss depth is excluded from scene-hit counts.
- Corrected leaf16 native windows at 320x240 and two samples pass on D3D12/Vulkan RT and D3D11 compute with native validation enabled. Earlier 640x480 screenshots were visually inspected; two-sample mixed rendering is visibly noisy.
- CUDA Compute Sanitizer reports zero errors for input/control fixtures, full-asset compute rendering and full-asset mixed rendering with the contribution check. Corrected leaf16 reconstruction and both full-asset application paths passed the fourth-checkpoint sanitizer reruns with zero errors.

At 128x96 the compute D3D11/D3D12/Vulkan PNGs are identical; CUDA differs by one byte value in two pixels. Mixed RT PNGs have sparse differences from floating-point and secondary-ray divergence: D3D12 versus Vulkan differs in 10 pixels with mean absolute RGB byte error 0.00705; CUDA differs in 41 pixels with mean error 0.00798. Maximum errors are 57 and 44 respectively. Bit-identical RT output is not claimed.

Logs are under the aggregate build directory: `point-cloud-app-build.log`, `point-cloud-app-input-tests.log`, `point-cloud-app-asset-tests.log`, `point-cloud-final-window-*.log`, `corrected-reconstruction-*-memcheck.log`, and `checkpoint-4-point-cloud-*-memcheck.log`. The previous source remains available as `legacy-main.cpp`; non-Slang builds retain that target.

## Public snapshot ownership (2026-10-19)

Frames now publish through `IGVDBVolume` and the public gather/apron/filter APIs.
`IGVDBGatherEvaluation` keeps original point-to-cell membership while evaluating
the GPU-quantized positions. A fresh owner replaces the visible snapshot only
after successful preparation/publication; retained previous snapshots stay valid.
The compute preview and mixed RT renderer consume raw snapshot channels directly,
and topology overlays use public host pools. Source conversion and normalized-ushort encoding/decoding now use public PointSet
and quantization operators. Quantization completion returns the actual GPU-decoded
PointSet directly, eliminating the application decode/readback/reupload bridge.
Weighted ushort color gathering is a separate operator contract.

Ownership acceptance: complete matrices pass 334/334 and graphics-only 252/252,
with no skips; both graphics adapters pass native reconstruction/app controls,
and CUDA memory checks are clean. Installed static/shared gather consumers each
pass 2/2. All four compute captures exactly match the preserved portable images;
RT changes at most one byte in two color channels. Tests-disabled production
matches DX11 exactly, and three bounded native compute windows pass. Logs and
image comparisons are under `build/m5-owned-point-cloud` in the aggregate.

The public quantization integration passes the four-API analytic, full-asset and
rendering fixtures, both native graphics adapters, and CUDA memory checking.
Compared with preserved portable captures, Vulkan compute is identical; the
other compute paths change six pixels by one byte. D3D12/CUDA RT change at most
eleven pixels with maximum byte difference 12; Vulkan RT changes one byte by one.
Conversion and actual-code references pass; cross-backend bit identity is not promised.
