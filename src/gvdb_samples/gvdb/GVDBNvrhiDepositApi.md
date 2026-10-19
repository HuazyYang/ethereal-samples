# Owned point deposition

`createDeposit` creates `IGVDBDeposit` from a public volume and the offline
`GVDBPointDeposit_depositOwnedPoints` artifact for its device. It accepts only
same-binary authenticated volumes and finished same-device PointSets. The factory
retains its owner. Existing NVRHI interfaces and API version 1 are unchanged.

## Equations and association

`Central` adds `(1 - distanceSquared / radiusSquared)^3` to the truncated local
voxel. `SixNeighbors` also adds at the six axis neighbors. The radius is finite
and in `[1e-4, 1e8]`; contributions outside the radius are deliberately **not
clamped**. The legacy unused amplitude parameter is omitted. Floating-point sums
follow original input ordinal order; normal backend floating-point differences
remain. Large contributions or existing nonfinite scalar values can yield
nonfinite results under ordinary binary32 arithmetic.

The associated active leaf comes from `floor(position + translation)`. Expansion
writes that brick's physical apron; it does not redirect writes into a neighboring
leaf interior. Missing or unpublished leaves receive nothing. Expansion requires
apron at least 1. There is no topology growth or implicit apron refresh.

`LastOrdinal` writes the exact last central point's RGBA8 value. `AverageRGB`
uses exact unsigned integer RGB sums divided by the central point count, discards
previous color, and writes alpha 255. Colors on expansion neighbors are untouched.
The average mode limits total input count to `UINT32_MAX / 255`. These explicit
policies replace the retained CUDA scatter's raced writes.

`SupportXZ` uses canonical velocity XYZ as direction. It finds the leaf before
applying `direction * directionOffset`, then truncates shifted local coordinates
in that original leaf. Shifted coordinates in `(-1,0)` truncate to zero; integer
coordinates outside the original interior are rejected. It adds the same
polynomial to a 3x3 XZ stencil and sets R8 material to 1 there. Color is unsupported.
Position/direction W is ignored. Used XYZ transform operations must remain finite;
the shader and CPU association use separate rounded binary32 multiply/add steps.

## Channels, lifecycle and limits

Scalar is R32_FLOAT. Optional color is RGBA8_UINT; support material is R8_UINT.
Selected channels must exist and be distinct. Inactive atlas slots in selected
outputs become zero. Untouched active interiors and aprons retain exact bits.
Unselected channel resources and revisions remain unchanged. Word-owned material
writes avoid raced byte updates; any allocated tail bytes are zeroed.

`prepare` clears the output on failure and changes no publication. It builds stable
CPU association lists and allocates fresh GPU outputs. Publish its ordinary
`IGVDBGenerationTicket` with `volume->recordAndPublish` on an open same-device
command list, then explicitly submit and synchronize. Tickets are single-use;
stale host/channel/publication state is rejected. Bindings retain source resources
if PointSets or the factory are released before recording. Retained snapshots
continue to expose their original resources.

The budget separately bounds points, actual scalar contributions, logical host
workspace/staging bytes, GPU allocation bytes and buffer count. For a nonempty
volume let V be the full atlas voxel count, P=max(1,actual contributions), L the
leaf pool count including inactive leaves, and T the sum of published topology
buffer byte sizes. The host reservation is `T + 4*L + 16*V + 16*P + 256`, including
copied topology, leaf-slot table, CSR arrays and upload staging. Allocator overhead
and preexisting PointSet/owner/artifact storage are excluded. GPU allocations are
selected channel allocated sizes plus `8*V + 8*P + 256` and 4 bytes per disabled
optional output. Exactly six buffers are allocated. Host reservation is a logical
upper bound over these arrays, not a process-wide heap limit.

Count is also bounded by `UINT32_MAX/16`; atlas cells and pairs must fit the public
32-bit buffer address space. A published empty owner produces a single-use no-op
ticket without bin construction or allocations after descriptor, authentication,
channel, point-count and apron checks. Its unused per-point transformed values
are not evaluated. Empty input on a nonempty owner still copies active bytes and
zeros selected unused slots.

## Validation

`gvdb_nvrhi_deposit_api_*` exercises independent destination-by-destination scalar,
color and material references, signed non-dense leaf placement, unused slots,
physical aprons, duplicate ordering, original-leaf shifted support, finite checks,
empty input/owner, root-only channel31 without apron, exact budget boundaries,
foreign objects, malformed artifact layouts, stale tickets and retained resources.
The installed consumer uses public SDK headers and offline artifacts only.
