# CPU topology import

`GVDBHostImport.h` converts `PackedHostTopology` or portable `PackedVolume` pools into a new mutable `HostVolume`. Link `gvdb::host_import` after including `GVDBHostImport.cmake`. No device, graphics SDK, CUDA SDK, shader compiler, or sample runtime is required.

The import preserves allocation order, node IDs, activity flags, parent/child links, priorities, ranges and mask bytes. It accepts padded node/child records and physically reordered level pools, normalizing them to the fixed host records. Atlas coordinates are reset to `{-1,-1,-1}` and active bounds are recomputed. The imported IDs belong to the new owner and its epoch.

This function imports topology only: atlas maps, channel bytes, transforms and other VBX metadata are neither imported nor validated. Use the existing standalone VBX parser first for file validation; a coherent GPU volume/serializer transaction is separate work. Do not interpret successful host import as successful atlas publication.

Before dereferencing serialized bytes, the importer checks level configuration, extents, count/stride/range arithmetic, alignment, and non-overlapping physical pools. It allocates a temporary owner, copies records, then validates reciprocal graph links and geometry before replacing the output. Failure preserves the caller's existing owner, bytes and revisions. Empty configured and allocated-but-inactive topologies are supported. Nonempty pools without a valid root, unreachable/orphan nodes, conflicting child ownership, bad parent links and invalid geometry fail explicitly.

The CPU regression checks canonical and padded/reordered pools, signed positions, exact IDs and node attributes, empty/inactive graphs, malformed offsets/strides/links, allocation failure and the real explosion VBX asset (10,284 active leaves). It also checks topology produced by the portable builder. Aggregate Release and standalone AddressSanitizer executions are recorded in the root project status.
