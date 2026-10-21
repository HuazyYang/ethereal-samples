# Point-cloud Slang input and reconstruction slice

This plan began with reusable input and reconstruction helpers and has since been integrated into the point-cloud application. Mixed mesh/volume rendering, progressive samples, environment lighting and time-series presentation now use the shared Slang renderer. The historical main is retained only as a reference in this archived plan; `SceneFile.*` remains the input parser.

## Preserved behavior

- Scene sections/tags, defaults, column-zero section recognition, comma vectors, first `%[0][width]d` filename conversion, literal directory-prefix concatenation, asset lookup and pre-set command-line render-scale precedence match SceneFile.*. The new parser uses the SDK-free shared SceneMaterial schema and retains mesh, polygon-series, camera, light and output metadata for the mixed renderer.
- Point frames are little-endian signed int32 count, float3 minimum and maximum, then tightly packed unsigned ushort3 positions. Odd point counts are valid; raw GPU uploads receive zero padding to a four-byte boundary. Trailing bytes remain accepted as in the original loader. Invalid counts, truncated headers/payloads, nonfinite or reversed bounds are rejected before allocation; a failed load leaves the caller's frame unchanged.
- GPU conversion is `(minimum + ushort * ((maximum-minimum)/65535)) * renderScale`, with no translation. The existing Slang convertAndTransform counterpart is reused.
- Each frame rebuilds sparse leaf16 topology with leaf-first log dimensions `{4,3,3,3,3}` and support radius 2, then bins the original float positions into width-4 subcells with radius 1. The legacy extra-brick integer coverage is `[int(point)-2,int(point)+4]`. Atlas placement is compact and independent of serialized world positions.
- The legacy `FP16` path is normalized unsigned 16-bit, not IEEE half. Subcell membership is determined before re-quantization. Bounds are actual point extrema padded by -1/+2 and clamped at zero for nonnegative axes; each expanded point is truncated to ushort and decoded as `(ushort/65535)*range+minimum`. Gather writes `min(3,distance-1)` at integer voxel positions, updates aprons with boundary 3, and optionally runs the existing six-neighbor smoothing with the scene's center weight, bias and iteration count. Every new frame replaces topology and values; it never accumulates prior frames.
- Point overlay data preserves the original maximum 250,000 line glyphs, stride `ceil(count/250000)`, delta `(0.5,0.5,0.5)` and color `position/256`.

## Deliberate correctness extensions

Negative coordinates previously disappeared in legacy CUDA binning. On axes containing negative input, use un-clamped padded extrema and floor-aligned signed topology/subcell coordinates; retain all points. Nonnegative assets preserve the original quantization. Small clouds confined to one brick are supported. Inputs outside the exact half-voxel float domain are rejected clearly. The deterministic portable smoothing uses separate source/destination storage, resolving legacy in-place neighbor races.

## Validation

Independent CPU fixtures cover all scene defaults/tags, teapot.scn, format/prefix behavior, malformed/truncated/odd-length input, odd point counts, nonfinite/reversed bounds, constant axes and real points.dat decoding. GPU fixtures cover conversion, normalized-ushort quantization, every voxel/apron against a CPU subcell gather and smoothing reference, signed points, and two unrelated frames proving no accumulation. All four compute APIs must pass before integration. The real 1,867,648-point asset and teapot scene provide the reconstruction acceptance case; full mixed-renderer parity is explicitly outside this slice.

## Executed results (2026-10-18)

The CPU input/parser test and corrected analytic/full-asset reconstruction tests pass. The real source asset reconstructs 14,583 active leaf16 bricks on D3D11, D3D12, Vulkan and CUDA. CPU source-file reconstruction at eight occupied voxels differs by at most 6.31e-5 across the tested APIs. Independent analytic tests cover every voxel/apron and two smoothing iterations, signed coordinates, constant axes, odd counts, failed-frame preservation and two unrelated frames without accumulation. GPU normalized-ushort codes are validated separately within one code of CPU float normalization; tight gather/smoothing checks then use those verified codes. Corrected CUDA analytic and real-asset sanitizer runs both report zero errors (`corrected-reconstruction-analytic-memcheck.log` and `corrected-reconstruction-asset-memcheck.log`). The earlier sanitizer files used the superseded leaf8 topology. Current logs are `branch-order-analytic-tests.log`, `branch-order-asset-tests.log`, and `point-cloud-leaf16-detail.log`.

## Application integration and acceptance

The application has now been migrated to `PointCloudAppScene` plus the shared mixed Slang renderer. The retained legacy main and parser remain available. See `src/gvdb_samples/samples/point-cloud/README.md` for the implemented behavior, renderer limitations and exact validation results. The earlier application/window/sanitizer acceptance used the superseded leaf8 topology and finite primary-ray range. Those results must be refreshed after the topology and primary-ray-policy corrections. Exact legacy image equivalence remains a separate comparison task.

## Branch-order correction

The legacy `fromBranching(r4,r3,r2,r1,r0)` signature reverses argument order. The final argument 4 means leaf16, not leaf8. Reconstruction now uses `{4,3,3,3,3}` and 16-voxel support alignment. Analytic fixtures explicitly assert leaf16/apron18 and independently enumerate support voxels to compare the complete leaf set. Corrected reconstruction fixtures pass on all four APIs, including an independent complete leaf-set reference. Earlier leaf8 counts, memory sizes and image acceptance are historical and superseded; current application captures are tracked in the sample README.

## Public owner adoption (2026-10-19)

Reconstruction now uses public host activation, volume generations, separate gather
association/evaluation PointSets, apron updates and smoothing. GPU quantization is
performed once per input ordinal; original positions still determine membership.
The app explicitly reads decoded GPU values into copied PointSet uploads. A fresh
owner per frame preserves prior snapshots on late failures. Compute and mixed RT
render directly from public raw snapshots; overlays use public host topology.
`readTopology` is an explicit diagnostic snapshot readback used by independent
fixtures, never a reconstruction or rendering authority. Public quantization and
the separate inverse-distance color role remain open.
