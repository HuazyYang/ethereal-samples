# Owned voxel filters

`GVDBNvrhiVoxelFilters.h` prepares filters over an authenticated `NvrhiVolume` publication. Link `gvdb::nvrhi_voxel_filters` and supply the local `GVDBAtlasFilters_filterAtlas.bin` artifact for the device. Runtime consumers need no compiler, Donut, native SDK or VFS. The concrete operator borrows its owner; the owner must outlive the operator and preparation calls. Prepared tickets retain all buffers, pipelines and bindings independently.

Call `prepare` and then the existing `NvrhiVolume::recordAndPublish` on an open caller-owned command list. The owner does not open, submit or wait. Only the selected channel gets new storage/content revisions. Topology and other channels retain their exact resources/revisions. Failed validation/allocation leaves the output ticket and publication unchanged. As with the owner, recording/device-loss failures cannot roll back void driver commands. Caller submission ordering and cross-queue synchronization remain required.

## Retained equations and deliberate extensions

All filters operate on active brick **interiors**. `parameters.x/y/z` correspond to the retained `params` vector; `w` is reserved but must be finite. Unused parameters are ignored after finite validation.

| Operation | Format | Equation |
| --- | --- | --- |
| Smooth | R32_FLOAT, RGB32_FLOAT, RGBA32_FLOAT | `(p.x * center + xm + xp + ym + yp + zm + zp) / (p.x + 6) + p.y`, componentwise. Float3/4 are explicit extensions of the scalar CUDA kernel. |
| Grow | R32_FLOAT | If center is nonzero, add `p.x * 10`; then values below `.01` become zero. |
| Noise | R32_FLOAT | Only values above `.01` add `p.x * random(atlasIntegerCoordinate)`. The retained Jenkins uint hash consumes the float bit patterns of atlas x/y/z; low23 mantissa bits construct `[0,1)`. No iteration-dependent seed is added. Placement changes therefore change noise. |
| Cut | R32_FLOAT | If voxel-center world x is below50 and `0 < value < 2`, replace it with `.02`; otherwise preserve it. Parameters are ignored, as in the retained kernel. Integer cell x<50 gives the exact center test within the owner's coordinate domain. |
| ExpandByte | R8_UINT | A zero center becomes truncated `p.y` if any of its six neighbors equals truncated `p.x`; all other bytes remain unchanged. |
| ExpandColor | RGBA8_UINT | Sum `int3(center.rgb*p.x)` and six separately truncated `int3(neighbor.rgb*p.y)` terms. If the largest RGB sum exceeds255, rescale each signed integer component by `component*255/max` with truncating division. Store low8 bits per RGB component and **alpha byte1**. Negative valid weights preserve signed-truncation/byte-wrap behavior. |

The retained references are `GVDBVoxelOps::compute` in `GVDBVoxelOps.cpp` and the six kernels in `kernels/GVDBOperators.cu`. Each iteration dispatches the filter, then optionally updates aprons. **There is no initial apron update.** First-pass stencil reads observe the publication's existing stored apron bytes, even if stale. With `updateApron=false`, those original apron bytes persist across all iterations. With it enabled, each filter result is followed by full apron reconstruction before the next iteration. This intentionally differs from the older portable `SlangAtlas` convenience helper, which preupdates aprons.

`skipOverAprons` retains its effective behavior for both true and false. Every retained compute kernel uses `GVDB_VOXPACKED`; false only makes the host launch larger dimensions. The packed-coordinate transform still covers interiors and clips extra threads outside the atlas. False does **not** request apron filtering. The new implementation computes the same interior domain without those redundant threads.

Each filter and apron pass reads a separate immutable source allocation and writes another allocation. This removes the retained CUDA in-place read/write race: each iteration has a deterministic complete source. Two full-atlas destination/scratch buffers are sufficient, independent of iteration count; immutable constant buffers and prebuilt binding sets are shared as appropriate. This is an intentional correctness correction, not a claim to reproduce race-dependent legacy outputs. Work is assigned by destination32-bit word so adjacent R8 voxels cannot race on packed stores.

## Boundaries and restrictions

Stencil filters (Smooth and the two expansions) require at least one apron cell for nonempty, nonzero work. Apron0 returns `FE_UNSUPPORTED`; pointwise filters support apron0. Arbitrary supported apron widths are reconstructed, including faces, edges and corners. Neighbor lookups use integer topology traversal, preserving signed coordinates and avoiding float voxel-center rounding.

`boundaryValue` is explicit and does not replace the channel descriptor's void value. Missing sparse neighbors use the full supplied vector. R8/RGBA8 boundary components are integer storage units in `[0,255]`, not normalized colors; R8 uses x, float formats use their component count. All four parameter/boundary components must be finite. The full vector boundary and wider-apron support extend the retained scalar boundary interface. Active interiors are never replaced by boundary values. Inactive/unused slots and physical tail padding become zero; existing inactive-slot poison is not preserved.

Additional checked restrictions:

- Smooth rejects `p.x == -6` (zero denominator).
- Grow rejects `abs(p.x) > FLT_MAX/10`, avoiding overflow of the constant increment itself.
- ExpandByte accepts `p.x,p.y` in `[0,255]` and truncates fractional labels toward zero.
- ExpandColor requires `abs(p.x)+6*abs(p.y) <= 32768`. This conservatively keeps all signed conversions, accumulated sums and subsequent multiplication by255 inside int32. Values outside this safe domain return `FE_INVALID_ARGS` instead of reproducing undefined/out-of-range legacy conversions.
- More than4096 iterations returns `FE_UNSUPPORTED`. This is an explicit current preparation-size restriction, not compatibility with unbounded legacy counts. Resource budget failures return `FE_OUT_OF_MEMORY`.
- Unknown operations, invalid/missing channels, unsupported formats and dirty/unpublished owner state fail before recording. Format/parameter validation still applies to zero iterations.

Float source data is not scanned on the CPU. Float arithmetic follows the target's IEEE behavior and may propagate NaNs or overflow for extreme source values; the API does not promise finite results from arbitrary raw payloads. Unchanged apron bytes remain bit-exact until an apron pass, and apron reconstruction preserves neighbor interior bits. Numerical float results may differ in final rounding across APIs; byte expansions and non-arithmetic bytes have exact expectations.

Zero iterations and empty publications produce an authenticated unconsumed no-op ticket without GPU allocation. Recording consumes it exactly once and leaves the exact generation/resource/revision identities unchanged. Stale no-op tickets are rejected using the same owner validation as other mutations.

## Validation fixture

The independent CPU oracle works per voxel, with explicit signed sparse adjacency and separately staged filter/apron passes. Tests exercise all six operations, scalar/vector smooth, multi-iteration byte propagation, both skip settings, no-preupdate stale aprons, update-disabled preserved aprons, byte wrap/normalization/alpha1, signed Cut threshold, deterministic atlas-space noise, inactive/unused slots, revision isolation, zero-work/budget/format failures, and readbacks retained after releasing owner/operator before submission. The CMake test module is optional; only the test harness depends on Donut.
