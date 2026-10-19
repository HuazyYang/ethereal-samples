# Retained scalar-grid queries

`createGridQuery` adds a device-only operation without changing existing NVRHI or
GVDB interfaces. Include `GVDBNvrhiGridQueryApi.cmake` when composing the API library.
Load the offline `GVDBGridQuery_queryScalarGrid.bin` artifact for the selected API.

Prepare authenticates an immutable same-device snapshot and canonical PointSet,
selects an R32_FLOAT channel and checks budgets. Record once into an open graphics
or compute command list. The caller submits and waits before finishing once.
No topology/channel publication, hidden submission or implicit readiness inference
occurs. Tickets retain inputs and pipeline/resources after owner/operator release.

Each output Float4 is (value, comparison, valid, 0). ScalarVoxel reads floor(position)
in unit voxel coordinates; comparison is zero. MacYDiagnostic trilinearly samples
Y faces at (i+.5,j,k+.5), and returns the aligned lower-corner value as comparison.
Both select the active leaf containing the original position, then use that leaf's
stored atlas/apron with zero outside the atlas. Taps may reach adjacent stored brick
bytes; no neighbor lookup, apron repair or physical world transform is implied.
Missing/inactive leaves and positions outside the signed integer domain produce
all-zero tuples. Atlas IEEE values propagate; validity denotes topology, not a
finite-value check. Empty results have null buffer/data and zero size.

This replaces the scalar role of legacy gvdbReadGridVel (channel is selectable)
and repairs gvdbCheckVal: deterministic missing outputs, checked query count,
signed floor coordinates and aligned comparison loads. Undefined legacy unaligned
byte reads and raced/one-past outputs are not reproduced. It is not a FLIP solver.

For N points, retained result host storage is 16*N bytes; new logical GPU/readback
storage is 384+32*N bytes in five buffers when N>0, otherwise zero. Existing input
storage, object/allocator overhead and native allocation alignment are excluded.
Budget failures return FE_OUT_OF_MEMORY before new buffer allocation. Missing
channels return FE_NOT_FOUND; non-R32 channels and incompatible artifacts return
FE_UNSUPPORTED. Descriptor/output validation and one-use ticket rules are enforced.
Failure preserves borrowed result outputs; prepare/factory clear owned outputs.
Borrowed GPU results are available after record and require caller synchronization;
CPU values become available after successful completion and live until ticket release.
