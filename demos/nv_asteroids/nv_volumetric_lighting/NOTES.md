# NvVolumetricLighting.d3d11 reconstruction notes

This folder reconstructs the C++ and HLSL source of `NvVolumetricLighting.d3d11.dll` (the NVIDIA
GameWorks Volumetric Lighting D3D11 backend, shipped next to the Asteroids demo), working only from
the binary: the Hex-Rays decompilation in `recon/build/nvvl_decomp/` and the fxc disassembly of
the 269 embedded DXBC blobs.

Summary of the verification results (section 9 has the details):

| Check | Result |
|---|---|
| Export table | identical (9/9 mangled names) |
| Imports | CRT + kernel32 only, no d3d11/dxgi, like the original |
| Shaders, byte-identical DXBC | **268 / 269** |
| Shaders, identical signatures / bindings / cbuffer layouts | **269 / 269** |
| Shaders, identical instruction streams | 268 / 269 (the remaining one differs only in register allocation) |
| Smoke test (WARP, 4 configurations x 3 frames, original vs rebuilt DLL) | bit-identical images, identical allocation counts |

## 1. Layout

```
NvVolumetricLighting/
  CMakeLists.txt                     builds NvVolumetricLighting.d3d11 (SHARED) + smoke test
  NvVolumetricLighting.d3d11.original.def / .rebuilt.def   export lists (dll2def.py)
  include/Nv/VolumetricLighting/
    NvVolumetricLighting.h           public API (exported signatures, enums, descriptors)
    NvFoundationTypes.h              nvidia::NvAllocatorCallback / NvAssertHandler, Nvc vector types
  src/
    Common.h                         globals, NV_NEW / global operator new/delete, release macros
    NvVolumetricLighting.cpp         exports, default allocator + assert handler, allocation operators
    VectorMath.h/.cpp                Vec2/3/4, Mat44 (column vectors), Inverse, Halton
    ShaderConstants.h                C++ mirrors of cbContext / cbFrame / cbVolume / cbApply
    ContextImp.h/.cpp                platform independent context (stage sequencing, constant buffers)
    d3d11/
      ContextImp_D3D11.h/.cpp        D3D11 backend (resources, states, all passes)
      D3D11Util.h/.cpp               ConstantBuffer<T>, RenderTarget, DepthTarget, LoadShaders
      ShaderPermutations.h           permutation keys (bit fields == table indices)
      CompiledShaders.h              declarations of the embedded shader tables
  shaders/
    ShaderCommon.hlsli               cbuffers (packoffset), samplers, shared constants
    Quad.hlsli, PostProcess.hlsli    fullscreen triangle interface
    RenderVolumeGeom.hlsli           VS/HS/DS/PS interface structs of the light volume
    ShadowMap.hlsli                  shadow map sampling used by the domain shader
    *_VS/HS/DS/PS/CS.hlsl            one source per shader family (12 families)
    permutations/<family>.json       permutation axes (macro, bit shift, width, values)
    permutations.json                merged list of all 269 permutations (generated)
    Permutations.cmake               the same list for CMake (generated)
  tools/
    shaders.py                       check / diff / gen (compile + compare with the original blobs)
    shader_tables.json               the 12 shader tables of the DLL (index -> blob VA, size)
  test/SmokeTest.cpp                 WARP smoke test, loads the original or the rebuilt DLL
```

Build (the root `recon/CMakeLists.txt` adds this folder automatically):

```
cmake -S D:/ps/repo/nvrhi-asteroids/recon -B D:/ps/repo/nvrhi-asteroids/recon/build_nvvl -G "Visual Studio 17 2022" -A x64
cmake --build D:/ps/repo/nvrhi-asteroids/recon/build_nvvl --config Release --target NvVolumetricLighting.d3d11
```

Every permutation is compiled at build time with
`fxc /nologo /WX /T <profile> /E main /D <AXIS>=<value>... /Vn g_<family>_<index> /Fh <header>`
(`NVVL_FXC` defaults to the SDK 10.0.19041.0 fxc, which identifies as compiler 10.1 like the
original blobs). The tables in `CompiledShaders.cpp` are generated at configure time from
`Permutations.cmake` and have the same sizes and index layout as the tables in the original DLL
(missing permutations are `nullptr`). Python is only needed to regenerate
`Permutations.cmake` / `permutations.json` (`python tools/shaders.py gen`) after editing a
`permutations/*.json` file.

`/WX` matters: the original RDEF compile flags are `0x00040100`
(`D3DCOMPILE_NO_PRESHADER | D3DCOMPILE_WARNINGS_ARE_ERRORS`).

## 2. Public API

The exported functions (all in `Nv::VolumetricLighting`, plus `nvidia::NvGetAssertHandler`) use an
"Args struct" calling convention: `BeginAccumulation(Context, BeginAccumulationArgs*)`,
`RenderVolume(Context, RenderVolumeArgs*)`, `EndAccumulation(Context, EndAccumulationArgs*)` and
`ApplyLighting(Context, ApplyLightingArgs*)`. `Context` is `void*`. Status codes:
`OK 0, FAIL -1, INVALID_VERSION -2, UNINITIALIZED -3, UNIMPLEMENTED -4, INVALID_PARAMETER -5,
UNSUPPORTED_DEVICE -6, RESOURCE_FAILURE -7, API_ERROR -8`.

**Reference header.** The policy allowed consulting the public header of the original GitHub
repository. That failed: `NVIDIAGameWorks/VolumetricLighting` returns "Repository not found" /
HTTP 404, because the GameWorks repositories need an org membership. So nothing was downloaded, and
`third_party_reference/` was removed again.

Enum, descriptor and field names follow the public NvVolumetricLighting API naming (ContextDesc,
ViewerDesc, MediumDesc, ShadowMapDesc, LightDesc, VolumeDesc, PostprocessDesc, eDownsampleMode,
fTargetRayResolution, ...), as far as it is known. Every layout and enum value was then derived
from, and checked against, the accesses made by the binary:

| Struct | Size | Evidence |
|---|---|---|
| VersionDesc | 8 | `OpenLibrary`: `Major == 1 && Minor == 0` |
| PlatformDesc | 16 | `platform == 0` (D3D11), device at +8, `GetFeatureLevel() >= 11_0` |
| ContextDesc | 24 | copied (`memcpy 0x18`); framebuffer w/h/samples, downsample, internal MSAA, filter mode |
| ViewerDesc | 148 | copied (`memcpy 0x94`); mProj, mViewProj, vEyePosition, viewport w/h (+140/+144) |
| MediumDesc | 96 | vAbsorption, uNumPhaseTerms, 4 x {ePhaseFunc, vDensity, fEccentricity} (stride 20) |
| ShadowMapDesc | 352 | eType, uWidth, uHeight, uElementCount, 4 x {mViewProj, offset/size, mArrayIndex} (stride 84) |
| LightDesc | 144 | eType, mLightToWorld, vIntensity, union Directional / Spotlight / Omni |
| VolumeDesc | 16 | fTargetRayResolution, uMaxMeshResolution, fDepthBias, eTessQuality |
| PostprocessDesc | 100 | mUnjitteredViewProj, fTemporalFactor, fFilterThreshold, eUpsampleQuality, vFogLight, fMultiscatter, bDoFog, bIgnoreSkyFog, fBlendfactor |

**Args structs.** Fields that the D3D11 backend never reads are declared as `unresolvedNN`
placeholders: `BeginAccumulationArgs+32`, `ApplyLightingArgs+24` and `+32`. Field placement:

- Scene depth SRV: `BeginAccumulationArgs+40` and `ApplyLightingArgs+40`.
- Shadow map SRV: `RenderVolumeArgs+32`.
- Scene target RTV: `ApplyLightingArgs+16`.

**Matrices** (`NvcMat44`) are four columns with the column-vector convention: the C++ code computes
`M * v` and `lastViewProj * inverse(viewProj)`. The memory layout is identical to D3DX /
DirectXMath row-major matrices used with row vectors, so DirectXMath matrices can be passed
unchanged, as the smoke test does. The HLSL keeps the default column_major packing with `mul(M, v)`.

## 3. Architecture

```
exports (NvVolumetricLighting.cpp)
  -> ContextImp (abstract, 328 bytes): owns ContextDesc / ViewerDesc copies, temporal state,
     fills the constant buffers, calls the stage hooks in order
       -> ContextImp_D3D11 (728 bytes): D3D11 objects and the 15 stage hooks
```

* `BeginAccumulation` copies `debugFlags` and the ViewerDesc, then calls the hooks in order:
  1. `BeginAccumulation_Start`: uploads cbContext (once per context) and cbFrame; binds the CBs and
     the samplers.
  2. `_UpdateMediumLUT`: renders the 1x512 phase function LUT.
  3. `_CopyDepth`: copies or downsamples the scene depth into the internal D24S8 buffer, with jitter.
  4. `_End`: clears the accumulation target.
* `RenderVolume`:
  1. `RenderVolume_Start`: uploads cbVolume, clears the stencil to `0xFF`, binds the CBs and samplers
     on VS/HS/DS/PS/CS.
  2. One of `_DoVolume_Directional`, `_DoVolume_Spotlight` or `_DoVolume_Omni`. Any other light type
     returns INVALID_PARAMETER.
  3. `_End`: unbinds the render target.
* `EndAccumulation` calls `EndAccumulation_Imp`, which does nothing.
* `ApplyLighting`:
  1. `_Start`: uploads cbApply (temporal reprojection setup) and sets the internal viewport.
  2. Then, by filter mode:
     - TEMPORAL: `_Resolve`, then `_TemporalFilter`.
     - otherwise, with internal MSAA: `_Resolve` only.
  3. `_Composite`.
  4. `_End`.
  5. Afterwards the jitter index advances (8-step Halton(2,3)) and the two history slots swap.

### Light volume rendering (per light)

The volume is rendered in three stages:

1. **Geometry pass.** The volume mesh is generated from `SV_VertexID` (no vertex buffers):
   - Directional: a grid of `res x res` 4-control-point patches on the light frustum far plane.
   - Spotlight: the same grid, plus a frustum cap drawn with `rs_CullFront_`.
   - Point light: 6 grid faces (dual paraboloid).
   - `res = uMaxMeshResolution / {16, 32, 64}`, chosen by `eTessQuality`.

   The hull shader tessellates up to the max factor, driven by the projected size and
   `fTargetRayResolution`. The domain shader displaces every vertex to the shadow map depth (searching
   cascades for directional lights). The pixel shader adds the signed analytic in-scattering integral
   from the eye to the surface: front faces subtract, back faces add.

   The depth/stencil state `dss_RenderVolume_` tests against the internal depth (LESS_EQUAL, no
   write) and counts depth failures in the stencil: front INCR, back DECR, starting from `0xFF`.
2. **Sky pass** (directional, non-wireframe only). A fullscreen pass with `dss_RenderVolume_Sky_`
   covers volumes that extend past the far plane.
3. **Final pass.** A fullscreen pass with `dss_RenderVolume_Final_` (stencil `0xFF > s`, read-only
   DSV, internal depth bound at t2) integrates up to the scene depth where the ray ends inside the
   volume.

Spot and point lights first compute **light LUTs** in two compute passes, ping-ponging between
`LUT[0]` and `LUT[1]`:

- Point light, or spotlight with falloff NONE: only the point LUT P.
- Spotlight with falloff FIXED: P, S1 and S2.
- Spotlight with falloff CUSTOM: no LUT; the PS integrates numerically instead.

### Render targets (all created in `CreateResources`, 0x180005FE0)

| Member | Debug name | Size / format | Created when |
|---|---|---|---|
| pDepth_ (+456) | NvVl::Depth | internal buffer, D24S8 (R24G8 typeless), internal MSAA | always |
| pPhaseLUT_ (+464) | NvVl::Phase LUT | 1 x 512 RGBA16F | always |
| pLightLUT_P_[2] (+472) | NvVl::Light LUT Point [i] | 256 x 512 RGBA16F | always |
| pLightLUT_S1_[2] (+488) | NvVl::Light LUT Spot 1 [i] | 256 x 512 RGBA16F | always |
| pLightLUT_S2_[2] (+504) | NvVl::Light LUT Spot 2 [i] | 256 x 512 RGBA16F | always |
| pAccumulation_ (+520) | NvVl::Accumulation | internal buffer RGBA16F, internal MSAA | always |
| pResolvedAccumulation_ (+528) | NvVl::Resolved Accumulation | internal buffer RGBA16F | MSAA or temporal |
| pResolvedDepth_ (+536) | NvVl::Resolved Depth | internal buffer RG16F | MSAA or temporal |
| pFilteredAccumulation_[2] (+544) | NvVl::Filtered Accumulation | internal buffer RGBA16F | temporal |
| pFilteredDepth_[2] (+560) | NvVl::Filtered Depth | internal buffer RG16F | temporal |

The debug names are passed to the create functions but never used, so no `SetPrivateData` is
called. The internal buffer is the framebuffer size `>> {0, 1, 2}` (FULL / HALF / QUARTER).

### Device states

- **Rasterizer.** Base: `CD3D11_RASTERIZER_DESC(D3D11_DEFAULT)` with FrontCCW = TRUE and
  DepthClip = FALSE.
  - `rs_CullNone_`
  - `rs_CullFront_`
  - `rs_Wireframe_`
- **Samplers.** Base: `CD3D11_SAMPLER_DESC(D3D11_DEFAULT)`, clamp addressing.
  - `ss_Point_` (s0)
  - `ss_Linear_` (s1)
- **Depth-stencil.**
  - `dss_NoDepth_`
  - `dss_WriteDepth_`: func ALWAYS.
  - `dss_RenderVolume_`: LE, no write, stencil front INCR / back DECR on depth fail.
  - `dss_RenderVolume_Sky_`: front NEVER, back DECR on depth fail.
  - `dss_RenderVolume_Final_`: no depth, stencil write mask 0, front NEVER, back GREATER.
  - Created but unused: `dss_TestDepth_` and `dss_RenderVolume_NoDepth_`.
  - Slot +664 (`dss_Unused_`) is never created or used.
- **Blend.**
  - `bs_NoColor_`: write mask 0.
  - `bs_NoBlend_`
  - `bs_Additive_`: src * blend factor + dst.
  - `bs_Additive_Modulate_`: src0 * fBlendfactor + dst * src1 (dual source).
  - `bs_Debug_Blend_` (NO_BLENDING): src, alpha dst * src1.a.

## 4. Function map (original address -> reconstruction)

CRT / startup code (0x18000F060 - 0x1800105B0) and EH funclets (0x1800105C0+) are not reconstructed.
Several tiny user functions were mislabeled by FLIRT as library code (e.g. `GetId`, `_OwningContext`,
`file_name`, `ClaimTicket`, `_Transcode_result::_Error`); they are listed under their real meaning.

**Static init and the API layer**

| Address | Reconstruction |
|---|---|
| 0x180001000 / 0x180010AB0 | static construction / destruction of `s_defaultAllocator` |
| 0x180001030 / 0x180010AD0 | static construction / destruction of `s_defaultAssertHandler` |
| 0x180005600 | `nvidia::NvGetAssertHandler` |
| 0x180005610 | `OpenLibrary` |
| 0x1800056B0 | `CloseLibrary` |
| 0x1800056D0 | `CreateContext` |
| 0x180005780 | `ReleaseContext` |
| 0x180005800 | `BeginAccumulation` |
| 0x180005830 | `RenderVolume` |
| 0x180005860 | `EndAccumulation` |
| 0x180005890 | `ApplyLighting` |
| 0x1800052D0 / 0x180005300 | `DefaultAllocator::allocate` / `deallocate` (malloc / free) |
| 0x180005530 | `DefaultAssertHandler::operator()` (empty) |
| 0x180005210-0x1800055E0 | allocator / assert handler (base) constructors, destructors, deleting destructors |
| 0x1800053D0 | `operator new(size_t, const char*, int)` (allocator, typeName "Gameworks Volumetric Lighting") |
| 0x180005420 | `operator delete(void*, const char*, int)` |
| 0x180005460 | `operator delete(void*)` (0x18000F180 = sized delete thunk) |
| 0x1800054A0 | `operator new[](size_t, const char*, int)` |
| 0x1800054F0 | `operator delete[](void*)` (0x18000F084 = sized delete[] thunk) |
| 0x180005180 / 0x1800051A0 | unreferenced helper: release and null a COM pointer, returns the refcount (not reconstructed) |

**Vector math** (`VectorMath.h/.cpp`)

| Address | Reconstruction |
|---|---|
| 0x180001060 / 0x1800010B0 | `float sqrt(float)` overload, used by `Length` |
| 0x180001080 | `memcpy` wrapper |
| 0x1800010D0 / 0x180001110 | `Vec2` ctor / copy |
| 0x180001150, 0x1800011A0, 0x1800011F0, 0x180001EE0 | `Vec3` ctors / copies |
| 0x180001240 | `LengthSq` |
| 0x180001290 | `Length` |
| 0x1800012B0 / 0x180001330 / 0x1800013B0 | `Vec3` `+`, `-`, `+=` |
| 0x180001420, 0x180001430, 0x180001490, 0x1800014F0, 0x180001550 | `Vec4` ctors (default, zero, xyzw, copy) |
| 0x1800015B0 | `Vec4::operator[]` |
| 0x1800015D0 / 0x180001660 / 0x1800016E0 | `Vec4` `+`, `* s`, `/ s` |
| 0x180001780 | `Vec4::xyz` |
| 0x1800017C0, 0x180001820, 0x1800018E0, 0x180001950, 0x1800019D0, 0x180001B80, 0x180001C00, 0x180001F10 | `Mat44` ctors (default, identity, zero, columns, floats, copy, NvcMat44) |
| 0x180001C80 | `Mat44 operator*` |
| 0x180001DA0 | `Transform(M, v)` |
| 0x180001D60 / 0x180001EC0 / 0x180001EB0 | element / column / data access |
| 0x180001F40 | `Inverse` |
| 0x180004120 | `Halton` |

**`ContextImp`**

| Address | Reconstruction |
|---|---|
| 0x180003930 | `ContextImp::ContextImp` |
| 0x1800038D0 / 0x1800038F0 | `~ContextImp` / deleting destructor |
| 0x1800039D0 | `ContextImp::BeginAccumulation` |
| 0x180003AB0 | `ContextImp::RenderVolume` |
| 0x180003BC0 | `ContextImp::EndAccumulation` |
| 0x180003C00 | `ContextImp::ApplyLighting` |
| 0x180003D90 / 0x180003DA0 / 0x180003DF0 | `GetOutputBufferWidth` / `GetOutputBufferHeight` / `GetOutputSampleCount` |
| 0x180003DB0 / 0x180003DD0 | `GetOutputViewportWidth` / `GetOutputViewportHeight` |
| 0x180003E00 | `GetInternalScale` |
| 0x180003E50 / 0x180003EA0 | `GetInternalBufferWidth` / `GetInternalBufferHeight` |
| 0x180003EF0 / 0x180003F40 | `GetInternalViewportWidth` / `GetInternalViewportHeight` |
| 0x180003F90 | `GetInternalSampleCount` |
| 0x180003FD0 / 0x180004010 | `IsOutputMSAA` / `IsInternalMSAA` |
| 0x180004050 | `GetFilterMode` |
| 0x180004060 | `GetJitter` |
| 0x1800041C0 | `GetCoarseResolution` |
| 0x180004230 | `SetupCB_PerContext` |
| 0x1800043A0 | `SetupCB_PerFrame` |
| 0x1800048E0 | `SetupCB_PerVolume` |
| 0x180004F60 | `SetupCB_PerApply` |

**Permutation keys** (`ShaderPermutations.h`: zeroing constructor / raw key operator)

| Address | Key |
|---|---|
| 0x180005920 / 0x180005940 | `Apply_PS_Desc` |
| 0x180005950 / 0x180005970 | `ComputeLightLUT_CS_Desc` |
| 0x180005980 / 0x1800059C0 | `DownsampleDepth_PS_Desc(bool msaa)` |
| 0x1800059D0 / 0x1800059F0 | `RenderVolume_VS_Desc` |
| 0x180005A00 / 0x180005A20 | `RenderVolume_HS_Desc` |
| 0x180005A30 / 0x180005A50 | `RenderVolume_DS_Desc` |
| 0x180005A60 / 0x180005A80 | `RenderVolume_PS_Desc` |
| 0x180005A90 / 0x180005AB0 | `Resolve_PS_Desc` |

**D3D11 descriptor helpers** (d3d11.h inlines)

| Address | Helper |
|---|---|
| 0x180005AC0 | `CD3D11_DEPTH_STENCIL_DESC(D3D11_DEFAULT)` |
| 0x180005BA0 | `CD3D11_BLEND_DESC(D3D11_DEFAULT)` |
| 0x180005C80 | `CD3D11_RASTERIZER_DESC(D3D11_DEFAULT)` |
| 0x180005D40 | `CD3D11_SAMPLER_DESC(D3D11_DEFAULT)` |
| 0x180005D20, 0x18000E150-0x18000E1D0 | default `CD3D11_*_DESC()` ctors |
| empty "lambda" functions | scope / unwind markers of these blocks (no code) |

**`ContextImp_D3D11`**

| Address | Reconstruction |
|---|---|
| 0x180005EA0 | `ContextImp_D3D11::Create` |
| 0x180005FE0 | `ContextImp_D3D11::CreateResources` |
| 0x180007980 | `ContextImp_D3D11::ContextImp_D3D11` |
| 0x180007D00 / 0x1800058E0 | `~ContextImp_D3D11` / deleting destructor |
| 0x180009740 | `BeginAccumulation_Start` |
| 0x180009920 | `BeginAccumulation_UpdateMediumLUT` |
| 0x180009B20 | `BeginAccumulation_CopyDepth` |
| 0x180009DA0 | `BeginAccumulation_End` |
| 0x180009E40 | `RenderVolume_Start` |
| 0x18000A120 | `RenderVolume_DoVolume_Directional` |
| 0x18000A8D0 | `RenderVolume_DoVolume_Spotlight` |
| 0x18000B700 | `RenderVolume_DoVolume_Omni` |
| 0x18000C020 | `RenderVolume_End` |
| 0x18000C070 | `EndAccumulation_Imp` |
| 0x18000C0A0 | `ApplyLighting_Start` |
| 0x18000C310 | `ApplyLighting_Resolve` |
| 0x18000C4D0 | `ApplyLighting_TemporalFilter` |
| 0x18000C6F0 | `ApplyLighting_Composite` |
| 0x18000CAE0 | `ApplyLighting_End` |
| 0x18000CB10 | `DrawFullscreen` |
| 0x18000CC50 | `DrawFrustumGrid` |
| 0x18000CDF0 | `DrawFrustumBase` |
| 0x18000CFC0 | `DrawFrustumCap` |
| 0x18000D1A0 | `DrawOmniVolume` |

**D3D11 resources** (`D3D11Util`)

| Address | Reconstruction |
|---|---|
| 0x18000D340-0x18000DA50, 0x18000DB80-0x18000DBE0 | `ConstantBuffer<T>`: Create / Map / Unmap / dtor / getCB / ctor for PerApply, PerVolume, PerFrame, PerContext |
| 0x1800095C0-0x180009680 | `ConstantBuffer<T>` deleting destructors |
| 0x18000DC00 / 0x18000DD10 / 0x18000DE20 / 0x18000DF30 / 0x18000E040 | `LoadShaders<PS / CS / VS / HS / DS>` |
| 0x18000E300 / 0x18000E350 / 0x18000E1F0 / 0x18000E240 / 0x18000E2A0 | `CreateShader` overloads |
| 0x18000E3B0 / 0x18000E400 | `Resource` ctor / dtor |
| 0x180005E50 / 0x180005E60 | `Resource::getSRV` / `getUAV` |
| 0x18000E4B0 | `RenderTarget::Create` |
| 0x18000E970 / 0x18000E9C0 / 0x180009700 | `RenderTarget` ctor / dtor / deleting dtor |
| 0x180005E70 | `RenderTarget::getRTV` |
| 0x18000EA20 | `DepthTarget::Create` |
| 0x18000EF60 / 0x18000EFC0 / 0x1800096C0 | `DepthTarget` ctor / dtor / deleting dtor |
| 0x180005E80 / 0x180005E90 | `DepthTarget::getDSV` / `getReadOnlyDSV` |

**Data**

| Address | Contents |
|---|---|
| 0x1800111D8 | `ContextImp` vtable: 16 slots |
| 0x1801FB898 | `ContextImp_D3D11` vtable: 0 dtor, 1-4 BeginAccumulation_*, 5-9 RenderVolume_*, 10 EndAccumulation_Imp, 11-15 ApplyLighting_* |
| 0x1801FB828 / 0x1801FB840 / 0x1801FB850 / 0x1801FB860 | allocator / assert handler vtables |
| 0x1801FFAC0 / 0x1801FFAC8 / 0x1801FFAD0 | `g_allocator` / `g_assertHandler` / `g_isLibraryOpen` |
| 0x1801FFAD8 / 0x1801FFAE0 | default assert handler / default allocator instances |
| 0x1801FE040-0x1801FF468 | the 12 shader tables (`tools/shader_tables.json`); the size tables sit next to the blobs |

## 5. Shader families and permutation axes

Table index = sum of `field << shift`. Every valid combination is present in the original tables
(269 blobs, 656 table slots).

| Family | Table (orig.) | Size | Axes (shift:bits = values) | Blobs |
|---|---|---|---|---|
| ps_Apply | 0x1801FF3C0 | 32 | SAMPLEMODE 0:1 (scene depth MSAA); UPSAMPLEMODE 1:2 (POINT, BILINEAR, BILATERAL); FOGMODE 3:2 (NONE, NOSKY = bDoFog && bIgnoreSkyFog, FULL = bDoFog) | 18 |
| cs_ComputeLightLUT | 0x1801FE250 | 16 | LIGHTMODE 0:1 (omni-style P only, spotlight P+S1+S2); ATTENUATIONMODE 1:2 (NONE, POLYNOMIAL, INV_POLYNOMIAL); COMPUTEPASS 3:1 (scatter + prefix sum, row carry) | 12 |
| ps_ComputePhaseLookup | 0x1801FF2E0 | 1 | none | 1 |
| ps_Debug | 0x1801FE040 | 1 | none | 1 |
| ps_DownsampleDepth | 0x1801FF370 | 2 | SAMPLEMODE 0:1 (scene depth MSAA) | 2 |
| vs_Quad | 0x1801FF2E8 | 1 | none | 1 |
| vs_RenderVolume | 0x1801FF380 | 8 | MESHMODE 0:3 (FRUSTUM_GRID, FRUSTUM_BASE, FRUSTUM_CAP, OMNI_VOLUME, GEOMETRY) | 5 |
| hs_RenderVolume | 0x1801FE050 | 64 | SHADOWMAPTYPE 0:1 (ATLAS, ARRAY); CASCADECOUNT 1:2 (1-4); VOLUMETYPE 3:1 (FRUSTUM, PARABOLOID); MAXTESSFACTOR 4:2 (16, 32, 64) | 48 |
| ds_RenderVolume | 0x1801FF2F0 | 16 | SHADOWMAPTYPE 0:1; CASCADECOUNT 1:2; VOLUMETYPE 3:1 | 16 |
| ps_RenderVolume | 0x1801FE2E0 | 512 | SAMPLEMODE 0:1 (internal MSAA); LIGHTMODE 1:2 (DIRECTIONAL, SPOTLIGHT, OMNI); PASSMODE 3:2 (GEOMETRY, SKY, FINAL); ATTENUATIONMODE 5:2; FALLOFFMODE 7:2 (NONE, FIXED, CUSTOM) | 162 |
| ps_Resolve | 0x1801FE2D0 | 2 | SAMPLEMODE 0:1 (internal MSAA) | 2 |
| ps_TemporalFilter | 0x1801FE048 | 1 | none | 1 |

How the C++ side selects permutations:

- **Directional.** HS/DS take {ATLAS (SIMPLE, CASCADE_ATLAS) or ARRAY, `uElementCount - 1`,
  FRUSTUM}. An element count of 0 is accepted for SIMPLE only; other layouts and more than 4
  elements return INVALID_PARAMETER. PS = {internal MSAA, DIRECTIONAL, pass, NONE}.
- **Spotlight.** HS/DS = {ATLAS, 1 cascade, FRUSTUM}. PS adds `eAttenuationMode` and `eFalloffMode`.
- **Point light.** HS/DS = {ARRAY, 1 cascade, PARABOLOID}. PS = {OMNI, `eAttenuationMode`}.
- **HS MAXTESSFACTOR** is always `eTessQuality`.
- **vs_RenderVolume MESHMODE 4 (GEOMETRY)** is compiled but never selected by the D3D11 backend.

Several axes don't change the generated code, which is why the original tables contain many
identical blobs:

- HS: the shadow map type and cascade count.
- PS: attenuation and falloff for directional and omni lights.
- PS: SAMPLEMODE for the GEOMETRY and SKY passes.
- CS: attenuation for pass 1.

### What the shaders do (derived from the disassembly)

**Quad VS.** A fullscreen triangle generated from `SV_VertexID`. TEXCOORD0 is the world position
from `g_mViewProjInv`; TEXCOORD1 is the uv.

**DownsampleDepth PS.** Writes `SV_DEPTH` from the scene depth.
- The lookup coordinate is evaluated per sample and offset by `g_vJitterOffset`, swapped to `.yx`
  on alternating checkerboard pixels.
- Single-sampled depth uses `SampleLevel(sPoint)`; MSAA depth uses `Load(int2(uv * g_vOutputSize), 0)`.

**ComputePhaseLookup PS.** Writes the 1x512 LUT with θ = π·v: the density-weighted sum of the phase
terms divided by the total density. The constants are taken as they appear in the blob:
- Isotropic: 1/4π.
- Rayleigh: 3/16π·(1+cos²)·(1 − cos⁴/8).
- Henyey-Greenstein: as usual.
- Mie hazy and murky: x = ((1−cos)/2)^8 or ^32, phase = (0.5 + k·x)/4π·(1 − x/2), with k = 4.5 or 16.5.

**RenderVolume VS.** Builds the volume mesh in light clip space and transforms it with
`g_mLightToWorld`, a divide by w, and `g_mViewProj`:
- Frustum grid: `res²` quad patches on z = 1.
- Base: 2 triangles.
- Cap: 4 side faces from the apex, each with `res` triangles plus one centre triangle, then 2 near
  triangles: `12·(res+1)+6` vertices.
- Omni: a cube of 6·`res²` patches, normalized to directions, with world position = direction ·
  `g_fLightZFar`.

**RenderVolume HS.** Passes the control points through (quad domain, integer partitioning, CCW
triangles). The patch constant function:
1. Culls a patch when the segment from the light's near point to every control point lies outside
   the view frustum.
2. Computes each edge factor as the larger of the factors for the points closest to the eye and for
   the control points. A factor is the projected edge length in pixels divided by
   `g_fTargetRaySize`, clamped to [1, max].
3. For frustums only, forces factor 1 on border edges with |clip.xy| ≥ 0.99951171875, which keeps
   the volume watertight with the cap.

**RenderVolume DS.** Bilinear patch interpolation.
- **Frustum:** searches the cascades from last to first (`g_mLightProj[i]`,
  `g_vElementOffsetAndScale[i]`, `g_uElementIndex[i]` for arrays). The lowest cascade with depth < 1
  wins. The point is unprojected with `g_mLightProjInv[i]` and pulled towards the eye by
  `g_fGodrayBias`.
- **Paraboloid:** maps with d = normalize(x, y, |z|), uv = d.xy/(d.z+1), using slice 0 when z > 0
  and slice 1 otherwise. The depth is mapped to lerp(zNear, zFar, depth).

**RenderVolume PS.** Computes the in-scattering integral along the view ray, signed by face
orientation in the GEOMETRY pass. SKY uses a ray length of `g_fZFar`; FINAL reconstructs the scene
position from the internal depth.
- **Directional:** `phaseLUT(acos(V·L)/π) · scatterPower · (1−exp(−σt·s))/σt`.
- **Omni:** one LUT_P sample, with u = distance along the ray and v = angle to the light.
- **Spotlight:**
  1. Intersects the ray analytically with the cone to get [t0, t1].
  2. Then, by falloff mode:
     - NONE: `P(t1) − P(t0)`.
     - FIXED: a combination of P, S1 and S2 (linear angular falloff).
     - CUSTOM: 9-tap Simpson integration with the phase LUT, attenuation, a `pow` falloff and
       extinction.
- The result is multiplied by `g_vLightIntensity`; alpha is always 0.

**ComputeLightLUT CS.** Writes 256x512 RGBA16F LUTs. x is the distance along the view ray in
[|eye−light| − zFar, |eye−light| + zFar]; y is the angle between the view ray and the eye-to-light
direction.
- **Pass 0** (32x8 threads, Dispatch(8,64,1)): computes the in-scattering per sample (law of cosines,
  extinction, phase LUT, attenuation), then runs a Hillis-Steele prefix sum per 32-texel row in
  groupshared memory. S1 = scattering / r and S2 = t·S1.
- **Pass 1** (32x4 threads, Dispatch(1,128,1 or 3)): carries the row sums across the 32-texel blocks.

**Resolve PS.** A weighted 3x3 (x samples) filter, weight = max(1 − |offset·resMultiplier|²/64, 0)^32,
that skips off-screen and non-finite taps. It writes colour and the first and second moments of the
linear depth.

**TemporalFilter PS.** The current value is a 3x3 tent in tonemapped YCoCg. The history, reprojected
with `g_mHistoryXform`, is luma-clipped to the neighbourhood. The blend factor is
`g_fHistoryFactor`, reduced by motion (relative to `g_fFilterThreshold`) and by depth-moment
disagreement, and capped at 0.98.

**Apply PS.** Dual-source output: in-scattering on SV_TARGET0 and transmittance on SV_TARGET1.
- The light buffer is upsampled by point, bilinear or bilateral filtering. The bilateral filter
  weights 3x3 taps by distance and by agreement with the depth moments.
- Optional analytic fog: T = exp(−z·zFar·σt), with in-scatter += multiScatter · fogLight ·
  scatterPower · (1−T)/σt. NOSKY skips the sky.

## 6. D3D11 usage

The DLL imports nothing from d3d11/dxgi; all calls go through the COM vtables of the device passed in
`PlatformDesc` and the context passed in each Args struct. Vtable offsets map to methods in d3d11.h
declaration order.

ID3D11Device:

| Offset | Method |
|---|---|
| +24 | CreateBuffer |
| +40 | CreateTexture2D |
| +56 | CreateShaderResourceView |
| +64 | CreateUnorderedAccessView |
| +72 | CreateRenderTargetView |
| +80 | CreateDepthStencilView |
| +96 | CreateVertexShader |
| +120 | CreatePixelShader |
| +128 | CreateHullShader |
| +136 | CreateDomainShader |
| +144 | CreateComputeShader |
| +160 | CreateBlendState |
| +168 | CreateDepthStencilState |
| +176 | CreateRasterizerState |
| +184 | CreateSamplerState |
| +296 | GetFeatureLevel |

ID3D11DeviceContext:

| Offset | Method |
|---|---|
| +56 | VSSetConstantBuffers |
| +64 | PSSetShaderResources |
| +72 | PSSetShader |
| +80 | PSSetSamplers |
| +88 | VSSetShader |
| +104 | Draw |
| +112 | Map |
| +120 | Unmap |
| +128 | PSSetConstantBuffers |
| +136 | IASetInputLayout |
| +144 | IASetVertexBuffers |
| +152 | IASetIndexBuffer |
| +192 | IASetPrimitiveTopology |
| +200 | VSSetShaderResources |
| +208 | VSSetSamplers |
| +264 | OMSetRenderTargets |
| +280 | OMSetBlendState |
| +288 | OMSetDepthStencilState |
| +328 | Dispatch |
| +344 | RSSetState |
| +352 | RSSetViewports |
| +400 | ClearRenderTargetView |
| +424 | ClearDepthStencilView |
| +472 | HSSetShaderResources |
| +480 | HSSetShader |
| +488 | HSSetSamplers |
| +496 | HSSetConstantBuffers |
| +504 | DSSetShaderResources |
| +512 | DSSetShader |
| +520 | DSSetSamplers |
| +528 | DSSetConstantBuffers |
| +536 | CSSetShaderResources |
| +544 | CSSetUnorderedAccessViews |
| +552 | CSSetShader |
| +560 | CSSetSamplers |
| +568 | CSSetConstantBuffers |

Notable call arguments that the decompiler had elided were recovered from the disassembly:

- `ClearDepthStencilView(dsv, DEPTH|STENCIL, 1.0f, 0)` in CopyDepth.
- `ClearDepthStencilView(dsv, STENCIL, 1.0f, 0xFF)` per volume.
- The composite blend factor is `{fBlendfactor x4}`.
- All integer-to-float conversions of sizes are unsigned.

## 7. Original behaviour reproduced as-is (bugs and quirks)

* **Shader table release.** The context destructor applies a `sizeof(x)/sizeof(x[0])` array-release
  macro to the heap-allocated `T**` shader tables. That count is 1, so only permutation 0 of each
  table is released; the other shader objects leak when a context is destroyed.
* **TemporalFilter binding.** `ApplyLighting_TemporalFilter` prepares 5 SRVs but binds 4. So
  `tLastDepth` (t3) is always null and the history depth moments are never read.
* **Composite depth input.** `ApplyLighting_Composite` binds the filtered depth (t2, bilateral
  upsampling) only when temporal filtering is on. Without it, t2 is null even if a resolved depth
  exists.
* **Uninitialized members.** `pAccumulatedOutput_` (+576) and the unused DSS slot (+664) are not
  initialized by the constructor.
* **`CreateContext` failure path.** `CreateResources` is called on the result of `NV_NEW` without a
  null check, and partially created resources are released by deleting the context.
* **cbVolume field.** `g_fLightToEyeDepth` is never written; whatever the mapped (discarded) buffer
  memory holds is uploaded.
* **`SetupCB_PerVolume`** always copies all 4 shadow map elements, regardless of `uElementCount`.
* **Light LUT CS** (as written in the blobs):
  - Pass 1 only processes columns 0-127, while pass 0 writes 256 columns and divides x by 127.
  - There are no group barriers, so the code relies on lockstep execution of the 32 threads of a row.
  - Alpha is scaled by 4 in pass 1.
* **Simpson integration.** The CUSTOM-falloff Simpson sum in the volume PS is scaled by 6 relative to
  the textbook formula.
* **Build differences.** The original was compiled without optimization: debug-style code, no
  inlining, static (non-inlined) bit-field helpers. The reconstruction builds with the default
  Release flags; this changes the machine code but not the behaviour. `/d2FH4-` is used so the DLL
  imports `__CxxFrameHandler3` like the original, without the VCRUNTIME140_1 dependency.

## 8. Shader source notes

* The original cbuffers were declared with explicit `packoffset`. This is visible in the code:
  with packoffset, fxc writes `1 - cb` as `add r, l(1.0), -cb[i].x`, and without it the operands are
  swapped. Switching `ShaderCommon.hlsli` to packoffset declarations fixed the last mismatches in
  ds_RenderVolume, vs_RenderVolume[2] and ps_Resolve[0].
* The post-process pixel shaders interpolate TEXCOORD1 per sample (`sample` modifier, so the blobs
  run at sample frequency). `PostProcess.hlsli` provides `PS_QUAD_INPUT`.
* Several commutative operand orders, the `[flatten]` on the last phase-function branch and the
  uppercase `SV_TESSFACTOR` semantics are required for byte-identical output.

## 9. Verification

### (a) Shaders

`python tools/shaders.py check` compiles all 269 permutations into
`recon/build/nvvl_check/<family>/<index>.dxbc` (+ `.asm` from `fxc /dumpbin`), compares them with
the original blobs and writes `recon/build/nvvl_check/report.json`.

| Family | Perms | Byte-identical | Header identical | Code identical |
|---|---|---|---|---|
| cs_ComputeLightLUT | 12 | 12 | 12 | 12 |
| ds_RenderVolume | 16 | 16 | 16 | 16 |
| hs_RenderVolume | 48 | 48 | 48 | 48 |
| ps_Apply | 18 | 18 | 18 | 18 |
| ps_ComputePhaseLookup | 1 | 1 | 1 | 1 |
| ps_Debug | 1 | 1 | 1 | 1 |
| ps_DownsampleDepth | 2 | 2 | 2 | 2 |
| ps_RenderVolume | 162 | 162 | 162 | 162 |
| ps_Resolve | 2 | 2 | 2 | 2 |
| ps_TemporalFilter | 1 | 1 | 1 | 1 |
| vs_Quad | 1 | 1 | 1 | 1 |
| vs_RenderVolume | 5 | 4 | 5 | 4 |
| **Total** | **269** | **268 (99.6 %)** | **269** | **268** |

"Header" means the signatures, resource bindings and cbuffer layouts in the reflection listing.

The only mismatch is `vs_RenderVolume[3]` (MESHMODE_OMNI_VOLUME, blob 0x1801DB0E0). It has the same
instructions and dataflow, but fxc assigns different temporaries in 6 lines: the face-sign and uv
vectors swap halves of r0, and r1.zw is used instead of r3.xy. Rewriting the face selection,
statement order and vector packing in a dozen ways did not change the allocation.

The rebuilt DLL embeds 268 of the 269 original blobs verbatim, confirmed by a byte search over the
built DLL.

### (b) DLL

- Build: no warnings at /W3.
- Exports: `NvVolumetricLighting.d3d11.rebuilt.def` is identical to `.original.def` (9 exports,
  identical mangled names).
- Imports: VCRUNTIME140 (incl. `__CxxFrameHandler3`), api-ms-win-crt heap/math (`malloc`, `free`,
  `exp`, `sqrtf`)/runtime, and KERNEL32. No d3d11/dxgi, same as the original.
- Note: the Visual Studio build prints `'pwsh.exe' is not recognized` after linking. This comes from
  the machine-wide vcpkg MSBuild integration (applocal step), not from this project.

### (c) Smoke test

`NvVolumetricLighting.smoketest.exe <dll> [ppm prefix]` creates a WARP device and loads the DLL by
its mangled names. It renders a synthetic scene (scene depth with a box, shadow maps with an
occluder) with one directional, one spot and one point light, for 3 frames, in 4 configurations:

1. Half resolution, no filter, bilinear upsampling, FIXED falloff.
2. Full resolution, scene MSAA2 + internal MSAA2, temporal filter, bilateral upsampling, fog NOSKY,
   NONE falloff, 3-cascade atlas.
3. Quarter resolution, internal MSAA4, point upsampling, fog FULL, CUSTOM falloff, 4-slice cascade
   array, NO_BLENDING.
4. Half resolution, scene MSAA4, temporal filter, WIREFRAME, CUSTOM falloff with inverse-polynomial
   attenuation, 2-slice array.

It prints a hash of every composited image and checks the allocator balance. The original and the
rebuilt DLL produce identical hashes in all 4 configurations (combined `05fc4ae02b3c4846`), identical
images, and the same 118 allocations, all released after ReleaseContext. The scene is synthetic and
chosen for path coverage, not for looks; the comparison is bit equality between the two DLLs.

## 10. Open questions

* **Public header and Args struct names.** The public header could not be retrieved, so the
  descriptor names follow the known NvVolumetricLighting naming. The "Args" struct members that the
  D3D11 backend never reads (`BeginAccumulationArgs+32`, `ApplyLightingArgs+24/+32`) are unknown.
  They may be D3D12 or other-platform fields, or resource objects paired with the views.
* **Unused states.** `dss_TestDepth_`, `dss_RenderVolume_NoDepth_` and the never-created slot +664
  are unused by this backend. They are probably used by a different path, such as a light-volume
  path without depth.
* **ShadowMapLayout::PARABOLOID** is assumed to be 3. The point-light path never reads the layout
  value; it always uses the 2-slice array path.
* **vs_RenderVolume MESHMODE_GEOMETRY** (index 4) has no caller.
* **vs_RenderVolume[3] register allocation.** The exact source form behind it is unresolved (see 9a).
* **Light LUT CS questions** (section 7): the 128 vs 256 column mismatch, the missing barriers and
  the factor 4.
