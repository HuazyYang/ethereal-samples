# Class map: Asteroids.exe (2018 donut) → reconstruction on donut main

The framework comes from `external/donut` (donut main @ 721014c with its nvrhi submodule). The 2018 classes below are
recovered from RTTI. "nvirt" is the vtable length in the binary, compared with the number of virtuals declared in
donut main. VAs are in Asteroids.exe. vtables are named `vtbl_<Class>` and unknown virtuals `<Class>::vfuncNN` in the IDA DB.

Strategy legend:
- **donut**: use donut main's class as-is. The demo code calls its API, so only demo-side call sites are reconstructed.
- **adapt**: donut has the concept but the 2018 behaviour differs, so it is reconstructed as a demo-side subclass or helper on top of donut.
- **recon**: no donut counterpart, so it is reconstructed from the binary.
- **3rd**: third-party code, statically linked. It is not reconstructed (it comes from donut's thirdparty or an upstream package).

## Framework (2018) classes

| 2018 class | vtable | ctor / key refs | nvirt | donut main counterpart | donut virtuals | strategy |
|---|---|---|---|---|---|---|
| ApplicationBase | 0x14025C810 | 0x140076440 | 17 (incl. IRenderPass) | `donut::app::ApplicationBase` : `IRenderPass` | 8 + 18 | donut. Override set to be matched in FeatureDemo |
| DeviceManager | 0x1402702D0 | 0x1401920B0, 0x1401922B0 | 12 | `donut::app::DeviceManager` | 25 | donut |
| DeviceManager_DX12 | 0x140270338 | 0x1401924B0 | 12 | `DeviceManager_DX12` (src/app/dx12) | n/a | donut. NVAPI init and the mesh-shader capability check move to the demo |
| DefaultMessageCallback | 0x140270010 | 0x1401908E0 | 2 | `donut::app::DefaultMessageCallback` | n/a | donut |
| BaseCamera | 0x14025C8B8 | 0x1400779C0 | 7 | `donut::app::BaseCamera` | 8 | donut |
| FPSCamera | 0x140257D60 | 0x140022EC0 | 7 | `donut::app::FirstPersonCamera` | n/a | donut |
| ThirdPersonCamera | 0x14025B650 | 0x140062170 | 7 | `donut::app::ThirdPersonCamera` | n/a | adapt (the demo follows the player ship with spline-animated distance) |
| IBlob / Blob | 0x14025C260 / 0x14025E5F0 | 0x140097EA0 | 3 | `donut::vfs::IBlob` / `Blob` | 3 | donut |
| StbImageBlob | 0x14025C280 | 0x140070B50 | 3 | internal to `donut::engine::TextureCache` | n/a | donut (TextureCache) |
| IFileSystem / NativeFileSystem | n/a / 0x140257E00 | 0x1400230B0 | 4 | `donut::vfs::IFileSystem` / `NativeFileSystem` | 7 | donut |
| SQLiteFileSystem | 0x14025E9E8 | 0x14009A720 (ctor), 0x14009AA70 (readFile) | 4 | none | n/a | **recon** as `donut::vfs::IFileSystem` (SQLite + BCrypt PBKDF2/AES + LZ4) |
| IView / PlanarView | 0x14025CE90 | 0x14007EA90 | 21 | `donut::engine::PlanarView` | 22 + 3 | donut |
| CubemapView | 0x14025CF40 | 0x14007E8E0 | 21 | `donut::engine::CubemapView` | n/a | donut |
| CompositeView / ICompositeView | 0x14025D1B8 | 0x140080600 | 2 | `donut::engine::CompositeView` | 3 | donut |
| StereoView<PlanarView> | n/a | n/a | n/a | `donut::engine::StereoView<PlanarView>` | n/a | donut (single-pass stereo permutations) |
| IShadowMap / CascadedShadowMap | 0x14025D1D0 | 0x140080600 | 12 | `donut::render::CascadedShadowMap` | 12 | donut |
| PlanarShadowMap | 0x140260C50 | 0x1400AD500 | 12 | `donut::render::PlanarShadowMap` | 12 | donut |
| Light / DirectionalLight / PointLight / SpotLight | 0x140257DA0 / DB8 / DE8 / DD0 | 0x1400245B0, 0x140036540, 0x140031980 | 2 | `donut::engine::Light` (SceneGraphLeaf) … | 3 | adapt (2018 lights are free objects; wrap them in SceneGraph leaves) |
| IDrawStrategy | n/a | n/a | 3 | `donut::render::IDrawStrategy` | 3 | donut |
| InstancedOpaqueDrawStrategy | 0x14025D360 | 0x1400833B0 | 3 | same | n/a | donut |
| TransparentDrawStrategy | 0x14025D380 | 0x140083510 | 3 | same | n/a | donut |
| BasicDrawStrategy | n/a | n/a | n/a | none (main has Instanced/Transparent only) | n/a | recon (trivial) |
| ForwardShadingPass | n/a | strings at 0x14025D570 | n/a | `donut::render::ForwardShadingPass` | 14 | donut, plus 2018 single-cbuffer differences handled in the demo forward_ps |
| ShaderFactory | n/a | 0x14019F180 (createShaderPermutation) | n/a | `donut::engine::ShaderFactory` | n/a | donut |
| ImGui_Renderer | 0x14025DE80 | 0x140091470 | 12 | `donut::app::ImGui_Renderer` | 12 | donut |
| Animation::Track<bool/int/float/float2/float3>, AbstractTrack | 0x140257E58 … | 0x14002FF40 | 3 | `donut::engine::animation::Sampler` (KeyframeAnimation) | n/a | adapt: animations.json uses step/spline modes (reconstruct the loader on top of donut samplers or as a small demo-side class) |
| chunk::ChunkFile / ChunkReader / MeshSet | n/a | 0x1400B4560, 0x1400B28E0 | n/a | `donut::chunk` (core/chunk) | n/a | adapt: the 2018 revision has a materials chunk and no `materialName`/`ctm`. Use a demo-side reader or patch the donut reader |
| ShaderBlob (NVSP) | n/a | 0x1401ADB10 | n/a | ShaderMake `ShaderBlob.h` | n/a | donut / 3rd |
| Json::* (CharReader, …) | 0x14027B7A8 … | n/a | n/a | jsoncpp (donut thirdparty) | n/a | 3rd |

## Demo classes (no donut counterpart, so recon)

| 2018 class | vtable | ctor / key refs | nvirt | notes |
|---|---|---|---|---|
| FeatureDemo | 0x140257F68 | 0x1400230B0 (init: media.db, ShaderFactory, scene load) | 17 | main render pass, derived from ApplicationBase |
| UIRenderer | 0x140258420 | 0x140024960 | 12 | derived from ImGui_Renderer, the demo UI |
| SpaceScene | 0x14025B040 | 0x14005A010, 0x14005A700 | 4 | asteroids.fscene loader, sectors and placement |
| SpaceObject | 0x14025AAB0 | 0x140050520, 0x140050EB0 | 5 | ships, planets, scene objects |
| MeshletDrawStrategy | 0x140259970 | 0x1400400E0 | 3 | NVAPI TS/MS meshlet draw path |
| IMeshSet | n/a | n/a | n/a | interface over chunk MeshSet (meshlets) and assimp meshes |
| MeshletRenderResources | n/a | n/a | n/a | GPU buffers for meshlets (vertex/normal/texcoord/index/prim/meshlet/lodInfo/instance) |

## Excluded third-party (not reconstructed)
nvrhi (d3d12 and validation, embedded older revision), GLFW, NVAPI, nvToolsExt, Assimp, PhysX 3.4 + extensions,
GFSDK_SSAO_D3D12, SQLite 3, LZ4, stb_image, Dear ImGui, jsoncpp, MSVC STL/CRT.
