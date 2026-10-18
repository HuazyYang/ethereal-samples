# Retained CUDA entry inventory

Status: review inventory, 2026-10-19. Reference retirement remains blocked on the project's acceptance gates. The machine-readable [entry inventory](gvdb-cuda-entry-inventory.json) accounts for all **100 explicit `__global__ void` declarations** currently found in the retained GVDB `.cu`/`.cuh` tree, keyed by source path plus entry name. Repeated SPH/scan names in different files are separate entries. Replacement file paths were checked on disk. Device helpers, CPU adapters, macros and graphics/presentation shaders require their own disposition; this is not a claim of exhaustive source-level parity.

The grouping records implementation roles, not positional CUDA ABI compatibility or permission to remove code. Several old kernels can share one Slang algorithm, while one old kernel can need several new stages. These counts are not a completion percentage:

| State | Entries | Meaning |
| --- | ---: | --- |
| owned-validated | 18 | Public owner-backed mutation/filter replacements passed the recorded Windows matrix; platform and consumer gates remain. |
| sample-validated | 12 | Migrated sample paths exercise these replacement roles; public ownership and broader acceptance remain. |
| portable-foundation | 37 | A portable helper/program exists; the listed public contract or integration work remains. |
| partial | 27 | Related functionality exists, but the old behavior is not fully covered. |
| pending | 6 | No completed replacement/disposition is claimed. |

The six pending entries are `gvdbScatterPointDensity`, `gvdbScatterPointAvgCol`, `gvdbAddSupportVoxel`, `gvdbMapExtraGVDB`, `gvdbRaySurfaceTricubic`, and OptiX `__exception__main`. The last requires an explicit diagnostic/backend disposition, not necessarily a one-to-one public shader stage.

Other material gaps are explicit in the JSON: normalized-ushort level-set color needs inverse-distance weighting and a zero-distance rule; ordinary gather after quantization does not cover it. Reduction must exclude inactive/unused slots, which the existing packed-column helper alone does not guarantee. Public signed point topology must not silently adopt the helper's nonnegative 16-bit key limit. Section/compositing and individual raycast/RT shading modes need separate legacy-equation/control/visual acceptance. The existing PIC/FLIP transfer utility is not a complete FLIP solver; the retained fluid samples use SPH.

The public point factory, authenticated append candidate and first owned renderer are uncommitted work at this inventory snapshot. Update the groups only after their acceptance evidence is recorded. The current project execution records and remaining gates live in the aggregate repository's `docs/slang-next-status.md` and `docs/plans/2026-10-18-gvdb-unified-volume-ownership.md`. Keep `ETHEREAL_BUILD_GVDB_LEGACY` and its sources until those gates are satisfied, including Linux, real multi-GPU routes, consumer ownership, visual comparisons and performance checks.
