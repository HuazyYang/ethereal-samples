# 2018 framework pass shaders: reconstruction notes

Source: `assets/shaders_split/framework/<shader>/<permutation>.dxil(.txt)` (DXC 1.2, SM 6.0, full reflection).
These are the geometry-pass shaders of the 2018 donut framework that the demo pixel shaders
(`demo/gbuffer_ps`, `demo/forward_ps`, `demo/material_id_ps`) are paired with. Donut main's versions cannot be
used because their vertex layouts and cbuffers differ (donut main: `SceneVertex{pos, prevPos, texCoord, normal,
tangent(float4)}` and split view constants; 2018: `SceneVertex{POS, UV, NORMAL, TANGENT, BITANGENT}`,
`GBufferFillConstants` with explicit matrices, monolithic `ForwardShadingConstants`).

| original (ShaderFactory path under `shaders/framework`) | reconstructed file (relative to `asteroids/shaders`) | stage / entry | permutations |
|---|---|---|---|
| `passes/gbuffer_vs.hlsl` | `framework/passes/gbuffer_vs.hlsl` | vs / `main` | `SINGLE_PASS_STEREO={0,1} MOTION_VECTORS={0,1}` |
| `passes/forward_vs.hlsl` | `framework/passes/forward_vs.hlsl` | vs / `main` | `SINGLE_PASS_STEREO={0,1}` |
| `passes/forward_gs.hlsl` | `framework/passes/forward_gs.hlsl` | gs / `main` | `MOTION_VECTORS={0,1}` |
| `passes/cubemap_gs.hlsl` | `framework/passes/cubemap_gs.hlsl` | gs / `main` | none |
| `passes/material_id_ps.hlsl` | `framework/passes/material_id_ps.hlsl` | ps / `main`, `main_alpha_tested` | none |
| `passes/depth_vs.hlsl` | `framework/passes/depth_vs.hlsl` | vs / `main` | none |
| `passes/depth_ps.hlsl` | `framework/passes/depth_ps.hlsl` | ps / `main` | none |
| | `framework/framework_cb.h` | HLSL + C++ | `DepthPassConstants`, `FRAMEWORK_SPS_VIEWPORT_MASK` |
| | `framework/forward_vertex_2018.hlsli` | HLSL | `SceneVertex` (2018 layout) |
| | `framework/framework_shaders.cfg` | ShaderMake | one line per source, original permutation sets, `-m 6_0` |

C++ loads them as `"framework/passes/<name>.hlsl"` from the asteroids shader output (the ShaderMake cfg paths).
`GBufferFillConstants`, `ForwardShadingConstants` and `MaterialConstants` come from `demo/include/surface_cb.h`
(shared with the demo pixel shaders); `material_id_ps` includes `demo/include/scene_material_2018.hlsli`.
The passes headers are included as `"../framework_cb.h"` / `"../forward_vertex_2018.hlsli"` so they resolve
with the ShaderMake include dirs (`asteroids/shaders`, `asteroids/shaders/demo/include`).

## Interface for the C++ passes

Vertex input (all `float` formats; the instance transform is a row-major float3x4 split over three float4
attributes with semantic indices 0..2, one signature element of 3 rows):

| shader | inputs (semantic → format) |
|---|---|
| gbuffer_vs, forward_vs | `POS` RGB32_FLOAT, `UV` RG32_FLOAT, `NORMAL` RGB32_FLOAT, `TANGENT` RGB32_FLOAT, `BITANGENT` RGB32_FLOAT (per vertex); `TRANSFORM0..2` RGBA32_FLOAT (per instance); gbuffer_vs with `MOTION_VECTORS=1` also `PREV_TRANSFORM0..2` RGBA32_FLOAT (per instance); `SV_InstanceID` declared, unused |
| depth_vs | `POSITION` RGB32_FLOAT, `UV` RG32_FLOAT, `TRANSFORM0..2` RGBA32_FLOAT (per instance), `SV_InstanceID` unused |

The 2018 ctor 0x140087630 builds this layout with the strings NORMAL/TANGENT/BITANGENT/TRANSFORM/PREV_TRANSFORM;
buffer slots/strides are a C++ concern (the shader only fixes semantics and formats).

Constant buffers / resources:

| shader | bindings |
|---|---|
| gbuffer_vs | `cbuffer c_GBuffer : b0 { GBufferFillConstants c_GBuffer; }` (432 bytes). Note: the demo gbuffer_ps reads the same struct at **b1**. |
| forward_vs | `cbuffer c_Forward : b0 { ForwardShadingConstants g_Forward; }` (5552 bytes). The demo forward_ps reads it at **b1**. |
| depth_vs | `cbuffer c_Depth : b0 { DepthPassConstants g_Depth; }` (64 bytes, `matWorldToClip`) |
| depth_ps | `t_Albedo` Texture2D t0, `s_MaterialSampler` s0 (no cbuffer) |
| material_id_ps `main` | `c_Material` b0 |
| material_id_ps `main_alpha_tested` | `c_Material` b0, `t_Diffuse` t0, `s_MaterialSampler` s0 |
| forward_gs, cubemap_gs | none |

Fields used: gbuffer_vs uses `matWorldToView`, `matViewToClip`, and with stereo `matWorldToViewRight`,
`matViewToClipRight`; `matWorldToClipPrev`, `viewportScale/BiasPrev`, `pixelOffset` are used by the demo
gbuffer_ps (motion vectors). forward_vs uses `matWorldToClip` and, with stereo, `worldToClipXRight`.

Stage outputs and pairing:

- gbuffer_vs out: `SV_Position, POS, UV, NORMAL(centroid), TANGENT(centroid), BITANGENT(centroid)`,
  `[PREV_WORLD_POS]` (MOTION_VECTORS), `[NV_X_RIGHT float4, NV_VIEWPORT_MASK uint4 nointerpolation]` (stereo).
  With MOTION_VECTORS=1 this matches the demo `gbuffer_ps` (_ASTEROIDS=0) input exactly. The PS always declares
  PREV_WORLD_POS, so the non-stereo gbuffer path must use MOTION_VECTORS=1 (the PS never declares the NV_* stereo
  outputs; with stereo the fast GS strips/keeps them).
- forward_vs out: `SV_Position, POS, UV, NORMAL, TANGENT, BITANGENT`, `[NV_X_RIGHT, NV_VIEWPORT_MASK]`. The demo
  forward_ps and material_id_ps (_ASTEROIDS=0) additionally declare PREV_WORLD_POS (register 6, never read).
  forward_vs does not write it, so pairing forward_vs with those demo pixel shaders relies on the runtime
  tolerating an unread, unwritten trailing PS input; the safe pairing is **gbuffer_vs MOTION_VECTORS=1**, whose
  output signature is identical to the demo PS input. Check the D3D12 debug layer when wiring
  (see open question 1).
- forward_gs (SPS fast GS): input = VS output with `[PREV_WORLD_POS]` + `NV_X_RIGHT` + `NV_VIEWPORT_MASK`; output adds
  `SV_ViewportArrayIndex` (=0) after PREV_WORLD_POS. MOTION_VECTORS must match the VS permutation.
- cubemap_gs: input `SV_Position + SceneVertex` (forward_vs SINGLE_PASS_STEREO=0 output), output adds
  `SV_ViewportArrayIndex` = 6-bit cube face mask.
- material_id_ps input: `SV_Position + SceneVertex` (no PREV_WORLD_POS) → pairs with forward_vs
  SINGLE_PASS_STEREO=0 (or gbuffer_vs MOTION_VECTORS=0, which has the same outputs).

Both geometry shaders are NVAPI **fast geometry shaders** (maxvertexcount 1, only vertex 0 is read and emitted).
They only work with the NVIDIA extension: nvrhi `ShaderDesc::fastGSFlags`
(forward_gs: `ForceFastGS | UseViewportMask` plus custom semantics `NV_X_RIGHT` = XRight and
`NV_VIEWPORT_MASK` = ViewportMask, i.e. single-pass stereo; cubemap_gs: `ForceFastGS | UseViewportMask |
OffsetTargetIndexByViewportIndex` and per-face `pCoordinateSwizzling`, as donut's CubemapView path does).
On a plain D3D12 GS a one-vertex triangle strip emits nothing. The asteroids demo renders mono, so the
stereo permutations and forward_gs are only needed if single-pass stereo is reinstated.

## Per shader

- **gbuffer_vs**: `o_vtx = i_vtx`, then position, normal, tangent and bitangent are transformed by
  `mul(instanceMatrix, float4(v, 1|0))`. The previous world position uses the previous instance transform with
  the *current* object position (2018 has no prevPos vertex attribute). Clip = `mul(mul(world, matWorldToView),
  matViewToClip)`. Stereo: right clip = `mul(mul(world, matWorldToViewRight), matViewToClipRight)`, viewport mask
  constant 0x00020001 in all four components. Provenance: donut 2021 gbuffer_vs structure, 2018 SceneVertex and
  cbuffer lifted from the DXIL.
- **forward_vs**: same transforms, clip = `mul(world, g_Forward.matWorldToClip)`; stereo right position is the left
  clip position with x = `dot(world, worldToClipXRight)` (NVIDIA SPS convention: only X differs).
- **forward_gs**: pure passthrough of vertex 0 with `SV_ViewportArrayIndex = 0`. Struct names
  `VertexShaderOutput` / `GeometryShaderOutput` and field names (`position`, `vtx`, `prevWorldPos`,
  `positionRight`, `viewportMask`, `viewport`) are from the type annotations.
- **cubemap_gs**: identical to donut 2021 `cubemap_gs.hlsl` (VSOutput/GSOutput/Passthrough/ViewportMask names match
  the annotations), with the 2018 SceneVertex.
- **material_id_ps**: `main` writes `g_Material.materialID`; `main_alpha_tested` runs `EvaluateSceneMaterial` and
  `clip(opacity - 0.5)` first (only the diffuse alpha survives dead-code elimination). Entry names are from the
  DXIL entry-point metadata.
- **depth_vs / depth_ps**: depth-only (shadow) pass; the PS clips at albedo alpha 0.5 and has no cbuffer
  (unlike donut 2021, which evaluates the material and uses alphaCutoff).

## Verification

`build/shader_check/framework/check_framework.py` (logs `check_hv2018.log`, `check_hv2021.log`) compiles every
original permutation with DXC 1.8 `-T <stage>_6_0` using the ShaderMake include dirs and compares the dumps.

| check | result (13 permutations, -HV 2018 and -HV 2021) |
|---|---|
| input/output signatures incl. semantic indices, registers, masks, system values, interpolation modes | identical |
| resource bindings (name, type, ID, register) | identical |
| cbuffer layouts | identical |
| dx.op histogram after known differences | identical (`ops~:OK` everywhere) |
| GS state (input triangle, output triangle strip, maxvertexcount 1, stream mask 1, 1 instance) | identical |

Known DXC 1.2 → 1.8 differences: `FMad(1.0|0.0, m, acc)` from `mul(.., float4(v, 1|0))` becomes fadd / is folded
(FMad count lower, fadd count higher by the same amount); DXC 1.2 kept the storeOutputs of `o_vtx = i_vtx` for
NORMAL/TANGENT/BITANGENT that are overwritten right after (9 extra StoreOutput in the original VS); cubemap_gs
`if (...) face_mask |= bit` compiles to selects instead of branches. All permutations also compile with `-WX`
under HLSL 2021 (ShaderMake default). `framework_cb.h` passes `clang++ -std=c++17 -fsyntax-only` with donut math.

## Open questions

1. Which VS the 2018 passes pair with the demo pixel shaders (the demo forward_ps / material_id_ps for
   _ASTEROIDS=0 declare PREV_WORLD_POS, which only gbuffer_vs MOTION_VECTORS=1 writes). The C++ ctors
   0x140087630 (GBuffer fill, uses gbuffer_vs + forward_gs/cubemap_gs) and 0x1400840B0 (forward, forward_vs +
   forward_gs with MOTION_VECTORS) decide this; the shaders themselves fix only the signatures listed above.
2. `NV_VIEWPORT_MASK = 0x00020001` is reproduced as a constant; its exact meaning is defined by the NVAPI
   single-pass-stereo extension (viewport 0 for the left eye, viewport 1 for the right eye) and is not needed for
   mono rendering.

# Light probe, tone mapping and bloom passes (second batch)

| original | reconstructed file | stage / entry | permutations |
|---|---|---|---|
| `passes/light_probe.hlsl` | `framework/passes/light_probe.hlsl` | gs / `cubemap_gs`; ps / `mip_ps`, `diffuse_probe_ps`, `specular_probe_ps`, `environment_brdf_ps` | none |
| `passes/histogram_cs.hlsl` | `framework/passes/histogram_cs.hlsl` | cs / `main` | `HISTOGRAM_BINS=256 SOURCE_ARRAY={0,1}` |
| `passes/exposure_cs.hlsl` | `framework/passes/exposure_cs.hlsl` | cs / `main` | `HISTOGRAM_BINS=256 SOURCE_ARRAY={0,1}` (identical binaries) |
| `passes/tonemapping_ps.hlsl` | `framework/passes/tonemapping_ps.hlsl` | ps / `main` | `HISTOGRAM_BINS=256 SOURCE_ARRAY={0,1}` |
| `passes/bloom_ps.hlsl` | `framework/passes/bloom_ps.hlsl` | ps / `main` | none |
| | `framework/framework_passes_cb.h` | HLSL + C++ | `LightProbeConstants`, `ToneMappingConstants`, `BloomConstants` (C++: `namespace framework2018`) |

The light_probe `cubemap_gs` (instanced fullscreen-triangle replication, writes SV_RenderTargetArrayIndex) is a
different shader from `passes/cubemap_gs.hlsl` (fast-GS viewport mask).

Differences from donut 2021 that are reproduced on purpose:
- light_probe: `GenerateBasis` bends `up` towards N near +-Y; diffuse/specular rotate the tangent frame per pixel
  by `frac(sin(dot(pos.xy, (12.9898, 78.233))) * 43758.5453) * 3.1415`; diffuse weight `NdotL / 2pi`, divided by
  the sum of NdotL; specular samples GGX with alpha = roughness^2 but its pdf uses D with alpha = roughness;
  `environment_brdf_ps` reflects **-N** instead of -V about H (so LdotH == NdotH): the LUT is ~G_Smith/NdotV in R
  (up to 7.47) and ~0 in G, which is exactly what `assets/media/EnvironmentBrdf.dds` contains.
- exposure_cs: UE4-style average without outliers (`ComputeHistogramSum`, `ComputeEyeAdaptationExposure`,
  `ComputeEyeAdaptation(old, target, frameTime)` survive as inlined function names), not donut 2021's cdf window.
- bloom: `BloomConstants` has `numSamples` at offset 16 and `padding` at 20 (donut main swapped them).
- histogram_cs and tonemapping_ps are identical to donut 2021.
- GenerateBasis uses `(a > b) & (a > c)` instead of `&&` so that HLSL 2021 produces the same single `and i1` branch.

Verification (`build/shader_check/framework2/`): `check_framework2.py` (logs `check_hv2018.log`,
`check_hv2021.log`): 12/12 permutations identical in signatures (incl. interpolation modes), bindings, cbuffer
layouts, entry properties (GS: triangle in, maxvertexcount 3, 6 instances; CS numthreads 16x16x1 / 1x1x1) and the
adjusted op histogram, under both -HV 2018 and -HV 2021; all float literals in the bodies match. Remaining raw
differences are the known DXC 1.2 -> 1.8 ones: normalize lowering (Sqrt+fdiv vs Dot3+Rsqrt), cbuffer reload CSE
(specular recomputes roughness^2 from a reload, +1 fmul in the original), exposure's speed-up/down choice is a
select instead of two blocks (3 fewer `br`), bloom `x^2 * log2e * argumentScale` reassociated (1 fewer fmul).
`wx_compile.sh` (-WX, default HLSL 2021, ShaderMake include dirs) and ShaderMake itself on the cfg lines
(`shadermake_test.log`) build all 12 as SM 6.0. `envbrdf_check.py` evaluates environment_brdf_ps in numpy against
EnvironmentBrdf.dds: all 4096 texels within 0.1 % (R) / 6e-8 absolute (G) with uv = ((x+0.5)/64, (y+0.5)/64),
uv.x = NdotV, uv.y = roughness, row 0 = roughness ~0 (fullscreen_vs UV at pixel centres; no flip).
`framework_passes_cb.h` compiles as C++17 next to donut main's `light_probe_cb.h`/`tonemapping_cb.h`/`bloom_cb.h`.

Follow-up (GPU check with the C++ passes): light_probe.hlsl now spells every `normalize` as `normalize2018`
(`v / sqrt(v.x*v.x + v.y*v.y + v.z*v.z)`), which DXC 1.8 lowers like DXC 1.2 did (fmul/fadd, Sqrt, 3 fdiv); mip_ps
and environment_brdf_ps now match the original op histogram exactly, diffuse/specular differ only in the cbuffer
reload CSE. Reason: with the intrinsic (Dot3 + Rsqrt) environment_brdf_ps rendered rows 0-2 (roughness < 0.04,
H ~ N) up to 2.4x too low on the GPU. Running the shipped 2018 DXIL blobs in place of ours on the same GPU gives:
diffuse/specular bit-identical to ours, EnvironmentBrdf identical in 4094/4096 texels (max 0.0039). Against the
2018 EnvironmentBrdf.dds the remaining difference (128 texels, rows 0-1, e.g. [0,0] 7.238 vs 7.473) is produced by
the original DXIL as well, i.e. it is GPU/driver arithmetic, not the reconstruction.

## passes/taa_cs (TAA resolve)

`passes/taa_cs.hlsl` + `taa_cb_2018.h`, reconstructed from the 8 permutations (SAMPLE_COUNT 1/2/4/8 x
USE_CATMULL_ROM_FILTER 0/1; the build makes SAMPLE_COUNT=1 only). It is the first public donut's `taa_cs.hlsl`
(5948981) minus its later additions:
- no PQ encode/decode (`pqC`, `invPqC` are not in the constants): variance clipping and blending run in linear HDR;
- one output (`u_Output`, u0); the history is `t_PrevFilteredRT` (t2), the previous frame's output, not a separate
  feedback texture;
- the history is sampled whenever its position lies inside the previous view (the C++ shrinks it by a 1 pixel
  margin), without the `newFrameWeight < 1` condition of the 2021 shader;
- constants (128 bytes): reprojectionMatrix (motion vector pass), previousViewOrigin/Size, viewOrigin/viewSize,
  sourceTextureSizeInv, clampingFactor (< 0 = no clamp), newFrameWeight, stencilMask, padding.

DXIL check (`SAMPLE_COUNT=1, USE_CATMULL_ROM_FILTER=1`): resource bindings identical, the `dx.op` histogram identical
op for op (barrier 1, FMax 9, FMin 3, SampleLevel 9, TextureLoad 4, ...); the only difference is one duplicate
constant-buffer load (the original reads row 6 twice, once for .zw).
