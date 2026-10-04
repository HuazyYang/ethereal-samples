# Reconstruction conventions (asteroids/)

These rules apply to every reconstructed source file of the Asteroids demo.

## Ground truth
- The binary is authoritative: `D:\ps\repo\nvrhi-asteroids\asteroids\Asteroids.exe` (IDA DB `Asteroids.exe.i64`).
- Hex-Rays output of every app function is in `recon/build/decomp/<addr>_<name>.c` with `index.tsv`
  (address, name, size, referenced strings). Each file header lists its strings and callees.
- The demo's data files are extracted under `recon/assets/media/media/**` (JSON, FBX, textures, .chk).
- Shader bindings and cbuffer layouts come from `recon/assets/shaders_split/**.dxil.txt` (reflection),
  and the reconstructed HLSL and shared headers live in `asteroids/shaders/demo/` and `asteroids/shaders/demo/include/`.

## Framework
- The framework is **donut main** (`recon/external/donut`) with its nvrhi. Do not reimplement donut classes.
  The 2018 framework classes are mapped to donut in `recon/docs/class_map.md`.
- Use donut idioms: `nvrhi::DeviceHandle`, `std::shared_ptr<donut::engine::ShaderFactory>`,
  `std::shared_ptr<donut::engine::CommonRenderPasses>`, `donut::engine::FramebufferFactory`,
  `donut::engine::IView`/`PlanarView`, `donut::engine::BindingCache`, `donut::vfs::IFileSystem`,
  `donut::log::{info,warning,error}`, `donut::json::Read<T>` plus jsoncpp (`<json/json.h>`), and `donut::math` (`dm::`) types.
- Render-pass classes follow donut style: a constructor taking `(nvrhi::IDevice*, std::shared_ptr<ShaderFactory>, std::shared_ptr<CommonRenderPasses>)`
  or similar, an `Init()` that creates shaders, binding layouts and pipelines, and `Render(nvrhi::ICommandList*, const IView&, nvrhi::IFramebuffer*, ...)`.
- Shaders are loaded through `ShaderFactory::CreateShader("demo/<file>.hlsl", "<entry>", &defines, type)`.
  The compiled blobs are built by ShaderMake from `asteroids/shaders/**/*.cfg`.
- Graphics API: D3D12 only. The 2018 NVAPI mesh shaders are replaced by D3D12 SM6.5 amplification/mesh shaders
  (nvrhi `MeshletPipelineDesc` / `MeshletState` / `dispatchMesh`). NVAPI is otherwise used only where the 2018 code used it.

## Third-party (not reconstructed)
PhysX 3.4.2, assimp (C API), HBAO+ 4.0 D3D12 (`GFSDK_SSAO.h`), nvToolsExt, SQLite, LZ4, stb, ImGui and jsoncpp.
Call them through their public headers (targets `asteroids_physx`, `assimp`, `GFSDK_SSAO_D3D12`, …; see `recon/cmake/ThirdPartyDlls.cmake`).

## Code style
- C++17, 4-space indent, donut naming (`PascalCase` types and methods, `m_` members, `c_` constants, `g_` globals).
- Put each 2018 class in its own `.h/.cpp` pair under `asteroids/src/<area>/`. Areas are `app`, `fs`, `meshlets`, `scene`, `passes`, `fx`, `ui` and `audio`.
- At the top of each class, note the binary provenance in one comment line, e.g. `// Asteroids.exe: SpaceScene (vtable 0x14025B040, ctor 0x14005A010)`.
  Do not annotate every line.
- Keep names from strings, RTTI, JSON keys, reflection and resource debug names. Invent clear names only where none exist.
- Where the reconstruction deliberately deviates (donut API differences, D3D12 mesh shaders, bug fixes), add a short
  `// deviation: ...` comment.
- No placeholders: reconstruct real logic. If something cannot be determined, implement the closest evident
  behaviour and leave a `// unresolved: ...` comment explaining what is unknown.
- Everything is compiled into the `asteroids_core` static library (glob of `asteroids/src/**`). Keep headers self-contained.
