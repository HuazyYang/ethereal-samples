# NVRHI volume ownership (M5 Step C)

`GVDBNvrhiVolume.h` is an additive C++ owner. It composes `HostVolume`, creates canonical raw channel allocations, and publishes retained `VolumeGeneration` objects. The implementation uses public NVRHI and portable GVDB layouts only. It has no Donut, `ComputeRuntime`, compiler, CUDA, OptiX, or graphics SDK include. The separate test executable uses Donut solely to create graphics devices.

Include `GVDBNvrhiVolume.cmake` and link `gvdb::nvrhi_volume`. With `GVDB_NVRHI_BUILD_OWNER_TESTS=OFF`, the module does not compile shaders or require Donut. Supply the selected backend's offline NSTA `GVDBAtlasRepack_relocateAtlas` artifact to `NvrhiVolume::create`. The loader checks target, compute entry, thread-group size, registers, scopes, descriptor counts, constant layout and structured strides before pipeline creation. Artifact data is copied; no filesystem convention is imposed on library callers.

## Recording protocol

1. Create an owner with a retained NVRHI device and configure channels using `setChannel`.
2. Edit `owner.host()`, then explicitly call `finishTopology`.
3. Use `describePlacement` to determine the destination atlas dimensions. Optionally provide full logical atlas uploads in `GenerationRequest`. Input bytes are copied during preparation; only active brick interiors are used. Input apron/unused bytes are ignored.
4. Call `prepareGeneration`. This validates and prepares every resource, input copy, constant and binding without recording commands. It preserves both the output ticket and the published generation on failure.
5. With an open same-device graphics or compute list, call `recordAndPublish({device, commands}, ticket)`. The owner records uploads/remaps and publishes the complete generation. It does not open, close, submit or wait. Subsequent application dispatches must rebind compute state.
6. Submit in publication order using NVRHI and establish explicit cross-queue dependencies. Do not discard recorded work after publication. A generation/content revision describes recording, not completion.

Prepared tickets retain a unique owner identity and the base publication, host revision and channel configuration. Wrong-owner, stale, already-used and wrong-context tickets fail before recording. The device/list identity check also works through NVRHI validation wrappers. This API is single-threaded; synchronize host edits and publication externally.

`prepareReadback` authenticates any retained generation from that owner and prepares readback allocations. `recordReadback` uses a caller list. After the caller submits and waits, static `NvrhiVolume::finishReadbackAfterCompletion` maps the ticket and returns logical channel bytes. Completion needs no live volume owner: the ticket retains the generation, staging allocations and device. It cannot infer that the caller actually waited.

## Placement and storage

The default grid is 16 x 16 x 1 bricks, growing Z as needed for active leaves. Requests may change the grid and apron. Placement modifies an owned packed topology copy; CPU nodes remain unplaced. GPU node bytes and the atlas map in each generation are authoritative for that generation.

Surviving interiors are preserved by signed leaf origin when leaf width and channel incarnation agree, including full CPU graph rebuilds that reorder node IDs. Changing leaf width with preservation enabled fails; `preserve=false` explicitly discards prior contents. Every successful `setChannel` starts a fresh incarnation, including an identical descriptor. Remove/publish/recreate cannot inherit an old allocation or reuse its revision: channel resource/content revisions use the monotonically increasing publication serial.

All five formats are raw buffers: R8_UINT, RGBA8_UINT, R32_FLOAT, RGB32_FLOAT, RGBA32_FLOAT use 1, 4, 4, 12, 16 bytes per voxel. Integer void values are whole bytes; floating void values must be finite. A shader invocation owns one destination 32-bit word and resolves its four bytes separately. This preserves RGB32's third component and R8 words crossing six-byte rows or brick edges without overlapping stores. New topology lookup regenerates apron faces, edges and corners from surviving source interiors. Missing/new cells use `voidValue`; unused slots and physical tail padding are zero. No old apron is copied and no destination thread reads another thread's writes.

Current regular brick grids have even pitch on every axis, so complete R8 atlas lengths are already multiples of eight. The GPU fixture exercises unaligned rows/brick edges; generic odd-length raw storage rounding is separately covered by the descriptor tests. Do not interpret a vacuous complete-atlas padding loop as evidence of an odd-length atlas shape.

Configured-empty and allocated-but-inactive volumes publish zero active leaves and no live channel allocations. They may retain meaningful topology metadata. No remap dispatch runs for an empty atlas.

## Limits and failure behavior

CPU topology supports signed32 coordinates. GPU publication additionally requires every node endpoint and each leaf apron to lie within [-8388607,8388607], preserving the exact voxel-center domain used by existing floating metadata consumers. The remap itself traverses integer coordinates. Pools, atlas voxel indices, maps and raw addresses are checked against their 32-bit addressing limits; actual backend allocation limits may be lower.

The first owner accepts its own allocations and copied CPU uploads only. It does not claim to authenticate arbitrary external `IBuffer` provenance. Partial block uploads, GPU-authored topology fetch, serializers and legacy facade adapters remain later operations.

Preparation uses fresh destination allocations. Validation, allocation, program/binding preparation and publication are failure-atomic; old snapshots remain valid. `PreparationBudget` permits deterministic allocation-failure tests, including failure after channel allocations. NVRHI write/state/dispatch calls return void, so device loss, backend recording failures or exceptions cannot roll back already recorded commands. The owner does not hide a device wait in edits or destruction. Binding sets track resource lifetime, and snapshots retain their device and allocations.

## Independent fixtures

`gvdb_nvrhi_volume_tests` runs on D3D11, D3D12, Vulkan and CUDA through the same owner. It checks five distinguishable byte patterns, signed/boundary coordinates, atlas growth and pitch changes, removal and reordered graphs, independently computed apron values, fresh channels, empty/reuse behavior, malformed artifact ABI, failed preparation, foreign command lists, stale/used tickets, and unsubmitted repacks/readbacks surviving owner destruction. Native validation and CUDA memory checking are separate runs. Build and runtime results are recorded in the project checkpoint, not implied merely by this test list.
