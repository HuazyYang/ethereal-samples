# Slang resample example

The original `head.raw` data path runs through NVRHI on D3D11, D3D12, Vulkan and
CUDA. Graphics backends provide a window; CUDA renders headlessly to an image.
The asset is exactly 128 x 256 x 256 unsigned bytes. Values are divided by 256,
then sampled into a 256-cubed index domain with 16-voxel leaf interiors.

The index-to-source transform remains `(0.5*x, z, 252-y)`. Dense mode activates
4,096 leaves. Sparse mode thresholds the original coarse downsample at 0.05,
activating 2,472 leaves. The optional single layer of face neighbors produces
2,997 leaves. Halo growth reads the original mask, so it does not cascade.

The application retains the original dense Float32 CPU input (RAW bytes divided by 256). Each rebuild creates a fresh public `IGVDBVolume`; no persistent `PackedVolume`, typed atlas, or `SlangVolume` shadow is kept.

Sparse selection uses `IGVDBDownsample`: copied-source preparation, caller recording/submission/wait, then completed values compared strictly with the threshold. Dense mode skips this query. The public host owner activates the coarse leaf masks and optional simultaneous six-face halo within the half-open destination extent. It finishes topology, publishes the initial scalar channel, then publishes the public `IGVDBResample` result with zero outside background. Publications submit in order on the graphics queue. Pending ComputeRuntime work is flushed before the separate command list is used.

Only after both publications are recorded and submitted does the sample replace its visible state. A preparation failure leaves the old state visible; submitted device failures are not recoverable transactions. A snapshot publication does not imply GPU completion. Same-queue viewer commands consume the ordered publication; readback explicitly submits and waits. CPU atlas/map copies exist only during verification. Independently retained snapshots preserve resources across later rebuilds and owner release.

The viewer consumes the canonical raw snapshot for density/surface/voxel/section/inset rendering. Topology overlays read public host nodes. The public resample operator covers coordinate-zero atlas planes, including the documented correction to the retained CUDA launch omission. Retained handwritten CUDA sources remain available; this migration does not authorize their removal.

The downsample placement remains `extent/(resolution+1)`, using two transformed endpoints, truncation and inclusive source boxes. Reflected upper endpoints clamp to the lower endpoint and are not reordered. GPU sums/counts plus correctly rounded CPU division preserve exact 0.05 threshold classification. Endpoint division is exact binary32 and Vulkan explicitly requires RTE32. The separate generic `downsampleDense` contract and old `downsampleRegion`/statistics helpers remain available as retained portable compatibility paths; this consumer uses the public query.

Examples:

```text
resample -dx12
resample -vk --sparse --no-halo --topology
resample -cuda --sparse --output head.png
resample -dx11 --self-test
resample -dx12 --sparse --topology --frames 2 --validation
resample -cuda --headless --verify --width 128 --height 96 --output checked-head.png
```

Keys 1, 2 and 3 toggle topology, sparse activation and halo, respectively.
Left dragging or arrow keys orbit; middle dragging pans, and right dragging or
the mouse wheel zooms. Shift-left dragging moves the light. Rebuild statistics appear
in the window title. The original Inspector checkboxes are represented by these
keyboard controls. The transfer-function stops and volume extinction are retained.

The shared viewer starts at yaw 90 degrees and pitch 20 degrees and frames the
current topology bounds at 4.5 times their bounding-sphere radius. The original
camera used a fixed target `(128,100,128)` and distance 1000, so dense/sparse framing
and rendered pixels are not claimed to match the original screenshots.

Use `--self-test` for independent transformed voxel/apron, coarse-value, activation, threshold, copied-input, retained-snapshot and empty-publication checks. `--verify` checks all three modes on the selected asset. `--frames N` bounds a graphics window for validation; `--headless --output image.png` saves the compute image. `--adapter N --validation` selects an explicit graphics adapter and enables native validation. CUDA runs headless.

Production artifacts are generated with tests disabled. Runtime consumers load offline artifacts and require no shader compiler or CUDA Toolkit discovery.

The portable Slang/NVRHI application is the supported implementation.

Earlier portable checkpoint evidence (before the ownership migration):

The corrected leaf16 topology and threshold-division regression pass all eight
analytic/full-asset tests on D3D11, D3D12, Vulkan and CUDA. The independent CPU
masks agree exactly: dense 4,096, sparse 2,472, and sparse+halo 2,997 leaves. See
`resample-abi-build.log` and `resample-abi-tests.log` in the aggregate build
directory. All six corresponding graphics-only cases pass too.
`resample-binding-audit.log` verifies both entry layouts across all targets. CUDA
Compute Sanitizer reports zero errors for corrected analytic and full-head runs:
`corrected-resample-analytic-memcheck.log` and `corrected-resample-head-memcheck.log`.
