# Queryable NVRHI GVDB API

`GVDBNvrhiApi.h` exposes the CPU topology and GPU volume owner through NVRHI-style `IObject` interfaces, `QueryInterface`, `AddRef`/`Release`, and `AutoPtr` handles. It is a C++17 API with C-linkage factory symbols, not a C-language header. Namespace `gvdb::portable` and API version 1 define the supported GVDB volume contract. No device wrapper or native CUDA pointer is accepted or cast to an NVRHI object.

The implementation composes `HostVolume` and `NvrhiVolume`. Every exposed operation has an implementation. Operator factories, GPU-authored topology fetch and serialization are later additions; they have no placeholder vtable entries here. The header uses public NVRHI and portable data layouts, with no CUDA, OptiX, graphics SDK, Donut or compiler include. Descriptor layouts have explicit 64-bit ABI assertions. Consumers must use a compatible NVRHI/C++ ABI and the package's compile definitions.

## Creation and ownership

Link `gvdb::nvrhi_api`, include `GVDBNvrhiApi.h`, fill `VolumeCreateDesc` with an existing NVRHI device, leaf-first `LevelConfig`, and the backend's offline `GVDBAtlasRepack_relocateAtlas` artifact, then call `createVolume`. The wrapper passes `c_GVDBNvrhiApiVersion` to `gvdbCreateNvrhiVolume`; mismatched versions, descriptor sizes and reserved fields fail before owner creation. The implementation retains the device and copies the artifact. It does not discover devices, compile shaders or impose a shader file path.

Use `LevelConfig::fromBranching(r4,r3,r2,r1,r0,output)` for explicit root-to-leaf legacy dimensions. In particular `r0=5` means a 32-voxel leaf. Direct `logDimension` arrays are leaf-first. The factory returns an owned `IGVDBVolume` reference; querying its `IGVDBHostVolume` or `IRHIObject` ancestors retains the same identity. All mutations are single-threaded; synchronize external callers.

Factory, snapshot getter and public preparation `**` outputs are cleared first, including on failure. Success returns one owned reference. Clearing an output slot does not release any reference previously stored there: pass an empty slot or use `AutoPtr::ReleaseAndGetAddressOf`. Independently retained tickets, snapshots and published generations survive failed preparation. This differs deliberately from the internal C++ owner's `unique_ptr&` preparation overload, which preserves that output object on failure.

## Explicit CPU and recording phases

1. Configure or edit CPU topology through `IGVDBVolume`/`IGVDBHostVolume`, then call `finishTopology`. Pool and node edits use the same validated commit contract as `HostVolume`. Allocation/growth can invalidate borrowed pointers; mutable borrows expire at the next commit and must be reacquired before further edits.
2. Set raw channel descriptors. Every successful `setChannel` starts a fresh incarnation, including identical descriptors. `describePlacement` gives the complete logical atlas layout before upload allocation.
3. `prepareGeneration` copies optional full-atlas uploads and prepares all allocations/bindings. Only active leaf interiors are authoritative. Aprons are regenerated; inactive/new regions use the channel void value. Failed preparation does not change the published generation.
4. Call `recordAndPublish({device,openCommands},ticket)` with a same-device graphics or compute command list. Tickets are one-use, tied to their owner, host/configuration revisions and base publication. Unrelated interface implementations and other-owner tickets fail before recording.
5. Close/submit using NVRHI and establish queue ordering yourself. Publication means commands were recorded, not completed. Submit generations in order and do not discard published work. Recording changes compute state, so rebind it before your next dispatch.

Validation/allocation failure is atomic. Device loss or an exception from a void NVRHI recording call cannot roll back commands already recorded. The API never opens/closes/submits/waits on the caller's command list.

`IGVDBSnapshot` retains a complete immutable binding description. `getTopologyBuffer` exposes node, child, level and atlas-map buffers; `getChannel` exposes the raw channel buffer, storage and revision. Returned resource pointers are borrowed while the snapshot lives; `AddRef` any resource kept independently. GPU contents remain subject to explicit queue ordering. CPU edits and later publications cannot replace the snapshot's bindings.

Readback also has three phases: `prepareReadback(snapshot,mask)`, `recordReadback(context,ticket)`, and `finishAfterCallerCompletion()` after the caller submits and waits. Only this owner's authenticated snapshots are accepted, including retained old ones. A readback retains its device, source generation and staging buffers and remains usable after the volume owner is released. `getChannelData` returns logical bytes owned by that readback object; no `std::vector` or heap allocation ownership crosses the DLL interface. Completion is one-use and cannot infer that the caller actually waited.

## Leaf markers

Apply host-owned leaf markers through the existing topology interface. `setActive(id,
marker != 0)` writes exactly 0 or 1. For the retained `gvdbMarkLeafNode` kernel's
integer-to-byte behavior, acquire `editNode(id)` and assign `node->flags =
uint8_t(marker)`. Every nonzero flag byte is active; values such as 2 and 255 are
preserved in published GPU nodes. Neither operation changes a published snapshot.

A successful mutable borrow dirties topology even without a write. `setActive`
dirties it only when the flag byte changes, including normalization from 255 to 1.
Invalid node IDs fail without dirtying the host. While dirty, preparation and
publication of previously prepared tickets fail. `finishTopology` validates the
graph, recomputes active counts/bounds and advances the topology revision; old
tickets remain stale. Reacquire mutable borrows after finishing. Prepare and record
a new generation explicitly to publish the markers.

Deactivation keeps the allocated node ID and graph links. A newly published atlas
excludes inactive leaves. Reactivation preserves the node ID but initializes its
interior from the channel void value when the preceding publication omitted that
leaf; it does not recover data from older snapshots. Those older snapshots retain
their original flags, interiors and aprons, including after owner destruction.
This also applies when all allocated leaves become inactive and are reactivated.

`tests/LeafMarkerApiFixture.h` checks these transitions, exact serialized GPU node
bytes and every channel byte through the integrated and installed API fixtures.
This host marker workflow does not promise GPU-authored marker ingestion or the
legacy incremental key/link/delink orchestration.

## Build and installed package

Include `GVDBNvrhiApi.cmake` after defining NVRHI. `GVDB_NVRHI_API_SHARED` selects a shared facade (default follows `BUILD_SHARED_LIBS`); the CPU and C++ GPU owners remain private static implementation libraries. `GVDB_NVRHI_BUILD_OWNER_TESTS=OFF` and `GVDB_NVRHI_BUILD_API_TESTS=OFF` omit Donut/shader/test requirements. The production library requires the NVRHI shader-program implementation and a supplied offline artifact.

Enable `GVDB_NVRHI_INSTALL=ON` to install the headers, libraries and `gvdbNvrhi` CMake package. Consumption is:

```cmake
find_package(gvdbNvrhi 1 CONFIG REQUIRED)
target_link_libraries(application PRIVATE gvdb::nvrhi_api)
```

The package finds NVRHI separately and exports `gvdb::host_volume` and `gvdb::nvrhi_volume` for consumers choosing the concrete C++ ownership APIs. No source-worktree paths are embedded in exported targets. Shared-library import definitions propagate through `gvdb::nvrhi_api`. Match the NVRHI toolchain/runtime ABI (the verified installed Windows SDK uses `/MD`, while the main project uses `/MT`). Runtime DLLs must be deployed or available on the process search path.

Offline shader sources are installed under `share/gvdb/shaders`; compile `GVDBAtlasRepack.slang` entry `relocateAtlas` to the selected backend's NSTA artifact before deployment. The library accepts artifact bytes and does not invoke ShaderTool. Static NVRHI packages may independently require the artifact-reader link dependency; that is separate from a compiler/runtime requirement. See the root shader build helpers for the required Vulkan register shifts.

## Validation recorded for this slice

The integrated public fixture passed on D3D11, D3D12, Vulkan and CUDA, plus the CPU ABI fixture (5/5). Separate native graphics validation runs were clean and CUDA Compute Sanitizer reported zero errors. Tests use only the public interfaces: identity/refcount balance, unsupported IIDs, version/size/null/reserved failures, CPU edits, invalid/foreign tickets, output clearing, retained snapshots, changed atlas placement, copied upload lifetime, and two generations/readbacks recorded before submission with owner destruction before execution. Every channel byte is checked against an independent interior/apron pattern.

Isolated static and shared facade builds each passed the driver-free ABI fixture against installed NVRHI with CUDA Toolkit and ShaderTool package discovery disabled. Installed static and relocated shared SDK consumers each passed public ABI and concrete ownership linkage tests. `tests/api-consumer` is the reusable installed-package fixture; its optional `GVDB_API_CONSUMER_CUDA_TEST` also passed for both installed variants, exercising actual creation/QI, publication and exact apron/interior readback after owner destruction across the library boundary. These installed GPU checks used the shared NVRHI CUDA SDK and a prebuilt remap artifact, with CUDA Toolkit and ShaderTool discovery still disabled.

Primary logs: `build/m5-api-tests.log`, `build/m5-api-{dx11,dx12,vk}-validation.log`, `build/m5-api-memcheck.log`; isolated build/install/consumer logs are under `build/m5-contract`. These checks cover the additive owner/facade slice; they do not claim that every legacy GVDB operator or serializer has migrated.
