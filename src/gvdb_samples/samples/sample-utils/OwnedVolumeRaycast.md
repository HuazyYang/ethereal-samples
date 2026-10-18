# Rendering a retained public volume snapshot

`raycastOwnedVolume` is an application helper for the existing compute volume and
surface marchers. It takes an `IGVDBSnapshot` and one `R32_FLOAT` channel. It does
not create a second topology, placement map, or atlas. `ComputeRuntime` remains the
sample's command recording and transient output utility; it is not part of the
public volume ownership API.

`render-to-file` loads VBX through `IGVDBSerializer`, records the returned ticket
on its caller-owned command list, submits that list, and explicitly waits for the
upload. It retains a snapshot and releases the owner and serializer before
rendering. The CLI, index-space camera, transfer function, integration/surface
equations, and PNG conversion are unchanged.

## Binding contract

The helper binds the snapshot's exact byte ranges: raw scalar atlas `t0`, raw node
pool `t1`, raw child pool `t2`, and structured 64-byte level records `t3`. Transfer
values are structured float4 at `t4`; output color/depth are `u0`/`u1`. Raw atlas
loads use `asfloat(ByteAddressBuffer.Load(index * 4))`. The wrapper includes the
existing raycast module, so it shares the marchers rather than duplicating them.
A root-only tree receives the runtime's valid dummy child binding. An empty
snapshot returns explicit background color and infinite depth without traversing
null topology buffers.

The snapshot must originate from the same device. Before allocation the helper
checks the channel/ranges, image byte count, 8x8 dispatch dimensions (at most
65,535 groups per axis), transfer byte count (at most 4 GiB), and finite raycast
parameters. A snapshot and its borrowed buffers stay alive while recording;
NVRHI binding-set liveness retains the dispatched resources through execution.
Neither publication nor a revision change means GPU completion. The caller must
order uploads and dispatches, and synchronize cross-queue use explicitly.

## Refreshing after publication

Each call reads one retained snapshot and creates bindings from it; this helper
has no binding cache. Acquire a new snapshot to render a new publication. An old
snapshot continues to render its old topology and channel contents after later
fills, repacks, imports, or owner destruction.

For a future cache, scope revisions by owner identity. Refresh traversal/bounds
when topology changes, atlas addresses and placed node data when placement
changes, and binding ranges/formats/resources when a channel's resource revision
changes. A content revision invalidates derived images or progressive history.
A host epoch change invalidates cached node identities and borrowed host data.
Use all metadata and resources from the same snapshot.

## Build and coverage

`OwnedVolumeRaycast.cmake` creates the production raw-raycast and atlas-remap
artifacts independently of `BUILD_TESTING`. A graphics-only build does not
require CUDA discovery. `OwnedVolumeRaycastTests.cmake` is optional and keeps the
old structured-atlas renderer exclusively as a comparison oracle.

The fixtures check signed coordinates, reversed/non-dense atlas slots, analytic
Beer attenuation and surface crossing, empty/missing regions, generation refresh,
repacking, invalid dimensions and formats, malformed-load preservation, and owner
release before recorded work is submitted. The actual `explosion.vbx` volume and
surface images are compared with the previous portable implementation on D3D11,
D3D12, Vulkan, and CUDA.
