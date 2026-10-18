# Slang resample example

The original `head.raw` data path runs through NVRHI on D3D11, D3D12, Vulkan and
CUDA. Graphics backends provide a window; CUDA renders headlessly to an image.
The asset is exactly 128 x 256 x 256 unsigned bytes. Values are divided by 256,
then sampled into a 256-cubed index domain with eight-voxel leaf interiors.

The index-to-source transform remains `(0.5*x, z, 252-y)`. Dense mode activates
32,768 leaves. Sparse mode thresholds the original coarse downsample at 0.05,
activating 17,609 leaves. The optional single layer of face neighbors produces
20,616 leaves. Halo growth reads the original mask, so it does not cascade.

The separate `downsampleRegion` helper preserves the original placement rule:
`extent / (resolution+1)`, truncated transformed endpoints and inclusive source
boxes. Reflected upper endpoints clamp to the lower endpoint; they are not
reordered. The generic `downsampleDense` helper retains its separate box-integration
contract. Atlas resampling initializes absent source values to zero and updates
all allocated apron voxels from the dense source.

Examples:

```text
resample -dx12
resample -vk --sparse --no-halo --topology
resample -cuda --sparse --output head.png
resample -dx11 --self-test
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

`--self-test` checks reflected/permuted and axis-aligned transforms against CPU
prefix-volume integration, exact activation masks and every atlas/apron voxel.
`--verify` repeats those checks with the complete input asset in dense, sparse
and halo modes, then renders the selected mode. Tests assert finite pixels and a
nonempty volume contribution; image files alone are not treated as proof of data
correctness. CTest runs both fixture and full-asset cases on each enabled API.

`legacy-main.cpp` retains the previous implementation for the non-Slang build.
