# Portable GVDB kernel contracts

`kernels.json` is the compile manifest. Each entry is a compute shader; use DXBC
`cs_5_0` or DXIL/SPIR-V/PTX `cs_6_5`. Vulkan register shifts match NVRHI defaults:
`t=0`, `s=128`, `b=256`, `u=384`, set zero. Compile each entry independently and
use its artifact reflection; CUDA layouts must not be guessed from D3D slots.
`GVDBPortable.h` defines host parameter and structured-buffer layouts.

## Memory and dispatch

The implementation binds explicit buffers. It neither embeds GPU pointers nor
indexes resource arrays. Every entry uses at most eight UAVs. Nodes retain the
64-byte VBX binary layout in raw buffers, with all node levels concatenated into
one buffer and all child levels concatenated into another. `LevelInfo` identifies
each pool's byte offset, stride and count. Validate all offsets and complete pool
extents fit a 4 GiB buffer, strides are aligned, node strides are at least 64,
child strides contain the complete child grid, hierarchy levels are at most ten,
and references target valid pools before upload. Oversize pools are unsupported;
serialized 48-bit indices must never be truncated. Bitmask-compressed child lists
are not implemented. Full child grids and original VBX reference values are used.

Position/velocity buffers use `float4` (16-byte stride), not a backend-specific
`float3`. Scalar atlas kernels use one `float` per voxel in x-major order. Texture
format conversion and additional channel formats require separate entrypoints;
these kernels do not reinterpret packed byte channels as floats.

Positive dimensions, a nonempty brick interior (`width > 2*apron`), matching atlas
resolution/brick counts, valid buffer sizes and finite parameters are required.
Host dispatch skips zero-sized workloads and validates 32-bit counts/offset sums.
All declared bindings needed by the chosen artifact must be supplied. Insert GPU
barriers between producer/consumer dispatches and before readback.

## Algorithms

- `scanBlocks` writes a local exclusive scan at u0 and one sum per 256 elements
  at u1. Recursively scan block sums, then `addScanBlockOffsets` reads globally
  scanned sums at t2 and adds them to u0. Arithmetic is modulo 2^32; host callers
  requiring allocation offsets must preclude overflow. The input is t1.
- Sort is 64 stable binary LSD passes. `radixFlags` reads `KeyValue` at t0 and
  writes zero flags at u0. Globally scan the flags, bind the scan at t1, and run
  `radixScatter` to u2. Swap key input/output after every bit, 0 through 63.
  Duplicate values retain their input order. This is a portable baseline, not
  a performance-equivalent replacement for optimized vendor radix sort.
- `reduceFloatBlocks` reads t3 and writes `Reduction` at u3. Recursively use
  `reducePartialBlocks` with t4 until one result remains. NaNs are omitted;
  infinities follow IEEE arithmetic. Empty reduction is `{+inf,-inf,0,0}` and
  requires no GPU dispatch. Compare floating sums with an appropriate tolerance.

## Volume, topology, particles and SPH

Neighborhood operators read an immutable source at t0 and write a distinct
allocation at u0. They copy untouched apron voxels. Update aprons between stencil
iterations. `updateApron` fills edges/corners and arbitrary apron widths by sparse
neighbor lookup. `resampleAtlas` uses `value.x` as absent-source background and
requires nonzero remap range. `downsampleDense` requires an axis-aligned,
nonnegative scale transform; it integrates a source box per destination voxel.

Topology writes raw nodes at u0 and children at u1. Link levels top-down and unlink
levels bottom-up, with barriers between levels. Coordinates must be unique within
a level. `applyLeafMarkers` consumes t1; `markLinkedLeaves` produces a separate
marker array from existing parent references. Parent and child references retain
VBX encoding. Root nodes may legitimately have a null parent.

Sparse particles: clear leaf counts, insert points, globally scan counts, then
scatter positions. Invalid/out-of-volume points have cell `0xffffffff`, offset
zero. Insertion order within a cell is unspecified; sort by key when deterministic
ordering is needed. SPH follows clearGrid, insertGrid, scan grid counts,
scatterGrid, computeDensity, computeForce, advanceParticles. Grid cells must be
at least the world smoothing radius wide so the 27-cell neighborhood is complete.
The SPH parameter layout follows the existing physical equations, including
self-exclusion and density fallback 1. Input and output particle buffers differ.

## Raycasting and validation fixtures

Raycast parameters supply origin and per-pixel direction increments in index
space. March step and maximum steps must cover the bounded ray interval. Transfer
entries are RGB plus nonnegative extinction; output is premultiplied radiance
composited with the specified background. Bounds are half-open. Scalar samples
use explicit trilinear interpolation and updated atlas aprons. At least one
transfer entry, a nonzero transfer range, positive march step, and a nonzero
finite ray direction are required. `raycastSurface` is an isosurface baseline;
it does not yet reproduce the legacy tricubic, level-set, shadow, and section
rendering modes.

`tests/PortableFixtures.h` supplies independent CPU expectations and a two-brick
sparse volume for backend tests. Test scan lengths 0,1,255,256,257,65537; stable
sort duplicates and both uint2 halves; recursive reduction and NaNs; stencil
source/destination isolation; sparse traversal, apron faces/corners and topology;
SPH equal/opposite pair forces; and analytic constant-density ray absorption plus
parallel-face misses. Source compilation is separate from runtime correctness.
Legacy CUDA sources remain until production callers and equivalence coverage are
migrated. These shaders are the portable foundation, not a claim of completed
GVDB example migration.

## Host integration

`GVDBPortableData` validates and packs CPU node pools before NVRHI upload.
`GVDBSlangVolume` owns the packed hierarchy and scalar atlas resources and calls
`ComputeRuntime` for volume operations and raycasting. `GVDBLegacyAdapter` reads
an existing host volume without performing GPU work; synchronize/fetch legacy
GPU topology changes before conversion.

`GVDBPortableVbx` loads VBX files directly into CPU packed topology and
atlas data without CUDA initialization. The raw API supports all five formats and
multiple ordered channels in one grid; the scalar API remains a single R32_FLOAT
convenience. It expands VBX1.0/2.x bitmasks and preserves the original atlas
slots and transform, and saves canonical VBX1.11. Compression, multi-grid, and topology reuse fail with explicit
diagnostics. The explosion.vbx asset has roundtrip tests preserving exact scalar
bits and normalized topology. `GVDBSlangSph` orchestrates complete grid, density,
force and leapfrog steps using NVRHI buffers and the same offline kernels.


## Additional portable production paths

`SlangAtlas` preserves tightly packed channel bytes, including 12-byte RGB32 and
one-byte R8. Its raw operators use one invocation per voxel, except R8 where each
invocation owns four consecutive bytes. Buffers have four-byte trailing padding;
public readback excludes that padding. Fill uses legacy normalized RGB for RGBA8,
byte values for R8, and XYZ/alpha-one for float vectors. Float smoothing is
component-wise; byte expansion and integer RGB normalization preserve their
legacy formulas. The helper validates incompatible formats and parameter ranges.

`discoverBrickKeys` produces stable sorted/unique ushort-coordinate keys and
per-level counts using the shared scan/radix pipeline. `SlangPointFusion` accepts
explicit disjoint `GatherCell` ranges, initializes the whole destination, and
runs density or level-set gathers. Points are float4 and colors packed RGBA8.
The constructor rejects overlapping destination cells and out-of-range points.

`voxelizeMesh` returns a dense R8/R32/S32 grid from a transformed indexed mesh.
It bins triangles in Y, scans/scatters their ranges, and uses conservative box
intersection plus oriented ray parity. Triangle bins include a half-voxel guard
so bin boundaries do not omit intersecting surfaces. Sparse topology activation
from this dense result is a separate integration step.

`raytracing.json` describes the nine-stage-entry procedural RT library. It uses
32-byte maximum native radiance payload storage (five scalar values plus CUDA alignment), 16-byte hit attributes, two ray types, two miss records,
and recursion depth two. A geometry has consecutive radiance/shadow hit records.
Camera/light positions are world space, hierarchy bounds are object/index space,
and transformed instances retain index-space marching step size. The initial
volume closest-hit path composites the configured background. Compute raycast
continues to serve D3D11. See `GVDBMigration.md` for remaining public-interface and
feature integration work.


## Sparse construction, velocity transfer and packed points

`buildTopology` creates validated node64/child pools from aligned leaf origins,
including negative coordinates and deduplicated leaves. `buildScalarVolume`
activates leaves using a caller-specified threshold and fills interiors/aprons
from a dense scalar array. Empty active topology is reported explicitly. These
CPU builders do not initialize CUDA or allocate GPU resources.

`binPointSubcells` expands each point to existing subcells touched by the legacy
support box `[point-radius, point+1+radius]`, then counts, scans and scatters with
NVRHI buffers. Its output feeds `SlangPointFusion` directly. Point/color pairing
is preserved; order within each cell is unspecified. Width must divide the leaf
width and be at most eight voxels. Allocation sizes and scan limits are checked.

`transferGridVelocity` samples three scalar channels on their staggered MAC face
locations with explicit trilinear interpolation. It blends PIC velocity with
particle velocity plus the grid delta and optionally advects using a midpoint
step. Current and previous channels must have the same sparse atlas layout and
updated aprons. Out-of-volume particles retain position and velocity. This is
not a pressure solver or a particle-to-grid transfer implementation.

`convertPackedPoints` reads unsigned byte/ushort or IEEE float/double XYZ streams
with explicit byte offset and stride. Binary64 conversion uses two 32-bit words
and round-to-nearest-even, requiring no shader doubles. Converted float4 points
use w=1. `quantizePointsU16` writes tightly packed ushort3 positions with clamping,
truncation and deterministic zero padding; nonfinite input components encode
zero. Every output word has one writer, including odd point counts.

`ExtendedComputeTests.cpp` exercises these production helpers and scalar
resampling/downsampling through the shared runtime. Its independent references
cover affine MAC fields across sparse seams, support expansion and empty bins,
software double rounding ties, packed stream offsets/strides and quantization.
