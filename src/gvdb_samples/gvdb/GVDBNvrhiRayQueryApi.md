# Public retained ray queries

Link `gvdb::nvrhi_api`, include `GVDBNvrhiRayQueryApi.h`, and pass the selected
backend's offline `GVDBRayRecords_queryRayRecords` artifact to `createRayQuery`.
The operator retains its NVRHI device and accepts authenticated snapshots with a
selected R32_FLOAT channel. It requires no compiler, native SDK or Donut runtime.

`prepare` copies `RayRecord` inputs and retains the immutable publication. Each
64-byte record matches the retained `ScnRay` layout: float3 hit/normal/origin/
direction at byte offsets 0/12/24/36, then color, point node/index and explicit tail
padding. Input memory can be released or modified immediately after preparation.
The output always preserves bytes 24 through 63. A hit replaces hit and normal;
a miss replaces hit with `(1e10,1e10,1e10)` and preserves the original normal.

Call `record` once on an open same-device graphics/compute command list, submit and
establish completion explicitly, then call `finishAfterCallerCompletion` once.
Before completion, `getOutputBuffer` exposes retained GPU outputs for ordered
consumers. Afterwards `getValues` exposes immutable host bytes until ticket release.
Queries never publish a volume, submit commands or wait. Tickets retain pipelines,
inputs and snapshots after operator/owner release. Failed getters preserve outputs;
failed preparation clears the ticket output. Foreign tickets and snapshots fail.

`Records` is a raw buffer of 64-byte elements. Separate structured Float4 `Hits`
and `Normals` outputs support GPU consumers. Hit W is 1 for a hit, 0 for a miss or
invalid ray, and -1 for traversal exhaustion. Normals W is zero; miss/limit normals
are zero in this separate output. Exhaustion writes sentinel hits and preserves
record normals, but must be handled through status rather than treated as a miss.

The traversal shares `GVDBRayQueryTrace.slang` with the existing arbitrary-ray
renderer. It retains absolute step wavefronts, hierarchy-entry epsilon, trilinear
stored-apron sampling with atlas clamp, and negative scalar gradients. It does not
normalize directions: step/epsilon/bias use the supplied direction's parameter
units. Bias subtracts direction times bias from hit position only. Nonfinite or
zero directions and nonfinite origins yield misses. Empty snapshots yield misses
without reading absent topology. Input count zero creates no GPU buffers.

Compared with retained CUDA, explicit iteration status and geometric DDA repair
undefined/nonterminating boundary cases; a flat gradient returns zero. Pathological
legacy behavior is not an equivalence target. No automatic apron refresh is done.

Nonempty preparation budgets account for eight buffers and `384 + 256*count`
logical GPU/readback bytes, plus `160*count` host bytes for copied records and
completed results. Individual buffer addressing limits bound the ray count.
The public ABI has its own new IIDs and factory; existing interfaces are unchanged.

`RayQueryApiFixture.h` runs against integrated four-backend and installed static/
shared consumers. It checks 259-ray tails, signed hit/miss/normal/bias references,
unnormalized directions, empty and exhausted queries, exact untouched record bytes,
GPU/CPU coherence, exact budgets, and owner/operator/source release. The integrated
driver additionally rejects malformed artifact layouts. Spray uses the public
query for each wand step and feeds its retained hit buffer into deposition.
