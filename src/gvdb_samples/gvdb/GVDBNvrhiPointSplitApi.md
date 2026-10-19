# Retained point coordinate splitting

`createPointSplit` consumes the offline `GVDBPointSplit_splitPointCoordinates.bin`
artifact and an authenticated same-device canonical PointSet. Include
`GVDBNvrhiPointSplitApi.cmake` when composing the public API library. Select
Position or Velocity and finite XYZ translation (W=0). Optional velocity must be
present; empty initial uploads have no optional velocity. Nonfinite selected or
translated XYZ is rejected during preparation. Source W/color/other attributes
remain unchanged in the retained immutable PointSet.

Prepare, record once into an open same-device graphics/compute list, submit/wait
in the caller, then finish once. No implicit wait/submission or owner publication.
The ticket retains all needed input/pipeline resources across source/operator
release. Empty input records/completes without GPU buffers and yields zero bounds.

`getAxisBuffer(0/1/2)` returns a borrowed raw float buffer range for X/Y/Z after
record. All planes share one allocation with offsets axis*N*4; caller synchronization
is required. `getAxisData` becomes available after completion. Bounds are exact
extrema of the actual completed GPU values, W=0; they have no padding/clamping.
Existing point/velocity quantization factories retain their separate bounds policies.
The legacy z==1e10 sentinel is an ordinary finite value for splitting, as in gvdbSplitPos.

Retained host result storage is 12*N bytes. New GPU/readback storage is 256+24*N
bytes in three buffers for N>0, otherwise zero. Input storage, object/allocator
metadata and native allocation alignment are excluded. Count is limited so every
32-bit raw output address fits. Failed factory/prepare clears owned outputs;
failed borrowed getters and bounds preserve caller outputs. Tickets are not
concurrently mutable; the caller owns recording, submission and completion order.
