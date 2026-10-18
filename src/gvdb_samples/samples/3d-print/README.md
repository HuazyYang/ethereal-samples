# Slang 3D-print example

The example loads the original `lucy.obj`, runs the portable GPU solid voxelizer,
constructs a sparse scalar atlas, and renders through the shared volume viewer.
D3D11, D3D12, Vulkan and CUDA use the same Slang compute kernels. CUDA uses the
headless image path; graphics backends also provide an interactive window.

The model transform retains the original order: Y rotation by -10 degrees,
translation by `(0.3,0.45,0.3)`, reciprocal voxel size, and a 100 mm part scale.
The transformed model bounds receive a 10% margin on every side. The dense working
grid is expanded to 32-voxel boundaries, including negative coordinates, then
converted to sparse leaves. Surface voxels contain 1, interior voxels 0.5, and
outside voxels 0. The shared atlas stores R32_FLOAT and populated neighbor aprons.

```text
3d-print -dx12
3d-print -vk --voxel-size .3 --shading section --section-height .6
3d-print -cuda --voxel-size .5 --output lucy.png
3d-print -dx11 --self-test
```

Key 1 toggles topology. Key 2 cycles voxel, surface, section and volume rendering.
Key 3 cycles the original 0.5, 0.4, 0.3 and 0.2 mm voxel sizes and rebuilds the
volume. Mouse height moves the section plane. Left dragging orbits, middle
dragging pans, right dragging or the wheel zooms, and Shift-left dragging moves
the light. `--no-inset` hides the cross-section
inset; `--shading section2d` produces the section alone. Statistics appear in the
window title. `--screenshot` is accepted as an alias of `--output`.

The viewer frames the active index bounds rather than the original fixed world
camera. Section height is a normalized fraction of those bounds, rather than the
old fixed 0..100 mm mouse mapping. The inset occupies a quarter of the output
width and height instead of a fixed 256x256 target. The initial transfer function,
solid values and voxelization transform are retained; screenshot equivalence to
the legacy renderer is not claimed.

`--self-test` runs an analytic closed cube through GPU voxelization and CPU sparse
construction. It verifies all dense classifications (56 surface and eight interior
voxels), every atlas/apron sample and an exact 32x32 cross-section image. The fixture
runs at both zero and negative index origins. CTest also loads and voxelizes the
actual Lucy asset at 0.5 mm with `--verify` on each enabled backend and checks
47 leaf32 bricks, 56,839 surface voxels and 131,750 interior voxels. The complete
leaf-origin set is checked against an independent partition of the occupied dense
voxels, rather than using a GPU count as its own reference. It requires finite pixels and
a nonempty rendered model contribution outside the inset. A section-only render
must contain nonblack model pixels; an empty black section does not pass.

The dense working grid and CPU topology construction are explicit migration
choices; they replace the old hierarchical voxelizer orchestration. Numerical
triangle-boundary behavior follows the portable voxelizer contract documented in
`gvdb/slang/GVDBMigration.md`. `legacy-main.cpp` retains the old non-Slang build.

The corrected leaf32 analytic and Lucy asset cases pass on all four APIs. See
`branch-order-analytic-tests.log` and `branch-order-display-asset-tests.log` in the
aggregate build directory. Earlier leaf8 screenshots are superseded. Corrected CUDA
Compute Sanitizer runs report zero errors for the analytic cube and Lucy asset:
`corrected-print-analytic-memcheck.log` and `corrected-print-lucy-memcheck.log`.
