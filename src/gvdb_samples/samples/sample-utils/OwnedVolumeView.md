# Snapshot voxel and section viewing

`OwnedVolumeView` records voxel, perspective section, and orthographic section
rendering from a retained public snapshot. It reads the canonical R32_FLOAT raw
channel and topology buffers directly. The snapshot and all recorded bindings
remain alive through GPU execution; no duplicate atlas or packed CPU topology is
created. `ComputeRuntime` owns command submission and synchronization.

`renderOwnedVolumeView` creates a color/depth output. `drawOwnedVolumeSection`
writes only a validated rectangle in an existing same-device output, allowing a
section inset after scene overlays. The section height is a finite world/index Y
coordinate. Scalar sampling, clipping, light, transfer and compositing equations
are shared with the retained typed `VolumeView.slang`. Empty/all-inactive
publications write the requested background and positive-infinity depth; an empty
inset changes only its rectangle.

Artifacts have an 8x8x1 group, no entry uniforms, exactly nine global resources,
224-byte ray constants at b0 and 64-byte view settings at b1. The helper validates
resource kinds, slots, strides, counts, spaces, constant members and parents before
output allocation. It also checks channel storage, topology ranges, dispatch and
32-bit addressing limits, rectangle bounds and existing output strides/capacity.
Production artifacts, including the standard raw density raycasters, are generated
beside the viewer's other kernels in `shaders/gvdb_slang/<backend>` and are built
independently of `BUILD_TESTING`.

A `VolumeScene` may override `ownedSnapshot()` and optionally
`ownedScalarChannel()`. The viewer retains one snapshot for the whole draw and
uses its metadata for framing, voxel/section modes and the inset. Volume/surface
custom hooks remain available for application shading, and otherwise use standard
raw density rendering. Colored level sets or nonzero sparse surface backgrounds
require a custom renderer such as `OwnedVolumeSurface`. Scene topology overlays
still come from `topologyOverlay`; a snapshot-only scene must implement that hook
when topology display is requested. Returning true with no lines is valid.

The default snapshot hook is empty, preserving existing legacy scene behavior.
Public NVRHI and GVDB interface IIDs/vtables are unchanged; these are app helpers.
