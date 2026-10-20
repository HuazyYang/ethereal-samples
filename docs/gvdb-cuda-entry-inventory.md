# Retained CUDA entry inventory

Status: review inventory, 2026-10-19. Reference retirement remains blocked on the project's acceptance gates. The machine-readable [entry inventory](gvdb-cuda-entry-inventory.json) accounts for all **100 explicit `__global__ void` declarations** currently found in the retained GVDB `.cu`/`.cuh` tree, keyed by source path plus entry name. Repeated SPH/scan names in different files are separate entries. Replacement file paths were checked on disk. Device helpers, CPU adapters, macros and graphics/presentation shaders require their own disposition; this is not a claim of exhaustive source-level parity.

The grouping records implementation roles, not positional CUDA ABI compatibility or permission to remove code. Several old kernels can share one Slang algorithm, while one old kernel can need several new stages. These counts are not a completion percentage:

| State | Entries | Meaning |
| --- | ---: | --- |
| owned-validated | 49 | Public volume-backed operators and device queries passed recorded Windows tests; platform and consumer gates remain. |
| sample-validated | 14 | Migrated sample paths exercise these replacement roles; public ownership and broader acceptance remain. |
| portable-foundation | 17 | A portable helper/program exists; the listed public contract or integration work remains. |
| partial | 20 | Related functionality exists, but the old behavior is not fully covered. |
| pending | 0 | No completed replacement/disposition is claimed. |

OptiX `__exception__main` now has an explicit diagnostic/backend disposition: opt-in SDK context validation, tagged host callbacks and failed completion replace per-pixel black recovery. The existing public shader-stage design is preserved. Zero pending roles does not mean complete parity: the 20 partial roles, public integration and project acceptance gates below remain open.

Other material gaps are explicit in the JSON: public normalized-ushort position quantization and inverse-distance level-set color now have explicit factories. Weighted color preserves the unnormalized sum and defines saturation/zero-distance behavior; public packed ushort velocity encoding now has a separate factory with exact retained attributes; public expanded subcell storage now preserves stable original ordinals and exact canonical attributes, with signed cell locations and composed position/velocity quantization. Broader consumer and platform acceptance remains. Public reduction now excludes inactive/unused slots and aprons, including poisoned imported data. Public point topology uses checked signed host candidates and does not inherit the separate GPU key helper's nonnegative 16-bit limit. Section/compositing and individual raycast/RT shading modes need separate legacy-equation/control/visual acceptance. The existing PIC/FLIP transfer utility is not a complete FLIP solver; the retained fluid samples use SPH.

The public point factory (`f18a6b9`), authenticated append candidate (`ca793cc`), first owned renderer (`b2f1185`) and retained-snapshot reduction (`4d0ee12`) are committed and validated. The custom renderer now also adopts owned snapshots (`8727803`). Public owned dense-to-sparse resampling also passes four-backend, native-validation, CUDA memory, and installed-consumer checks. Ordinary density/level-set gathers now publish owned channels with stable point association and independent support/distance radii. Depth-map also adopts public snapshots (`ea973c9`) while preserving its graphics/CUDA interop image results. Regional downsampling now has a public device-only query with explicit caller completion, exact endpoint division and Vulkan RTE32 controls, validated on both NVIDIA and AMD graphics. The resample application now consumes the public downsample query, host topology/halo, owned resample publication and raw snapshot viewer. 3d-print now consumes the public two-phase mesh voxelizer, owner/resample APIs and raw snapshot viewer. The voxelizer validates copied meshes and work/allocation budgets, exposes caller recording/completion and retained dense results, and fixes AMD integer-face misses with exact geometric division. Both graphics adapters, CUDA memory checking and static/shared installed API consumers pass; broad legacy boundary/performance/platform gates remain. Point-cloud now uses public owner/gather/apron/filter APIs and raw snapshots, preserving pre-quantization associations through a queryable gather evaluation extension. Both complete Windows matrices and two-adapter native checks pass. Public position quantization and a separate inverse-distance color factory now pass four-API, native two-adapter, CUDA memory and installed-consumer checks. Public point deposition now covers central/six-neighbor scalar scatter, exact central last/average color and original-leaf shifted XZ support/material through immutable generations, with native/memory/package acceptance. Spray now publishes through public owner/point/gather/apron/filter APIs and consumes retained raw snapshots for queries and all five display modes; exact exported legacy ray-buffer compatibility remains separate. Public obstacle mapping now retains two authenticated snapshots and repeats checked origin-to-obstacle leaf IDs into caller-selected subcell slots, with native/memory/installed-package acceptance. Other consumer and operator migrations remain active. The current project execution records and remaining gates live in the aggregate repository's `docs/slang-next-status.md` and `docs/plans/2026-10-18-gvdb-unified-volume-ownership.md`. Keep `ETHEREAL_BUILD_GVDB_LEGACY` and its sources until those gates are satisfied, including Linux, real multi-GPU routes, consumer ownership, visual comparisons and performance checks.

Tricubic density surfaces now use retained raw snapshots and the original quadratic tensor of linear atlas fetches, with explicit zero-border/clamp addressing and finite refinement repairs. Four-API analytic/asset tests, both native graphics adapters, CUDA memory checks and tests-disabled production pass. These results do not establish exact legacy image parity or authorize reference retirement.

The sample CUDA path now supplies a message callback and enables SDK validation for ray tracing with `--validation`. The NVRHI backend documents failure/output handling in `ethereal-nvrhi/docs/cuda-slang-backend.md` in the aggregate workspace; no public exception stage or black-pixel continuation is promised.

Public scalar-grid and Y-face diagnostic queries now consume authenticated retained snapshots/points with selectable R32 channels, explicit caller completion and actual GPU results. Signed floor lookup, zero misses, aligned comparison loads and explicit nonfinite tap handling replace the unsafe legacy diagnostic cases. Four APIs, native NVIDIA/AMD validation, CUDA memory checking and installed static/shared consumers pass. Broader consumer/platform/performance acceptance remains.

Public point insertion now associates center or velocity-direction support positions with retained active leaves and sorts original canonical attributes on the GPU. Stable ordinals and leaf ranges replace atomic ordering; pre-translation NOHIT exclusion is tested with a cancelling translation. Native, memory and installed-package Windows acceptance passes; broader consumer and performance/platform gates remain.

Public coordinate splitting exposes retained raw X/Y/Z ranges and completed CPU planes with exact actual-GPU extrema for position or velocity. Four APIs, native adapters, CUDA memory and installed static/shared consumers pass. Legacy bounds padding remains in the quantization factories; broader platform/performance gates remain.

Public leaf marking now has an accepted host workflow through `setActive`/`editNode`,
`finishTopology` and explicit generation publication. Exact flag bytes, stale-ticket
rejection, signed inactive/reactivated leaves, all-inactive topology and retained GPU
node/channel bytes pass four APIs, both native graphics adapters, CUDA memory checks
and installed static/shared consumers. This disposition covers host-owned markers;
GPU marker ingestion and incremental key/link/delink integration remain separate.

The public ray-query factory now accepts copied 64-byte legacy-layout records and
retained R32 snapshots. It exposes exact record transport, separate hit/normal GPU
buffers, explicit caller completion and exhaustion status. Four-API, native,
memory and installed-package checks pass. Spray's wand uses this public operator;
all twenty captures match the previous portable path exactly.

Shared 2D/3D sections now consume retained snapshots; spray adopts the 3D path.
Native Windows and CUDA memory checks pass. Correct threshold indexing changes
32 spray fixture pixels, with exact cross-API agreement. Exhaustive RGBA8 integer
composition passes all four APIs and differs from retained CUDA approximate
arithmetic by at most one byte. The group stays partial pending broad legacy
scene comparisons and multi-volume composition adoption.
