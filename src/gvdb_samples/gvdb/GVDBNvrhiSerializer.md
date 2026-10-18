# NVRHI GVDB serialization

`GVDBNvrhiSerializer.h` adds queryable `IGVDBSerializer` and `IGVDBSaveTicket` interfaces to the existing version-1 public volume API. They have new IIDs and no legacy `GPDevice` dependency. Include `GVDBNvrhiSerializer.cmake` to add the implementation to `gvdb_nvrhi_api`; installed applications link `gvdb::nvrhi_api` from `find_package(gvdbNvrhi 1 CONFIG REQUIRED)`. The package supports static and shared API libraries. CUDA, graphics SDKs and the Slang compiler are not serializer dependencies.

## Explicit phases

Create a serializer with `SerializerCreateDesc::volume` and `createSerializer`. It retains that volume; `getVolume()` is borrowed.

1. **Load:** `prepareLoadVBX(blob,budget,&ticket,&diagnostic)` parses and deep-copies the input, validates CPU topology, stages exact placement and channels, and prepares all GPU resources. It leaves the current host and published generation unchanged. Call the existing `volume->recordAndPublish(context,ticket)` on an open same-device graphics/compute command list. The caller closes, submits and synchronizes that list.
2. **Save:** obtain a retained `IGVDBSnapshot`, then call `prepareSaveVBX(snapshot,optionalMetadata,budget,&save,&diagnostic)`. Preparation freezes that generation's placed topology, channel order and metadata and allocates staging buffers. `recordSaveReadback(context,save)` records once. After the caller submits and waits, `save->finishAfterCallerCompletion(&blob,&diagnostic)` returns an owned `IDataBlob` containing VBX bytes.

There is no hidden queue submission or completion wait. Publication means recording succeeded, not that the GPU finished. Published command lists must not be discarded. The save ticket retains all resources and CPU data needed for completion after the serializer, snapshot and volume interfaces are released. Successful completion is single-use; a CPU serialization/blob-allocation failure can be retried without recording again. All public raw outputs clear before failure. Output and diagnostic addresses must be distinct; an aliased address returns `FE_INVALID_ARGS` with the slot null. Optional diagnostic blobs contain UTF-8 text, with a terminator supplied by NVRHI's string-blob implementation.

## Fidelity and metadata

Import preserves pool order/IDs, active flags, sparse or reversed atlas slots, exact grid/apron dimensions and every logical channel byte, including apron bytes and unused slots. Float NaN payloads and negative zero are copied as bytes. Only raw-buffer tail padding is zeroed. CPU host adoption keeps the owner object address stable but invalidates pool/node borrows and advances the host epoch; cached node IDs must be paired with that epoch.

`VbxMetadata` stores the five three-component vectors (pre-translation, angles, scale, translation, voxel size) and all 256 grid-name bytes. Names may contain embedded NULs and need not be terminated. Transform components must be finite; voxel sizes must also be positive. The generic owner retains an immutable metadata attachment with the imported generation. Channel mutation and repacking in the same host epoch retain it. Clear/reconfigure drops it for subsequent generations. Retained old snapshots keep their own metadata even after another file is loaded. `getMetadata` returns identity/default metadata for a generation without an attachment and rejects another owner's snapshot. A save override is copied into its ticket and never alters the source snapshot.

Files do not encode per-channel void values; imported descriptors therefore use zero void values. Later apron regeneration uses those descriptors, while an untouched imported generation saves its original apron bytes exactly.

## Supported layouts and failure codes

The CPU codec accepts one uncompressed, non-reused atlas grid in the supported VBX 1.0, 1.11+ and 2.x layouts. All five canonical channel formats are supported. Bitmask input topology is normalized to indexed children. Saving emits canonical VBX 1.11; this preserves represented topology, placement, metadata and raw channels, not the original file encoding.

The serializer supports one to 32 dense channels numbered `0..N-1` and at least one active leaf. Empty snapshots, holey channel masks and more than 32 file channels return `FE_UNSUPPORTED`; channels are never silently renumbered. Malformed input and CPU codec failures return `FE_INVALID_ARGS` with an optional parser diagnostic. The current standalone codec reports failures through a boolean/string API, so allocation exceptions caught inside that codec also reach this category. Allocation failures elsewhere and explicit budget violations return `FE_OUT_OF_MEMORY`; no exception-text classification is used.

`SerializationBudget::maxBlobBytes` limits input bytes on load and the exact canonical serialized size on save. `maxAllocatedBytes` and `maxBuffers` bound upload/staging GPU allocations, including raw padding. They do not cap parser CPU allocations. Save checks its staging and output-size budgets before allocating staging resources. Load performs full validation/resource preparation before returning a ticket. Failures preserve the current volume and any independently retained tickets/snapshots; a host/config/publication change makes a prepared load ticket stale.

## Validation

The aggregate fixture covers all four backends, a signed two-leaf topology with apron 2 and sparse reversed slots in a 3x2x1 grid, all five formats with poisoned unused/apron bytes and NaN/-0 patterns, metadata through mutation/repack/second load, exact byte round trips, real `explosion.vbx`, stale/foreign tickets, failure atomicity, output alias rejection and release before submission/completion. Static/shared package consumers independently exercise save/load and owner-free completion through installed headers and libraries, with CUDA-toolkit and Slang-compiler discovery disabled.

Package logs are `build/m5-contract/serializer-api-{shared,static}-{configure,build,test,install}.log` and `serializer-installed-{shared,static}-{configure,build,test,cuda-test}.log`. Aggregate and native validation records are maintained by the project checkpoint documentation.
