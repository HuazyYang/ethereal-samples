# Public owned-volume voxel filters

`GVDBNvrhiVoxelFilterApi.h` provides the additive `IGVDBVoxelFilters` interface and `gvdbCreateNvrhiVoxelFilters` C factory. The existing `IGVDBVoxelOps` IID/vtable and volume API are unchanged. Include `GVDBNvrhiVoxelFilterApi.cmake` to add the facade to the same static/shared `gvdb_nvrhi_api` binary that owns authenticated volume wrappers. Installed applications use `find_package(gvdbNvrhi 1 CONFIG REQUIRED)` and link `gvdb::nvrhi_api`; the concrete C++ operator is also exported as `gvdb::nvrhi_voxel_filters`.

Create using `VoxelFiltersCreateDesc`: a public volume plus an offline `GVDBAtlasFilters_filterAtlas.bin` artifact for that volume's backend. The factory retains the volume and copies/creates the shader resources through the concrete operator. Runtime users need neither the Slang compiler nor a native GPU SDK. `getVolume()` returns the borrowed retained interface.

`prepare(request,budget,&ticket)` validates the request and prepares resources without recording, submitting or waiting. Success returns the existing `IGVDBGenerationTicket`; publish it with `volume->recordAndPublish(context,ticket)` on an open same-device NVRHI graphics/compute list. The caller owns submission, ordering and completion. As with other mutations, recording replaces compute state and must not be discarded after publication. Prepared tickets/resources and retained snapshots survive operator/volume release.

`VoxelFilterRequest` is a versioned, trivially copyable 64-byte descriptor; its `structSize` must match, reserved fields must be zero, and both flags must be exactly 0 or 1. `VoxelFilterPreparationBudget` limits GPU allocation bytes/count and has a zero reserved field. Every raw output clears before failure. Request validation and failed allocation leave the current generation unchanged. A real volume from another implementation cannot be adopted through this factory.

## Operation and scheduling contract

The six operations are Smooth, Grow, Noise, Cut, ExpandByte and ExpandColor. Smooth supports scalar, RGB and RGBA float channels; Grow/Noise/Cut require R32_FLOAT; ExpandByte requires R8_UINT; ExpandColor requires RGBA8_UINT and writes alpha byte 1. All parameter/boundary components must be finite. Exact equations and bounded conversion rules are described in [the core filter contract](GVDBNvrhiVoxelFilters.md).

Each iteration reads a stable source, filters interiors, then optionally updates apron faces, edges and corners from that result. There is no apron preupdate: the first iteration observes stored apron bytes. `updateApron=0` preserves the active bricks' previous apron bytes. `updateApron=1` uses the request's explicit boundary vector for missing neighbors; it does not change the channel descriptor's void value. Both `skipOverAprons` values preserve the retained kernels' effective interior-only filtering. Unused atlas slots and raw padding are zeroed.

At most 4096 iterations are accepted. Zero iterations authenticate and validate the request normally, then produce a single-use no-op ticket requiring no GPU allocations and changing no resource/content/placement revision. Stencil work needs at least one apron voxel. Successful work replaces only the selected channel; untouched channels retain their buffers/revisions and all old snapshots remain usable. A later host/configuration/publication change invalidates a prepared ticket.

## Verification

The public fixture independently evaluates all six operations and five formats. It checks two-iteration Smooth with both flags, stored-apron versus post-update equations, nonzero bias, fixed-coordinate Noise values, world-space Cut on both sides of x=50, integer expansion and alpha byte 1. It also checks selected-only revisions, untouched slot31 bytes, old snapshots, zero-work tickets, finite/range/size/reserved/flag failures, partial-allocation failure, foreign/stale tickets, invalid contexts and owner release before submission.

Private public tests passed D3D11/D3D12/Vulkan/CUDA, native graphics validation on all three graphics backends, and CUDA Compute Sanitizer with zero errors. Installed static/shared ABI and concrete-library link tests pass with CUDA-toolkit and Slang-compiler discovery disabled. Both installed GPU consumers verify the two-pass apron schedule and complete a retained readback after releasing the filter and volume.

Logs: `build/m5-contract/filter-api-*-smoke.log`, `filter-api-*-validation.log`, `filter-api-memcheck.log`, `filter-api-{shared,static}-{configure,build,test,install}.log`, and `filter-installed-{shared,static}-{configure,build,test,cuda-test}.log`.
