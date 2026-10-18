# Point-fusion Slang migration contract

The production example keeps the procedural 10x10 city, 100 m blocks, 7.4 m lanes,
3.7 m curbs, five index voxels per metre, 240x180 scanner, 90-degree horizontal
field of view, four-block ray range, radius-one level set, radius-two support,
eight-voxel leaves and four-voxel subcells. City generation uses seed 1043.

## Scanner and explicit corrections

`PointFusionScanner` uploads boxes as two float4 vectors and a uint4 color record.
Each pixel owns one uint LCG state. It advances that state three times per scan,
using the first two values for jitter. The old CUDA source indexed seeds with
thread-local coordinates, allowing different blocks to race on the same seed;
that data race cannot define reproducible reference output. A deterministic
per-pixel seed replaces it, while retaining the 1664525/1013904223 LCG.

Every pixel writes a validity flag, position, and packed RGBA, including misses.
The exclusive scan compacts valid hits in pixel order. Position (0,0,0) remains a
valid hit; it is never used as an invalid sentinel. Position.w=0 and flag=0 mark
invalid pixels. Outside-city scans clear outputs instead of leaving stale data.
Axis-parallel slabs are handled explicitly. Equal-distance boxes select the first
object. The strict maximum-distance comparison retains the previous contract.

## Persistent scalar and raw color state

`PointFusionVolume::fuse` reads the compact positions for CPU topology management.
It unions all leaves touched by [point-support, point+1+support]. It constructs
subcell point lists in stable scan order while building topology. This deliberate
CPU step avoids the existing atomic insertion order affecting byte-quantized
color averages. The distance minimum and color accumulation execute in Slang.
This is a correctness-first migration; GPU-resident stable binning can replace
this host work without changing the public data contract.

A Slang remap pass copies old scalar and raw RGBA values to the new atlas using
world coordinates, independent of changed root position, leaf ordering or atlas
shape. Newly activated voxels start at distance 3 and transparent black. Existing
`SlangPointFusion` gathers accumulate into that imported state. The accumulated
level set is the minimum of previous distance and point distance minus radius.
The color recurrence retains the legacy previous-color-as-one-contributor rule,
with scan-order sums and byte truncation. CPU color comparisons allow one byte
of floating-point quantization difference. Cells with no new contributors are
copied as raw bytes without re-quantization. Scalar aprons use boundary zero; color
aprons use transparent black. A scan containing no valid points leaves the
existing volume unchanged. Reset/color-channel toggling clears accumulated state.

The viewer integration receives `volume()`, `packed()`, `colorAtlas()` (raw RGBA8),
`hasVolume()` and statistics. Interactive parity additionally requires a per-frame
update hook, explicit FPV camera, scanner mouse/key precedence, colored surface
sampling and line/point overlays. A level-set renderer must treat missing sparse
space as far distance 3; the current density renderer returns zero and would
otherwise report false iso-zero boundary hits. These viewer changes are owned
separately and are prerequisites for claiming the full example migrated.

## Validation

Scanner fixtures compare every hit, color, original pixel index and updated RNG
state against an independent CPU slab reference over two scans, including empty
scenes, parallel rays, origin hits, ties, far clipping and outside-city scans.
Fusion fixtures compare every interior/apron scalar and color against a stable
CPU recurrence across repeated scans, newly introduced negative-coordinate
leaves, root/atlas reordering, empty scans and reset. Compile both new Slang modules
for DXBC/SM5, DXIL, SPIR-V and PTX; run fixtures on all four NVRHI backends and CUDA
Compute Sanitizer. Real-city frames and viewer controls follow after integration.
