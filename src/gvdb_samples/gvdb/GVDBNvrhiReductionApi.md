# Retained-snapshot scalar reduction

Include `GVDBNvrhiReductionApi.cmake` while building the SDK; installed clients
include `GVDBNvrhiReductionApi.h`, find `gvdbNvrhi 1 CONFIG`, and link
`gvdb::nvrhi_api`. The optional concrete C++ target is `gvdb::nvrhi_reduction`.
Both static and shared builds support the same public interfaces. The caller
supplies an NVRHI device/volume and an offline artifact compiled from
`GVDBAtlasReduction.slang`, entry `reduceActiveColumns`. Sample artifacts reside
at `shaders/gvdb_reduction/<dxbc|dxil|spirv|ptx>/GVDBAtlasReduction_reduceActiveColumns.bin`.
Runtime consumers require no graphics/CUDA SDK or Slang compiler discovery.

Create `IGVDBReduction` with `createReduction`, retain an `IGVDBSnapshot` from
that volume, and call `prepare(snapshot, channel, budget, &ticket)`. Only
R32_FLOAT channels are supported. The reduction sums every active brick's
interior, excluding aprons and unused atlas slots. It also returns the exact
number of included voxels. Empty snapshots produce sum/count zero without GPU
allocation. Old retained snapshots are accepted; foreign volume/snapshot
implementations are rejected. Preparation never modifies or republishes a volume.

Call `record(context, ticket)` once on an open same-device graphics/compute
command list. Rebind compute state before unrelated commands. Close, submit and
wait using NVRHI, then call `ticket->finishAfterCallerCompletion(value)`. There
are no hidden waits/submissions or readiness guesses. Tickets retain resources
and can finish after operator, snapshot and volume release. They belong to their
creating operator and cannot be recorded through another operator. One successful
finish consumes the ticket; failures preserve the value output and permit retry.
Raw pointer outputs are always cleared before validation.

The numerical order is explicit: a GPU thread accumulates one packed XY column
in float, skipping unallocated slots while visiting Z bricks/interiors in order.
The CPU then accumulates the completed column sums in double and converts once
to the public float result. This is not an exact or compensated sum; rounding,
NaN and infinity follow those operations. The selected snapshot's immutable
placement controls traversal. Borrowed snapshot buffers are read-only under the
immutable-generation contract: synchronization alone does not make caller writes
compatible with retained snapshots. Owned operators publish new storage and never
modify retained generation storage.

`ReductionPreparationBudget` limits GPU buffer bytes and count, including the
padded constant buffer and column/readback storage. Invalid descriptors clear
outputs; unsupported formats and ABI mismatches fail explicitly. This query does
not expose unimplemented min/max/histogram operators or change existing volume
or voxel-operation interface identities.
