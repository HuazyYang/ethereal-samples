# Obstacle-volume mapping

`createObstacle` creates an additive device operation from the
`GVDBObstacleMapping.slang:mapObstacleSubcells` artifact. It accepts authentic
published snapshots from this SDK on the same device, including retained older
snapshots and different owners. Channels are not required.

For each active source leaf, the integer source origin is looked up in the
obstacle tree. An active leaf containing it within signed half-open bounds yields
its obstacle leaf pool index. That ID repeats in every subcell of the source
brick. Missing/inactive/unmapped entries are `UINT32_MAX`. No coordinate transform,
intersection of two bricks, or separate lookup at each subcell center occurs.
This preserves `gvdbMapExtraGVDB`'s intended repeated-brick association, while
repairing its skipped root source and unchecked root/outside/boundary accesses.

`subcellWidth` must be a power of two dividing the source leaf width. Each output
brick contains `(leafWidth/subcellWidth)^3` contiguous X-fast uint32 entries.
Null mapping uses source leaf-pool order. A nonnull mapping has exactly one entry
per source pool slot, each either a unique output brick ordinal or `UINT32_MAX`.
Slots for inactive source leaves still participate in mapping validation. Holes
are initialized to missing; exclusions permit a zero-brick output.

Preparation copies the mapping and retains both generations, validates input and
budgets, and allocates all resources. It records no work. Record once into an
open same-device graphics/compute list; the operator identity must match. The
borrowed structured GPU output is then accessible, but the caller must submit and
synchronize before consuming it. Finish once after caller completion to obtain
immutable borrowed CPU uint32 values; repeated `getValues` is supported. Failure
preserves borrowed outputs; raw factory/preparation outputs are cleared. No
operation changes an owner's visible snapshot, or infers readiness from revision.

For B output bricks and N output indices, checked host array bytes are `4B+4N`.
If N is nonzero, six NVRHI buffers total `384+4B+8N` descriptor bytes: constants256,
inverse map4B, output4N, readback4N, raw fallback64 and level fallback64. Budgets
exclude retained snapshot allocations, pipeline objects, allocator overhead and
stack locals. Zero output allocates no GPU buffers. N must fit `UINT32_MAX/4`;
subcells per brick must fit uint32. Generic owner publication coordinate limits
still apply. Shader artifact validation checks entry, group size, resource slots,
strides, scopes and constant-member offsets before pipeline creation.

The snapshot's GPU contents follow the ordinary owner/caller ordering contract;
writing into its borrowed topology allocations violates that contract. A result
ID belongs to the retained obstacle snapshot, never to a later owner's topology.
