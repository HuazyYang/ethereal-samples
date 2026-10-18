# Per-call apron boundary overrides

Include `GVDBNvrhiApronApi.cmake` to add `IGVDBApronOps` to the SDK. Installed
consumers include `GVDBNvrhiApronApi.h`, find `gvdbNvrhi 1 CONFIG`, and link
`gvdb::nvrhi_api`. Static and shared builds use the same interface. The existing
`IGVDBVoxelOps` vtable/IID and its default `prepareApron` behavior remain intact.
The concrete C++ overload is available through `gvdb::nvrhi_voxel_ops`.

Create the operator with `createApronOps`, the owned volume and the existing
`GVDBAtlasMutation.slang` artifact, entry `mutateAtlas`. No shader change or new
artifact format is required. Sample artifacts are
`shaders/gvdb_voxel/<dxbc|dxil|spirv|ptx>/GVDBAtlasMutation_mutateAtlas.bin`.
Installed consumers need no CUDA/graphics SDK or Slang compiler discovery.

Set `ApronRequestDesc.channelMask` and optionally provide up to32
`ApronBoundaryOverride` records. Each record names a unique selected channel;
all reserved fields must be zero. Values use exactly `ChannelDesc::voidValue`
rules: every Float4 component is finite, R8 X is a whole storage byte in[0,255],
and RGBA8 requires all four whole storage bytes. Float formats retain their
corresponding raw Float32 components, including negative zero; unused components
still must be finite. Overrides do not use the normalized fill convention and
do not silently truncate fractional integer-channel inputs.

`prepare(request, budget, &ticket)` copies the override values and prepares fresh
selected-channel resources. It requires a current finished publication. Existing
interior bytes are preserved exactly, including NaN payloads and signed zero.
Apron faces, edges and corners read active neighboring interiors; only missing
neighbors use the supplied boundary override. Selected channels without an
override use their existing void values. Channel descriptors and topology remain
unchanged, unselected bindings retain their resources/revisions, and old
snapshots keep their original bytes. Unused atlas slots and physical tail padding
are zero. The caller may modify or destroy the override array after preparation.

Publish with the existing `volume->recordAndPublish(context, ticket)`, then close,
submit and synchronize using NVRHI. The factory retains its volume and prepared
commands retain their resources. Preparation never records/submits/waits. Raw
outputs clear before failure; failed validation/allocation does not publish a
partial generation. Tickets follow the existing owner, stale-base and one-use
checks. `ApronPreparationBudget` limits all new GPU bytes and buffers.

Zero mask, empty atlas, and apron width0 return authenticated one-use no-op
tickets. Descriptor, channel and override validation still runs first, including
fractional/out-of-range integer values and nonfinite unused Float4 components.
Valid no-ops allocate nothing and leave all resource/revision identities intact.
A zero mask cannot contain an override because every override must name a
selected channel. With zero override count, the array pointer is ignored.

The concrete overload is
`prepareApron(mask, values, overrideMask, budget, output)`. Its values array is
logically32 Float4 entries; only set bits are read, every override bit must be
selected, and a nonzero override mask requires a nonnull array. It uses the same
channel validator before no-op handling and preserves an existing C++ output on
failure. The original C++ overload delegates with no overrides.
