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
