# Retained-snapshot subcell expansion

`GVDBNvrhiSubcellApi.h` is an additive device-only
operator using `expandSubcellPoints` from `GVDBPointSubcells.slang`. Prepare retains
authenticated same-device snapshot/points and builds stable host associations.
Record writes cell ranges/source ordinals and expands canonical point attributes
on a caller-owned graphics/compute list. Submit/wait explicitly, then finish once
to obtain the actual GPU-expanded PointSet. No volume publication is performed.

Cell order is leaf-pool index then local z/y/x, x fastest. Inactive slots have zero
counts. Cell locations expose signed origins and source leaf pool indices; inactive
slots have zero origin and UINT32_MAX. This also distinguishes active empty cells
from inactive cells. Inclusive support is [translatedPosition-radius,translatedPosition+1+radius]
with signed phase-relative floor addressing, shared with owned gather. Output
ordinals within each cell are stable. Float3/Float4 layout and W bits are preserved;
velocity/color are exact copies and positions include translation. Compose public
position and velocity quantizers on the result for packed normalized ushort3 data.

For C cells, P pairs and total present attribute stride S, retained host bytes are
24*C+4*P+P*S. Nonempty GPU bytes are 24*C+256+4*P+2*P*S+4*A, where A is the number of
absent optional attributes (dummy output buffers). Buffer count is 7+K, where K is
the number of present attributes. With no pairs only the 24*C range/location buffers are needed;
with no cells no GPU storage is needed. Snapshot/point input and transient topology
export/hash storage are excluded. Cell, clipped candidate-visit and pair budgets
bound association work. Point pairs are limited to UINT32_MAX/16, cells to
UINT32_MAX/16 and subcells per leaf to UINT32_MAX. Getters return borrowed storage
for the ticket lifetime; buffer mirrors require record and caller synchronization.

## Compatibility and ownership

This is a public storage contract, not the positional CUDA ABI. Legacy atomic
insertion order becomes stable input order. The API uses fixed source leaf-pool
cell order rather than arbitrary `sc_mapping` permutations. Consumers may use the
returned source leaf indices and origins to build their own view. Signed floor
coverage replaces the old negative-position rejection/truncation; active root
leaves are included instead of being rejected because their parent is undefined.
No-hit points outside active support contribute no pairs. Empty cells and inactive
slots are deterministic; no uninitialized output is exposed.

Preparation borrows neither mutable host topology nor caller point arrays. It
retains immutable snapshot and canonical PointSet ownership; later publication or
release of the original volume does not change the ticket. Metadata is available
on the CPU after prepare. Its raw GPU mirrors become available after record and
require caller ordering/completion before use. Finish is allowed once after
caller completion; failures clear interface output and allow retry. The output
PointSet survives release of the ticket and all source owners. Calls on one ticket
must be serialized. Existing point conversion may transform source float bits;
this operator preserves the resulting canonical velocity/color/W bytes exactly.

Expanded code storage is composed explicitly: use `createPointQuantize` for
positions, then `createVelocityQuantize` when velocity is present. Code order is
exactly the returned pair order; neither operator recomputes cell association.
Retain their tickets for code-buffer lifetime. This supports both legacy float
and normalized-ushort3 attribute-storage roles without native SDK types.

## Point-to-leaf insertion extension

`GVDBNvrhiPointInsertionApi.h` and `createPointInsertion` use the same creation
descriptor/artifact and ticket. Include `GVDBNvrhiPointInsertionApi.cmake` to enable
its tests when composing the library. The factory also supports QueryInterface
between IGVDBPointInsertion and IGVDBSubcells with one canonical IObject identity.
Existing subcell descriptors, behavior and vtables are unchanged.

Center mode associates p+translation with exactly one active leaf. VelocitySupport
mode associates (p+translation)+velocity*supportOffset, requiring the canonical
velocity attribute; negative offsets are supported. Sorted positions contain only
p+translation, and original velocity/color/W bits remain associated. The legacy
pre-translation z==1e10 sentinel is excluded. Direction XYZ and transformed values
must be finite; absent velocity returns FE_NOT_FOUND, nonfinite math FE_INVALID_ARGS.
An empty initial upload has no optional velocity, so use Center for that empty set.

One cell per leaf-pool slot exposes offsets/counts, locations and original ordinals.
For each range, ordinal[j] maps to that leaf and local offset j-range.offset; this
replaces pnode/poff/gcnt/goff buffer plumbing. Stable input order replaces atomic
insertion order, and inactive slots remain empty. The candidate budget counts all
input points once, including missing/sentinel points and empty topology. Other
logical budgets and recording/completion lifetimes are identical to subcells.
Host lookup is O(N log L) over immutable retained metadata; GPU expansion uses the
existing Slang artifact. Temporary lookup/topology storage is excluded from the
retained-byte budget. Broader performance/platform acceptance remains separate.
