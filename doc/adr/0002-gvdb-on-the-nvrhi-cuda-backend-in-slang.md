# 0002. The GVDB samples run on the NVRHI CUDA backend, in Slang

- Status: Proposed
- Date: 2026-10-22
- Plan: aggregate [`docs/plans/2026-10-22-slang-cuda-optix-backend.md`](../../../docs/plans/2026-10-22-slang-cuda-optix-backend.md),
  steps 11-15 (maintained copy in `ethereal-nvrhi/doc/plans`)

## Context

`src/gvdb_samples` computes on a private COM layer, `gp` (`gvdb/GPDevice*.h`): a CUDA driver-API device, an OptiX 9
device and interop with NVRHI. The kernels are written in CUDA C++:
- 127 `.cu` kernels and 12 OptiX programs, compiled to PTX by nvcc;
- plus Thrust for sorting and reductions.

NVRHI now has a CUDA and OptiX backend that runs Slang code compiled offline (ethereal-nvrhi ADRs 0011, 0012), and an
interop interface for D3D12 and Vulkan (ADR 0013). The samples are its test bed.

## Decision

The samples move from `gp` to NVRHI, and all of their CUDA C++ goes.

- **Kernels and OptiX programs become Slang modules**, compiled by `ethereal_compile_shader_containers()` into NVRHI
  shader containers. Thrust is replaced by Slang kernels (a 64-bit radix sort, a min/max reduction).
- **The library runs on NVRHI interfaces.** `IGVDBVolume` and the operation objects use `nvrhi::IDevice` (a CUDA device),
  `ICommandList`, `IBuffer` and `ITexture` instead of `gp::`. A kernel is a cached `IComputePipeline`.
- **CUDA constructs map to Slang as follows.**
  - The texture and surface handles in `VDBInfo` become global `Texture3D` / `RWTexture3D` arrays bound per element.
  - Node pointers become device addresses.
  - `__constant__` variables become global parameters.
- **Presentation goes through `IInteropDevice`**: `importTexture` plus the two queue waits.
- **D3D11 is no longer offered**, since it has no interop (ADR 0013). The GVDB and SPH samples refuse `-d3d11`; the SPH
  host-copy fallback goes.

## Consequences

- No nvcc, CUDA language or Thrust in the samples. `ETHEREAL_BUILD_GVDB` no longer enables the CUDA language.
- The samples depend on the format limits of the backend (ADR 0011) and on the Vulkan limit for layered, mipmapped
  textures (ADR 0013). GVDB's atlases are single-level 3D textures, which map on both APIs.
- Results may differ slightly from the CUDA reference (fast math in nvcc against NVRTC through Slang). They are
  compared against reference screenshots with a tolerance.

## Alternatives

- **Keep `gp` and use NVRHI only for presentation.** That is the status quo; it does not exercise the new backend.
- **Run the samples on D3D12 / Vulkan compute in Slang.** Possible later: the Slang modules are the same. CUDA and
  OptiX stay the reference platform of GVDB.
