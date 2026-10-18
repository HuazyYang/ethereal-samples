# Owned point sets and topology

`GVDBNvrhiPointApi.h` adds `IGVDBPointOps`, `IGVDBPointUploadTicket`, and
`IGVDBPointSet` with new interface identities. Include `GVDBNvrhiPointApi.cmake`
when building the SDK, then use `find_package(gvdbNvrhi 1 CONFIG REQUIRED)` and
link `gvdb::nvrhi_api` from an installed consumer. Static and shared API builds
use the same header. No CUDA, graphics SDK, or Slang compiler is required by the
consumer. The caller supplies its NVRHI device, volume, and offline artifact
compiled from `GVDBPointStreamConversion.slang`, entry `convertPointStream`.
The sample build writes it to
`shaders/gvdb_slang/<dxbc|dxil|spirv|ptx>/GVDBPointStreamConversion_convertPointStream.bin`.

## Explicit phases

1. Fill `PointOpsCreateDesc.volume`, `conversionArtifact`, and
   `conversionArtifactBytes`, then call `createPointOps(desc, &factory)`.
   It retains the volume and device.
2. `preparePoints` validates and deep-copies CPU strided streams and allocates
   conversion/readback resources. Input storage may immediately be destroyed.
3. `recordPoints` records into an open, same-device graphics/compute command
   list. The caller closes, submits, and waits using NVRHI.
4. `finishAfterCallerCompletion` copies the actual converted GPU bytes into an
   immutable `IGVDBPointSet`. One successful finish consumes the upload ticket;
   allocation failure can be retried. The ticket works after factory/volume
   release. No method implicitly submits, waits, or checks completion.
5. `prepareTopology` creates an ordinary `IGVDBGenerationTicket` from that point
   set. The volume's existing `recordAndPublish` is the only publication path.
   Later host/configuration/publication changes make the ticket stale.

Every raw output pointer is cleared before validation. All descriptors require
exact `structSize` and zero reserved fields. Foreign implementations are rejected;
point sets from another factory on the same NVRHI device are accepted, while
upload tickets belong to their creating factory. A point set retains its device,
canonical GPU buffers and CPU bytes; both views remain valid after owner release.
The borrowed GPU buffers are read-only: writing them violates the immutable
CPU/GPU association contract. Arbitrary external GPU streams are not adopted;
a future explicit readback/import protocol can prove that association.

## Data and conversion

Position and optional velocity streams accept three or four components; colors
require four. Supported input encodings are Float32, Float64, UInt8, and UInt16.
Streams may use unaligned byte offsets and arbitrary nonoverlapping element
strides. The final element range and padded raw-buffer bounds are checked without
overflow. Optional streams have zero elements or exactly the position count.
An empty point set needs no data pointers, records no GPU work, has only the
Position attribute bit, null buffers, empty bytes and zero bounds.

Canonical position and velocity components are Float32. Their original component
counts and element order are preserved, including W. Conversion first rounds
Float64 to binary32, then applies the established component-wise binary32 equation
`(minimum + value * delta) * scale + translation`. The identity path preserves
signed zero and subnormals; NaNs follow the converter's canonical quiet-NaN policy.
All transform fields must be finite. Finished position XYZ must be finite;
position W and velocity components may contain nonfinite values. Bounds are exact
converted XYZ extrema with no legacy positive clamp or artificial padding.

Colors become four UInt8 components in **byte storage units**. There is no
implicit normalization: for normalized source colors set transform scale to
`{255,255,255,255}` explicitly. Integer conversion clamps and truncates; nonfinite
values map to zero. Positions, velocity and colors stay associated by input index.

`maxInputBytes` counts copied source prefixes/gaps through the last element,
separately for each supplied stream; generated zero tail padding is excluded.
`maxAllocatedBytes` and `maxBuffers` cover GPU resources for the requested phase,
including padded source/destination/readback buffers and full constant buffers.
Topology CPU storage is limited by the validated point count, activation budget,
and configured host pool limits. Preparation failure never changes the volume.

## Topology rules and compatibility

`FullRebuild` replaces the complete active leaf set and advances the host epoch.
`Accumulate` preserves existing node indices and epoch, can grow the root and
append nodes, and explicitly reactivates required inactive leaves. It requires
both `preserveMetadata=1` and `PlacementDesc.preserve=1`; it retains all existing
leaves. Empty rebuild clears the tree, while empty accumulation keeps it. An
unchanged accumulation may still publish new revisions/resources; it is not an
identity-preserving no-op. Rebuild metadata retention is explicit and old retained
snapshots stay coherent regardless of later publication.

`minDepth` is the minimum **root level**, with a leaf root at level zero. It is
not the number of tree levels. It must be within the configured level count;
support may grow the root further. Empty trees do not gain a synthetic root.

Every point activates its containing brick, using mathematical floor for signed
coordinates. For full rebuild with radius below one, that is the entire support.
For radius at least one, full rebuild additionally enumerates the inclusive brick
box with integer endpoints `trunc(float(trunc(p)) - radius)` and
`trunc(float(trunc(p)) + 2*radius)`. Accumulation enumerates additional support for
all radii, with the symmetric upper endpoint `trunc(float(trunc(p)) + radius)`.
Endpoint arithmetic is binary32 and both integer conversions truncate toward zero,
matching the valid positive legacy path. The endpoint-to-brick conversion uses
mathematical floor for signed coordinates. The actual containing brick is always
unioned with that box: a negative fractional position near zero cannot lose its
own brick when endpoint truncation reaches zero. This is an explicit extension
of legacy kernels that skipped all negative positions.

All centers are visited first, followed by each support box in x/y/z nested order.
`maxActivationCandidates` counts every center visit plus every enumerated support
cell **before deduplication**, including repeated points and overlaps. For one
point in a two-by-two-by-two support box the budget is nine visits. Coordinate,
endpoint, multiplication and budget overflow are rejected before a staged host is
edited. GPU publication also enforces the owner's representable coordinate domain.

The implementation corrects legacy existing-only inactive reactivation, empty
rebuild, root promotion and signed-crossing limitations. It does not promise
legacy node-byte identity for those formerly invalid cases and does not implement
point gathering, sorting or a FLIP solver through this factory.

## Float support coverage and descriptor compatibility

`PointTopologyDesc` now appends `coverage` and a zero reserved field, making the
current descriptor 40 bytes. The original 32-byte prefix is unchanged. A caller
with `structSize=32` retains Legacy behavior; preparation copies only that prefix
and never reads an absent tail. Current 40-byte requests validate the new tail.
The interface IID and vtable are unchanged.

Opt in with `PointSupportCoverage::FloatSupport` when topology must cover the
same continuous support box used by subcell gathering. For each position axis,
form binary32 `p-radius` and `(p+1.0f)+radius`, then floor both endpoints onto the
leaf grid and activate every inclusive leaf origin. This applies to both rebuild
and accumulation, including radius zero. The actual containing leaf is still
included. Centers are activated first, followed by the existing X/Y/Z support
order, and the activation-candidate budget counts center plus support visits
before deduplication.

A full rebuild uses the zero-anchored leaf grid. Accumulation follows the current
root's phase modulo leaf width, preserving valid shifted trees and existing IDs.
For example, on width32, radius2 and phase0, p=29.75 includes leaf32 and p=-30.75
includes leaf-64. Legacy truncation-based coverage remains the default. Invalid
or unrepresentable endpoints and exhausted CPU/GPU budgets fail without changing
the host or published generation. FloatSupport changes topology coverage only;
it does not gather values, translate points, or change minimum-depth semantics.


FloatSupport plans one compatible root enclosure from the complete incoming
center/support bounds before activating leaves. This prevents multi-axis signed
support from exhausting root levels solely because of point order. The enclosure
retains the whole prior root, existing node IDs/origins and leaf phase, and the
centers-before-support leaf allocation order remains unchanged. Newly allocated
interior-node ordering can differ from sequential promotion. Default Legacy
coverage keeps its original promotion path. An already published maximum-level
root cannot absorb out-of-extent points without changing its existing topology;
that append still returns unsupported. Empty point batches create no root.
