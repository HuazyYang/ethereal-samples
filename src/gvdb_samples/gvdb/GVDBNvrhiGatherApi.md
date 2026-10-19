# Owned point density and level-set gathers

Include `GVDBNvrhiGatherApi.cmake` to add the public factory and point conversion
API to the SDK. Installed consumers include `GVDBNvrhiGatherApi.h`, find
`gvdbNvrhi 1 CONFIG`, and link `gvdb::nvrhi_api`. Static and shared builds use the
same header. The gather implementation and PointSet authentication are private
parts of that binary; no concrete gather target or private header is installed.
No CUDA/graphics SDK or Slang compiler discovery is required by consumers.

Create `IGVDBGather` with `createGather`, passing the owned volume and an offline
`GVDBAtlasGather.slang` artifact, entry `gatherOwnedAtlas`. The sample build writes
`shaders/gvdb_gather/<dxbc|dxil|spirv|ptx>/GVDBAtlasGather_gatherOwnedAtlas.bin`.
Create point data through `IGVDBPointOps`: prepare CPU input, record conversion,
submit and wait explicitly, then finish the upload ticket into `IGVDBPointSet`.
Gather accepts a finished PointSet from this API binary on the same NVRHI device;
its creating factory and volume may already have been released. Its canonical
CPU bytes are the actual GPU conversion result, and its retained raw GPU streams
refer to the same immutable dataset. Arbitrary external buffers are not adopted.

Fill the 64-byte `GatherRequestDesc`, then call
`gather->prepare(request, budget, &ticket)`. The volume must have a current,
finished publication. `channel` selects R32_FLOAT; optional `colorChannel`
selects a distinct RGBA8_UINT channel and requires PointSet colors, even for an
empty atlas. An empty PointSet has no color attribute and is therefore scalar-only. The default
`UINT32_MAX` disables color. Position Float3 and Float4 streams are accepted;
only XYZ participates, and velocities are unused. Translation components must
all be finite; XYZ is added in binary32 before both coverage and shader distance
calculations. For a nonempty atlas, every translated XYZ must remain finite,
even if another coordinate places the point outside the active bounds. Radius must be finite and
strictly positive. `supportRadius=-1` follows that radius for bin coverage; an
explicit finite value >=0 selects independent support without changing scalar
distance calculations. `accumulate` is exactly 0 or 1, and every reserved field
is zero. The request remains 64 bytes, with support radius at offset40 and the
remaining reserved word at offset44.

Preparation uses stable CPU cell-to-point-index lists from authenticated point
bytes and frozen placed topology. It never reads back the GPU or records,
submits, or waits. Point buffers are retained by prepared bindings; the PointSet
may be released after preparation. Publish the returned existing generation
ticket with `volume->recordAndPublish(context, ticket)`, then close, submit and
synchronize through NVRHI. Tickets obey the normal one-use, same-owner and stale
host/configuration/publication checks. Preparation failure clears the raw output
and preserves the live volume. Old snapshots retain their original storage.

Subcell widths 1, 2, 4 and 8 are supported when they divide leaf width. Cells use
the common leaf-origin phase, so shifted valid roots and signed coordinates are
supported. For each translated point component `p`, binary32 endpoints are
`p - support` and `(p + 1.0f) + support`, where support resolves from
`supportRadius` as described above. Each endpoint is floored onto the
phase-relative subcell grid; bounds are inclusive and clipped to the active
bounding box before integer conversion. Only active placed leaves receive
pairs, including a root-only leaf. Point indices within each cell preserve
original input order. This makes color summation deterministic for a given
backend; the retained CUDA atomic insertion order did not specify that order.

For every active interior voxel, density starts at 0 and takes a candidate
`radius - distance` only when it is greater. Level set starts at 3 and takes
`distance - radius` only when it is less. Accumulation instead starts from the
previous scalar bits. Explicit comparisons preserve seed bits on ties and when
a candidate or seed is NaN. Distance uses the integer voxel coordinate without
a half-voxel shift. Color includes every point in the cell, regardless of its
distance from that voxel. For example, subcell width1, a point at0, distance
radius1 and support radius2 include the cell at3 for color even though its
density contribution is zero. Fresh color starts at `(0,0,0,1)`; accumulation starts
from the previous RGBA8 bytes divided by 255 and counts that value as one extra
contributor. Input colors are byte storage units, divided by 255 once. Sum,
divide by the contributor count, truncate each component times 255, and keep
its low byte. In particular the initial alpha contribution intentionally wraps;
one opaque point in a fresh cell produces alpha 254, not a saturated 255.
Normalized binary32 sums/divisions can round to opposite sides of an integer
byte boundary on different backends, so these computed color bytes do not have
a universal cross-backend bit-identity guarantee. The independent fixture uses
a derived interval around the nonnegative byte-sum quotient: with `N`
contributors, `t=(N+8)*FLT_EPSILON`, its bound is `512*t/(1-t)`. Only truncated
integers intersecting that interval are accepted, and the fixture asserts at
most two choices. Exact endpoint/power-of-two cases, alpha wrapping, unchanged
aprons, empty accumulation and unused-slot bytes remain strict checks.

Fresh empty cells receive scalar 0/3 and color 0. Accumulating empty cells retain
exact previous bytes, including NaN payloads and signed zero. A nonempty atlas
always prepares fresh selected-channel storage, even when no point pairs exist.
Both modes preserve all active apron bytes exactly and zero unused atlas slots.
There is no implicit apron refresh; compose the existing explicit apron operator
when needed. Other channels and topology retain their resources/revisions. An
empty atlas is an authenticated one-use no-op after descriptor and PointSet
authentication; it does not walk or translate unused point data.

`GatherPreparationBudget` separately limits all leaf-pool cell-table slots
(including inactive leaves), clipped candidate visits before sparse lookup,
actual cell-to-point associations, and new GPU bytes/buffers. Counting and range
checks precede publication. Candidate and pair limits prevent oversized support
from silently truncating work.

Signed positions, shifted grids, root-only leaves, deterministic ordering,
fresh reset when there are no pairs, defined empty-cell colors, apron retention
and unused-slot zeroing are explicit policies or corrections to retained paths;
they do not promise legacy byte identity in those cases. Normalized-ushort
weighted color remains unsupported: the retained variant sums color/distance
without normalization and performs undefined nonfinite/out-of-range integer
casts at zero distance. This API does not emulate those casts or silently add
an epsilon/weight normalization.

## Separate association and evaluation positions

Query `IGVDBGatherEvaluation` from the existing factory and call
`prepareEvaluation` with the 80-byte `GatherEvaluationDesc`. Its embedded
`request.points` supplies original cell associations; `evaluationPoints`
supplies distance positions and optional colors. Both sets must be completed,
authenticated by this API binary, on the owner device, and have equal counts.
The caller guarantees ordinal correspondence. Component counts/strides can
differ; association colors are unused. Translation applies to both sets and
must remain finite for both on a nonempty atlas. The usual empty-atlas policy
still authenticates both but does not translate unused data.

This preserves membership when a quantization or deformation stage moves a
point across a subcell boundary. Budgets count association visits/pairs; no
implicit rebinning, conversion or readback occurs. Prepared bindings retain
evaluation storage after either PointSet or factory is released. Ordinary
`prepare` uses one set for both roles and retains its original descriptor,
vtable and behavior. Color equations remain ordinary gather equations; this
extension does not implement normalized-ushort inverse-distance color.
