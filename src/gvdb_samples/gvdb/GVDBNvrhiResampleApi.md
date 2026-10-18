# Owned dense-to-sparse resampling

Include `GVDBNvrhiResampleApi.cmake` to add the public factory to the SDK. Installed
consumers include `GVDBNvrhiResampleApi.h`, find `gvdbNvrhi 1 CONFIG`, and link
`gvdb::nvrhi_api`. The optional concrete C++ target is `gvdb::nvrhi_resample`.
Static and shared API builds use the same header. Supply the NVRHI volume and an
offline `GVDBAtlasResample.slang` artifact, entry `resampleOwnedAtlas`. The sample
build writes `shaders/gvdb_resample/<dxbc|dxil|spirv|ptx>/GVDBAtlasResample_resampleOwnedAtlas.bin`.
Consumer configuration requires no CUDA/graphics SDK or Slang compiler discovery.

Create `IGVDBResample` with `createResample`. Fill `ResampleRequestDesc` with a
selected R32_FLOAT channel and dense X-fastest Float32 source bytes, resolution,
three affine transform rows and a remapping range. The source uses four bytes
per voxel; `sourceBytes` must cover the checked resolution product. Extra trailing
bytes are ignored. `prepare(request, budget, &ticket)` deep-copies the used data
for a nonempty destination, so caller storage may be changed or released before
recording. Empty destinations still validate/authenticate the descriptor and
return an ordinary one-use no-op ticket without reading/uploading unused input.

The destination must have a current, finished publication. The only publication
path is the volume's existing `recordAndPublish(context, ticket)`. Caller command
lists remain explicit: close, submit, and synchronize with NVRHI. Preparation
never records/submits/waits, allocates fresh selected-channel storage, and leaves
the volume unchanged on failure. Old snapshots retain their original buffers;
other channels and topology retain their revisions. Stale or foreign generation
tickets are rejected by the normal owner rules. The operator retains its volume,
and recorded resource lifetime follows the existing generation/command contract.

For each voxel of an active brick, including its apron, the kernel constructs
integer world coordinates from leaf origin plus local coordinate minus apron.
It applies the three affine rows and truncates each component toward zero. Thus
source coordinates strictly between -1 and 0 sample coordinate zero, preserving
the retained resampler's rule. There is no half-voxel offset or trilinear filter.
The kernel deliberately covers coordinate-zero atlas planes that the retained
`GVDB_VOXUNPACKED` launch skipped. Zeroing unused atlas slots is also an explicit
owned-generation policy; these cases do not promise legacy byte identity.
An in-range source value is mapped by
`outMin + (value - inMin) * (outMax - outMin) / (inMax - inMin)`.

Outside source coordinates preserve the exact previous destination bits by
default, including NaN payloads and signed zero. With `preserveOutside=0`, they
use `background.x`. Nonfinite or out-of-int32 transformed coordinates are outside
before integer conversion. Unused atlas slots are always zero. All rows, range
components and background components must be finite; input range width must be
finite/nonzero and output width finite. Source sample values themselves may be
NaN or infinity and follow the stated remap arithmetic without sanitization.

The request is a versioned 128-byte POD with zero reserved fields and an explicit
0/1 `preserveOutside` value. Every raw output clears before validation. GPU byte
and buffer limits cover source upload, new selected-channel storage and constants
through the shared mutation builder. Invalid size/range/format/budget requests
fail before publication. Arbitrary external GPU buffers are not adopted; dense
CPU input gives the factory a provable ownership and association boundary.
