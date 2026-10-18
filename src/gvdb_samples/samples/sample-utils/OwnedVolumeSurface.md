# Snapshot-backed colored level sets

`OwnedVolumeSurface` is an app helper over `IGVDBSnapshot`. It retains the device,
compiled program and private command list, while each draw obtains scalar, color,
topology and placement from the single snapshot passed to `render`. It does not
create a second volume, copy the atlas or cache channel bindings between draws.
A caller that retains an earlier snapshot can continue drawing that publication
after later topology/channel changes or owner destruction.

The scalar channel must be `R32_FLOAT`; optional color is `RGBA8_UINT` from the
same publication. `UINT32_MAX` selects transfer-function color. Missing sparse
cells use the explicit finite scalar background, independently of stored apron
bytes. Color is point sampled at the hit and divided by 255. The gradient points
toward increasing scalar values; zero or nonfinite gradients use the established
ray-facing fallback. Camera clipping, refinement, lighting and transfer equations
are shared with `VolumeSurface.slang`. The raw variant changes only scalar loads
and the empty-publication early return. Empty/all-inactive publications produce
the requested background and positive-infinity depth without fabricating a leaf.

The artifact ABI is checked before pipeline creation: compute group 8x8x1,
240-byte constants in fifteen 16-byte fields, and exactly nine resources with
fixed logical slots, strides, spaces and counts. Channel extents, raw ranges,
level stride, dispatch axes and output/transfer address ranges are validated
before draw allocations. This helper accepts public snapshots, not arbitrary
external atlas buffers.

The helper records and submits on the graphics queue without waiting. Flush or
close any other immediate command list before calling it. Later work on that
queue is ordered normally; cross-queue synchronization remains the caller's
responsibility. The submitted bindings retain all buffers even if the snapshot,
owner or renderer is released before completion. This is a single-threaded app
helper, not a queue or transaction abstraction.

A snapshot-only `VolumeScene` overrides `volumeInfo`, `renderCustom` and optionally
`topologyOverlay`. Metadata, overlays and rendering must describe the same
retained publication throughout one viewer call. `topologyOverlay` returning true
with no lines explicitly represents empty topology. Screen overlays need only
the rendered image, so no legacy `SlangVolume`/`PackedVolume` is required. Legacy
voxel/section/inset fallback still requires `volume()`; unsupported requests fail
explicitly. Existing scenes retain their default legacy behavior.

Include `OwnedVolumeSurface.cmake` for `gvdb_owned_surface`; its offline graphics
and enabled CUDA artifacts are generated independently of `BUILD_TESTING`.
When the viewer and typed surface targets are available, `gvdb_owned_surface_tests`
checks bit-identical raw/typed output plus independent
signed/shifted, adjacent/hole, nonbinary color, zero-gradient, clip-plane and
empty-publication cases, snapshot-only overlays, malformed ABI rejection,
dispatch limits, and lifetime after owner/renderer release.
