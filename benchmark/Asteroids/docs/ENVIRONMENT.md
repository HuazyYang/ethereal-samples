# Environment

Recorded 2026-09-30 during the setup phase. Re-check before the measurement phase.

| Item | Value |
|---|---|
| CPU | AMD Ryzen 7 8845HS (8C/16T) |
| Benchmark GPU | NVIDIA GeForce RTX 4050 Laptop GPU, vendor:device `10de:28a1`, driver 32.0.16.1047 (NVIDIA 610.47), Vulkan API 1.4.341 |
| Other GPU | AMD Radeon 780M Graphics, `1002:1900`, driver 32.0.31041.1004 |
| OS | Windows 10 Pro 10.0.19045 |
| Power plan at setup time | Balanced (`381b4222-f694-41f0-9685-ff5bb260df2e`) |
| Toolchain | Visual Studio 2022 Community (MSBuild 17.14), Windows SDK 10.0.19041.0, CMake 4.3.1 |
| Python | `python` = 3.11.9; CMake's `find_package(Python3)` selects 3.13 |
| Vulkan SDK | 1.4.357.0 installed (`VULKAN_SDK`), not required by the build |

## Adapter LUID

`-adapter_luid N` and `run.json` use the DXGI adapter LUID as one decimal number:
`N = (HighPart << 32) | LowPart` (`Benchmark::LuidToU64`).

DXGI adapters at setup time (`IDXGIFactory1::EnumAdapters1` order):

| Index | LUID (decimal) | Adapter |
|---|---|---|
| 0 | 86317 | AMD Radeon 780M Graphics |
| 1 | **96800** | **NVIDIA GeForce RTX 4050 Laptop GPU** (benchmark target) |
| 2 | 96689 | Microsoft Basic Render Driver (software) |

**A LUID is only valid until the next reboot or driver restart.** Windows assigns it when the adapter
is started. Query it again at the start of every session and never hard-code it:

```powershell
build\windows-release\bin\benchmark_selftest.exe -list_adapters
# output: <luid decimal> TAB <dxgi index> TAB <vendor:device> TAB <name>
$luid = (build\windows-release\bin\benchmark_selftest.exe -list_adapters |
         Where-Object { $_ -match 'RTX 4050' }).Split("`t")[0]
```

In code: `Benchmark::EnumerateAdapters()`, `Benchmark::FindDxgiAdapter(luid)`,
`Benchmark::FindDxgiAdapterIndex(luid)` in `src/common/benchmark.h`. For Vulkan the same number is
`Benchmark::LuidFromBytes(VkPhysicalDeviceIDProperties::deviceLUID)` (valid when `deviceLUIDValid`).

## Default adapter selection (why pinning is mandatory)

Observed with the unmodified reference sample (`asteroids_reference.exe -d3d12|-vk -close_after 5`):

- Diligent D3D12 created its device on adapter 0, the **AMD Radeon 780M**.
- Diligent Vulkan selected the **NVIDIA GeForce RTX 4050**.

Without an explicit adapter the two APIs therefore run on different GPUs. Every renderer has to
create its device on the adapter given by `-adapter_luid` and report the adapter it actually got with
`Benchmark::SetActualAdapterOrFail`.
