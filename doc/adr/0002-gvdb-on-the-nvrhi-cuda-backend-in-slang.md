# 0002. The GVDB samples run on the NVRHI CUDA backend, in Slang

- Status: Accepted
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
  shader containers, with fast math (`-fp-mode fast`: NVRTC `--use_fast_math`, as the nvcc builds had). Thrust is
  replaced by Slang kernels (a prefix sum, a 64-bit radix sort, a min/max reduction).
- **The library runs on NVRHI interfaces.** `IGVDBVolume` and the operation objects use the CUDA device
  (`nvrhi::cuda::IDevice`), `ICommandList`, `IBuffer` and `ITexture` instead of `gp::`, through a compute context
  (`gvdb::IComputeContext`: the device and the command list GVDB work is recorded into). A kernel is an
  `gvdb::IComputeKernel`: a compute pipeline whose arguments are the entry point's uniform parameters, placed by the
  reflection (`gvdb::launchKernel`).
- **CUDA constructs map to Slang as follows.** The kernels use pointers, so they are CUDA-only code: it has no D3D
  register layout, and a CUDA-only module cannot bind global resources. Everything a kernel reads is therefore a kernel
  argument or reached from one.
  - The texture and surface handles in `VDBInfo` stay handles, from `nvrhi::cuda::IDevice::getTextureHandle` /
    `getSurfaceHandle`; Slang reads them as `Sampler3D` / `RWTexture3D` through `Handle()`.
  - Node pointers become device addresses (Slang pointers).
  - `__constant__` variables become kernel arguments, passed by value (the same constant bank).
  - The OptiX launch parameters are a `[[vk::push_constant]]` block; the shader records carry no data (a hit finds its
    scene instance by `InstanceID()`).
- **Presentation goes through `IInteropDevice`**: `importTexture` plus the two queue waits; the SPH sample imports its
  vertex buffers with `importBuffer`.
- **D3D11 is no longer offered**, since it has no interop (ADR 0013). The GVDB and SPH samples refuse `-d3d11`; the SPH
  host-copy fallback goes.

## Consequences

- No nvcc, CUDA language or Thrust in the samples. `ETHEREAL_BUILD_GVDB` no longer enables the CUDA language.
- The samples depend on the format limits of the backend (ADR 0011) and on the Vulkan limit for layered, mipmapped
  textures (ADR 0013). GVDB's atlases are single-level 3D textures, which map on both APIs.
- Results may differ slightly from the CUDA reference (nvcc against NVRTC through Slang, both with fast math). They
  are compared against reference screenshots with a tolerance.
- Two defects of the CUDA C++ port are fixed on the way: the raycaster's DDA read past a node's child list for the cell
  on its far face, and colour channels were read through texture objects created for linear filtering.

## Alternatives

- **Keep `gp` and use NVRHI only for presentation.** That is the status quo; it does not exercise the new backend.
- **Run the samples on D3D12 / Vulkan compute in Slang.** Possible later: the Slang modules are the same. CUDA and
  OptiX stay the reference platform of GVDB.
