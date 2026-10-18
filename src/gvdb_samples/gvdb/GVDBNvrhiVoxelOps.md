# NVRHI voxel preparation (M5 Step D, initial operations)

`GVDBNvrhiVoxelOps.h` provides fill, clear, same-format channel copy, and apron reconstruction over `NvrhiVolume`. Include `GVDBNvrhiVoxelOps.cmake` and link `gvdb::nvrhi_voxel_ops`. Supply the backend's offline NSTA artifact for `GVDBAtlasMutation.slang` entry `mutateAtlas`. The runtime library needs NVRHI and the host/owner libraries only; no Donut, shader compiler, CUDA SDK or sample runtime is included. GPU test targets separately use Donut for device creation.

The concrete C++ operator borrows a stable owner that must outlive it. It retains the device, program and pipeline, but does not cache a generation when created. Every preparation acquires the current owner snapshot and checks that finished host topology and channel incarnations match that publication. The public queryable wrapper retains its volume for this lifetime.

## Preparation and publication

All methods prepare an existing `PreparedGeneration`. Pass that ticket to `NvrhiVolume::recordAndPublish` with an open same-device graphics/compute command list. No method opens, closes, submits or waits. Source and destination allocations are distinct; each destination 32-bit word has one shader writer. Binding sets retain both generations' resources through recorded execution.

Only selected channels receive new allocations and resource/content revisions. Topology, atlas map, placement revision, and every unselected channel handle/revision remain identical. CPU placement data is shared immutably between generations rather than copied per dispatch or retained through a chain of old generations. Repack/removal still uses the owner path; an existing operator sees the new publication on its next preparation.

A zero channel mask, empty atlas, same-channel copy, or apron update with apron width zero prepares an unconsumed no-op ticket. Recording still validates owner, current base generation, host/channel state, device and queue, and consumes the ticket exactly once. The current generation object and all revisions remain unchanged. A stale no-op is rejected just like a stale write ticket.

Failed preparation preserves the caller's output ticket and current publication. Budgets include replacement allocations and uploads; partial allocation failures discard the private candidate. Driver loss or failure inside NVRHI's void recording methods cannot roll back recorded commands. The caller must submit publications in order and explicitly synchronize queues, as for ordinary owner generations.

## Operation values

All input fill components must be finite. The retained fill conventions are explicit:

| Format | Nonzero `prepareFill` value |
| --- | --- |
| R8_UINT | `x` in [0,255], truncated to a byte |
| RGBA8_UINT | normalized `xyz` in [0,1], each multiplied by 255 and truncated; alpha 255 |
| R32_FLOAT | `x`; when `z < 0`, the retained sum of three atlas-coordinate sine waves |
| RGB32_FLOAT | `xyz` (portable extension over the legacy unsupported float3 surface write) |
| RGBA32_FLOAT | `xyz`, alpha 1 |

An all-zero float4 selects clear, including alpha zero. A nonzero `w` can therefore request a zero RGB fill with alpha one/255. Out-of-range integer/normalized-color fills are rejected instead of reproducing undefined casts or component spill. Fill writes every voxel of each active brick, including its aprons and coordinate-zero planes. This deliberately corrects the retained CUDA unpacked launch's skipped zero-coordinate planes. Unused slots and physical padding stay zero.

Clear writes zero bits to all selected channel storage; it does not apply the channel's void value. Copy accepts only two configured channels of the same format in the same owner. It preserves source interior/apron bits, including NaN payloads and signed zero, while retaining the destination channel's descriptor. It does not perform conversion or change the destination boundary policy. Updating aprons afterwards applies that destination policy.

Apron reconstruction copies every interior bit and fills faces, edges and corners from current active neighboring interiors using integer topology traversal. Missing cells use all components of the destination `ChannelDesc::voidValue`, in storage units. No source apron is trusted, and unused slots remain zero. The owner coordinate/address limits continue to apply.

## Current scope and evidence

The core fixture independently compares every output byte for five formats, signed leaves, nontrivial vector boundaries, R8 six-byte brick rows, raw NaNs/negative zero, unchanged topology/unselected revisions, highest channel 31, retained snapshots, no-op/stale tickets, partial preparation failures and operator reuse after growth. The sine fill has a separate numerical CPU equation. Recorded generations/readbacks are exercised after operator and owner destruction.

This initial module does not yet implement smooth/grow/noise/cut/expand, custom programs, partial atlas blocks, resampling, reduction, point gather, or voxelization. Those operations must use the same private mutation builder and publication path; this module is not a claim of complete legacy voxel-operator parity. Actual backend/native-validation/sanitizer results are recorded separately when run.
