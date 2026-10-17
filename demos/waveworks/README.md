# WaveWorks Ocean and Beach

The WaveWorks SDK and both examples are built from C++, ISPC and HLSL sources alongside Ethereal. The import is based on WaveWorks commit `4d69707`.

This port currently targets Windows x64. Install ISPC and make `ispc.exe` available on `PATH`. Enable Ethereal's Donut, D3D11, D3D12 and Vulkan backends, with both DXBC and SPIR-V shader toolchains. From the Ethereal root:

```powershell
./build.ps1 -Config Release -Target waveworks_beach -CMakeArgs '-DETHEREAL_BUILD_WAVEWORKS=ON'
cmake --build build --config Release --target waveworks_ocean
./build/bin/waveworks_ocean.exe --rhi=d3d11 --simulation-dev=gpu
./build/bin/waveworks_beach.exe --rhi=vk --simulation-dev=gpu
```

The build uses Ethereal's Donut, nvrhi, ImGui and ShaderTool targets. The SDK and shader headers are generated from source in the build tree. The examples link to `donut_app` and the source-built `waveworks_sdk` import library, so Windows loads `WaveWorks.dll` normally. Render passes use `donut::app::IRenderPass` and `donut::app::DeviceManager` directly; resource allocation and transitions use NVRHI APIs. Command-line options are parsed locally without a third-party argument parser. Runtime media lives in this directory's `media/` folder; the examples find it through the source path recorded at build time.

Both examples support `--rhi=d3d11`, `--rhi=d3d12` and `--rhi=vk`. Use `--simulation-dev=cpu` for CPU wave simulation, or `gpu-N` to select a GPU adapter. Beach includes the surveyed coast and the procedural scene selected with `--beach-terrain=cove`. Run either executable with `--help` for all options. Beach pre-simulates shallow water before the first frame; allow roughly 30 seconds for the default coast on a laptop CPU.

Set `WAVEWORKS_VALIDATE_RHI=1` before launch to enable the graphics debug runtime and NVRHI validation. Vulkan validation additionally requires the Khronos validation layer. Release builds and GPU rendering smoke checks have been exercised on all three backends (RTX 4050 Laptop GPU). CPU simulation was also exercised with Vulkan Ocean and D3D12 Beach. Vulkan Beach reports four unused vertex-attribute warnings in shadow pipelines; no Vulkan validation errors remain in the tested paths.

No original WaveWorks DLL/import library, shader bytecode, pseudocode, disassembly, IDA database, test suite or tool is part of this demo.

The [FFT simulation wiki](waveworks-fft-simulation-wiki.md) describes the numerical model and source organization. Existing NVIDIA notices in imported source and media are retained.
