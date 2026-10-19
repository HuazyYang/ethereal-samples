# Public normalized-ushort point quantization

`GVDBNvrhiPointQuantizeApi.h` adds a device-only operator without changing an
existing interface or descriptor. Supply the offline `encodePointU16` and
`decodePointU16` artifacts from `GVDBPointQuantize.slang`. The SDK does not compile
shaders or expose CUDA, graphics SDK, or Slang compiler types to consumers.

## Sequence and ownership

1. Obtain a completed public PointSet from point conversion or quantization.
2. Create the quantizer and prepare a ticket. Input must be an authenticated
   PointSet from this API binary on the same NVRHI device. Preparation retains
   its original ordinal order and never submits, reads GPU data, or changes topology.
3. Record once using the originating quantizer on a caller-owned open same-device
   graphics/compute command list. Copy queues are rejected. Submit and synchronize
   explicitly; no method submits or waits on the caller's behalf.
4. Call `finishAfterCallerCompletion` once successfully to read the actual GPU
   result and obtain an authenticated immutable PointSet. Failure clears the
   interface output and permits retry. Position Float3/Float4 layout and W bits
   remain unchanged; velocity and color bytes/buffers retain their associations.
   The source factory, source PointSet and quantizer can be released before completion.

`getCodes` returns six bytes per point as little-endian ushort3 after completion,
borrowed for the ticket lifetime. `getCodeBuffer` is available after recording,
includes zero padding to four bytes, and requires caller synchronization or
same-queue ordering. Empty input produces an empty completed PointSet and CPU
code range; no GPU buffers are allocated and `getCodeBuffer` returns `FE_NOT_FOUND`.
Failed borrowed-data getters leave their output unchanged. Retain the ticket or
acquire a buffer reference for continued GPU access. Calls on a ticket must be serialized.

## Arithmetic and bounds

For each XYZ component, encode `uint(saturate((position-minimum)/range)*65535)`;
decode `(float(code)/65535)*range+minimum`. This is normalized unsigned 16-bit
encoding, not IEEE half. Native binary32 operations can place a value on either
side of a code boundary. Portable validation permits one code interval and
compares decoded positions against the actual GPU codes. There is no CPU result
substitution. Decoded finite XYZ extrema are recomputed for the returned PointSet.

Explicit bounds clamp out-of-range values. All descriptor float components must
be finite. XYZ ranges must be at least `16*FLT_MIN`, and each
`abs(minimum)+range` must not exceed `FLT_MAX/2`. Unsupported numeric domains
return `FE_UNSUPPORTED`. `LegacyPadded` derives bounds from canonical converted
XYZ extrema, requiring them within +/-4,000,000. It uses minimum-minus-one
(clamped to zero for nonnegative minima) and maximum-plus-two; an empty set uses
minimum zero/range one. Descriptor W is ignored by arithmetic; source W is copied
bit for bit, including signed zero and NaN payloads.

## Budgets and failure behavior

For N nonempty points and position stride S (12 or 16), packed allocation P is
`alignUp(6*N,4)`. New GPU bytes are `256+2*(P+N*S)` in five buffers. New retained
host data is `6*N+N*S` plus copied canonical velocity/color bytes. Already-owned
input storage is excluded; budgets are logical buffer/data bytes, not driver
allocation granularity or C++ object overhead. Empty input needs zero buffers.
Point count is limited to `UINT32_MAX/16` to bound raw addressing. Budget failure
returns `FE_OUT_OF_MEMORY`; invalid descriptors, foreign objects, wrong factory,
wrong device or repeated use fail before recording. Caller command-list lifecycle
and completion remain explicit obligations.

For point-cloud gathering, keep original points for association and use the
returned decoded points as `GatherEvaluationDesc.evaluationPoints`. Quantization
does not implicitly activate topology, bin points, gather or publish a volume.

## Velocity encoding

`createVelocityQuantize` / `gvdbCreateNvrhiVelocityQuantize` selects the canonical
velocity stream using the same offline artifacts, descriptors and ticket ABI.
XYZ velocities must be finite. Missing velocity returns `FE_NOT_FOUND`, including
empty uploaded PointSets whose optional attributes are absent. Nonfinite XYZ
returns `FE_INVALID_ARGS`; arbitrary W payload bits remain supported. Position
bytes, GPU buffer and bounds, color bytes, component counts and ordinal order
remain unchanged. Completion uses actual GPU-decoded velocity data and retains
other GPU buffers; it never substitutes a host-computed result.

Use Explicit bounds or `PointQuantizationBounds::AttributeExtrema`. The latter
uses unpadded signed velocity extrema and substitutes range one for a constant
axis, matching the retained host velocity-bounds repair. `LegacyPadded` is only
accepted by the position factory; AttributeExtrema is only accepted by velocity.
Both factories retain the same numeric-domain checks and saturation arithmetic.
Code comparisons allow one interval; decoded-value error is bounded against the
actual codes using binary32 operation scales, including cancellation near zero.

The five-buffer formula above uses the selected stream stride S. Retained host
bytes are `6*N` plus all canonical attribute bytes (selected decoded values and
copies of the other attributes). Input buffers are excluded. Preparation scans
retained CPU velocity values for finite extrema; it does not read back or submit.
This operator does not duplicate points into subcells or replace the remaining
public subcell storage work.
