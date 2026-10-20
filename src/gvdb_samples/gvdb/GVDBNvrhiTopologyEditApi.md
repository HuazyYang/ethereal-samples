# GPU topology editing and validated fetch

The additive `GVDBNvrhiTopologyEditApi.h` factory creates private writable copies
of a current publication's node/child pools. GPU edits cannot mutate retained
publications. The declared pool counts, strides, level metadata and root ID stay
fixed; growth or compaction requires a separate topology builder.

1. Finish and publish the host topology, then `prepare` an edit ticket.
2. `recordBegin` copies the source pools into private same-device buffers.
3. Get Nodes/Children bindings and record Slang edits with normal command-list
   ordering. Levels is a read-only retained metadata binding.
4. `recordFetch` copies the complete declared ranges into readback buffers.
5. Submit and wait using NVRHI, then call `finishAfterCallerCompletion`.
6. Record/publish the returned ordinary generation ticket. This atomically
   replaces host topology, advances its identity epoch, recomputes bounds and
   remaps compatible channel interiors by signed leaf origin.

The live owner remains authoritative until publication. Any intervening host
edit, channel configuration change or publication invalidates the candidate;
stale device bytes cannot overwrite newer host work. Source host topology must
match its publication. Factory, ticket, device and recording context identities
are checked. Tickets retain source resources and the owner independently.

Fetch validates actual GPU bytes using the CPU topology importer, including
reciprocal parent/child links, ranges, geometry, flags and root connectivity.
Atlas coordinates in device node records are discarded and rebuilt. Every
allocated node, including inactive nodes, must remain connected: raw delinked
orphan pools are rejected until their builder reconnects or compacts them.
Invalid data never changes the owner or its current generation.

All stages require caller submission/completion; none submit or wait internally.
Do not modify private buffers after recording fetch. A successful finish consumes
the ticket. Failure clears its output; allocation failures can be retried.

`maxReadbackBytes` bounds the logical node+child payload. `maxAllocatedBytes`
bounds private GPU copies plus staging (twice that payload), with at most four
buffers. Retained source resources are excluded. The authenticated baseline CPU
clone and importer use host pool limits; this readback budget is not a total host
RSS limit. Publication has separate byte/buffer limits and uses existing volume
placement/preservation rules. There is no public vtable change to existing APIs.

The additive `IGVDBTopologyActivity` factory composes the five offline
`GVDBTopology` artifacts listed in `TopologyActivityCreateDesc`. Between an edit
ticket's begin and fetch, `record` consumes one structured uint marker per
allocated leaf. Zero deactivates; any nonzero marker activates. It delinks inactive
leaves and derives internal activity bottom-up, clears child lists, then links
every allocated node top-down, including inactive nodes. This preserves the
owner's connected-pool invariant and permits later reactivation. It does not
allocate new nodes, change coordinates, or compact disconnected pools. The source
geometry and counts must remain intact before this operation.

Markers must belong to the factory's device; the caller owns that contract because
NVRHI buffers do not expose device identity. Marker offsets are four-byte aligned
and are applied as shader indices, avoiding Vulkan storage-view alignment limits.
The marker range must contain exactly the source's allocated leaf count. Every
dispatch is bounded to 65,535 groups of 256 threads; unsupported larger ranges
fail before recording commands. Geometry uses integer coordinates; public owner
publication retains its existing coordinate-domain checks.

The activity request has a separate allocation budget. Empty topology uses no
buffers. A leaf root needs 264 bytes in two buffers (256-byte constants plus an
unused eight-byte child binding). For a nonempty connected hierarchy with root
level R>0, one buffer holds `256 * (2 + 3*R)` bytes of constants. Each dispatch has
a distinct immutable range. Resource bindings, pipelines and driver allocation
overhead are outside this logical byte budget. Invalid arguments, budgets and
stale revisions are rejected before command recording; explicit UAV barriers
order all levels. Activity can be recorded repeatedly before fetch. Publication
still advances the epoch, so callers must reacquire IDs under the new revision.
