# Surface shading shaders: reconstruction notes

Source: `assets/shaders_split/demo/<shader>/<permutation>.dxil(.txt)` (DXC 1.2, ps_6_0, full reflection,
no debug info). All six shaders are reconstructed from the DXIL. Code that also exists in the 2021 public
donut (`git -C D:\ps\repo\ethereal\donut show 5948981:...`) was taken from there and then corrected to what the
DXIL does.

| file | entry | permutations (original set) | original |
|---|---|---|---|
| `forward_ps.hlsl` | `main` | `IS_SHIP={0,1} _ASTEROIDS={0,1}` | `forward_ps` (4) |
| `gbuffer_ps.hlsl` | `main` | `IS_SHIP={0,1} _ASTEROIDS={0,1}` | `gbuffer_ps` (4) |
| `material_id_ps.hlsl` | `main` | `IS_SHIP={0,1} _ASTEROIDS={0,1}` | `material_id_ps` (4) |
| `deferred_lighting_ps.hlsl` | `main` | none | `deferred_lighting_ps/default` |
| `shield_ps.hlsl` | `main` | none | `shield_ps/default` |
| `accumulation_ps.hlsl` | `main` | none | `accumulation_ps/default` |
| `include/surface_cb.h` | | HLSL + C++ | MaterialConstants, GBufferFillConstants, ForwardShadingConstants, DeferredLightingConstants, ShieldConstants, `LightType_*` |
| `include/surface_2018.hlsli` | | HLSL | `SurfaceParams` (116 bytes, from the type annotation), `square()` |
| `include/scene_material_2018.hlsli` | | HLSL | material bindings, `SceneVertex`, `EvaluateSceneMaterial`, `RgbToNormal`, Toksvig roughness |
| `include/asteroid_material.hlsli` | | HLSL | `EvaluateAsteroidsMaterial`, `fbmd`/`noised`/`hash`, `colorRamp`, `safeNormalize`, LOD dither (`t_RandomsTexture`) |
| `include/lighting_2018.hlsli` | | HLSL | `ShadeSurface`, `GGX`, `slerp`, `GetIncidentVector`, `GetLightProbeWeight`, `EvaluateShadowGather16`, `EvaluateShadowPoisson`, `ShadowWithMaxDistance`, `sharpen` |
| `surface_shaders.cfg` | | ShaderMake | one line per source, original permutation sets |

The names of inlined functions are known from the mangled exit labels in the DXIL (`EvaluateSceneMaterial`,
`EvaluateAsteroidsMaterial`, `RgbToNormal`, `fbmd`, `safeNormalize`, `colorRamp`, `GetMotionVector`,
`GetIncidentVector`, `ShadeSurface`, `EvaluateShadowGather16`, `EvaluateShadowPoisson`, `ShadowWithMaxDistance`,
`sharpen`, `GetSurfaceParams`, `raySphereIntersections`, `shadeShield`). The signatures come from the
mangling. Most local names come from the phi names (`surface.N`, `localNormal`, `albedo`, `m`/`d`/`a`/`b`
in fbmd, `shadow`, `cascade`, `objectShadow`, `diffuseTerm`, `lightProbeWeight`, `incidentVector`,
`halfAngularSize`, `attenuation`, `spotlight`, `bubbleColor`, `color`, `X`/`Y`/`nSample`/`TotalColor`/`Pixel`,
the two `palette` statics). The remaining helper and variable names are invented, for example `IsInTextureRect`,
`GetShipEmissive`, `AdjustRoughnessToksvig`, `LodTransitionDither`, `shadeHexGrid`, `hexagonEdgeDistance`,
`valueNoise`, `LinearToPQ`/`PQToLinear` and `Lanczos2`.

## Shared headers

- `surface_cb.h` follows the donut `*_cb.h` convention: it compiles as HLSL and, after
  `#include <donut/core/math/math.h>` and `using namespace donut::math;`, as C++. Matrices are declared
  `row_major` through `SURFACE_ROW_MAJOR`, like `meshlet_cb.h`. `static_assert`s check all sizes and array offsets. Checked with
  `clang++ -std=c++17 -fsyntax-only` against `external/donut/include` (together with `meshlet_cb.h`).
- Headers from other groups are reused, not duplicated:
  - `light_cb.h` (LightConstants / ShadowConstants / LightProbeConstants, the 2018 layout) is included by `surface_cb.h`.
  - `meshlet_cb.h` provides `FrameCB` (cbFrame) and `ObjectConstants` (cbObjectInfo). Because it already existed,
    no `surface_frame_cb.h` copy was written. The shaders declare `cbFrame` and `cbObjectInfo` themselves (same
    registers as `meshlet_common.hlsli`) and do not include `meshlet_common.hlsli`. Declaring them in the shader
    keeps the original resource ID order (CB0..CBn) and avoids pulling in the meshlet resources.
- `LightType_*` values (1 directional, 2 spot, 3 point) are confirmed by the DXIL: spot/point is tested as
  `(type & ~1) == 2` and spot as `type == 2`.

## Per shader

### material_id_ps
Writes `g_Material.materialID` to an R32_UINT target (SV_Target, uint).
- Bindings: `c_Material` b0. With `_ASTEROIDS=1`: `t_RandomsTexture` Texture3D t15 space1.
- `_ASTEROIDS=1`: the input is asteroidMS (SV_Position, ATTR1, ATTR2, nointerpolation ALPHA_LOD), and
  `LodTransitionDither` runs first: `random = t_RandomsTexture.Load(int3(pos.xy, frac(alpha)*64) & 63).x`.
  If `alphaLod.y != 0` (LOD fading in), the pixel is discarded when `random >= alphaLod.x`. Otherwise it is
  discarded when `random < alphaLod.x`.
- `_ASTEROIDS=0`: the input is SV_Position + `SceneVertex` (POS, UV, centroid NORMAL/TANGENT/BITANGENT) + PREV_WORLD_POS.
- `IS_SHIP` makes no difference: the IS_SHIP-0 and IS_SHIP-1 disassemblies are identical apart from value names.
- Provenance: pure lifting.

### gbuffer_ps
Outputs: RT0 = (diffuse, opacity); RT1 = (emissive, 1) when any emissive component is > 0, else (specular, 0);
RT2 = (normal, roughness); RT3 (float2) = motion vector in pixels.
- Bindings: `c_Material` b0, `c_GBuffer` b1, `s_MaterialSampler` s0, `t_Diffuse/Specular/Normals/Emissive` t0..t3.
  `_ASTEROIDS=1` also uses `cbFrame` b0 space1 and `t_RandomsTexture` t15 space1. `IS_SHIP=1` also uses `cbObjectInfo` b4 space1.
- `_ASTEROIDS=0`: `EvaluateSceneMaterial(SceneVertex)`, `GetMotionVector(svPos.xy, PREV_WORLD_POS)`.
- `_ASTEROIDS=1`: LOD dither, then discard if `cbFrame.enableDistanceLod && random > alphaLod.z` (view distance
  fade), then `EvaluateAsteroidsMaterial(ATTR1.xyz, (ATTR1.w, ATTR2.w), ATTR2.xyz)`.
  `prevWorldPos = pos + (preViewTranslationPrevious - preViewTranslation)`, so asteroids are treated as static.
  If `cbFrame.visualizeLods` is set, RT0.rgb is replaced by a 10-entry LOD palette indexed by
  `ceil`/`floor(alphaLod.w)` (the fade-in flag picks ceil or floor).
- `IS_SHIP=1`: the emissive written to RT1 is scaled per region of the 4096x4096 ship texture.
  Texels [0,1008)^2 use `cbObjectInfo.lodBias`, [211..245)x[2447..2457) use `cbObjectInfo.center.x` and
  [3898..3915)x[1207..1237) use `cbObjectInfo.center.y`.
- `clip(opacity - 0.5)` is applied in all permutations. For asteroids the opacity is the constant 1.
- Provenance: GetMotionVector is the 2021 donut function in its 2018 form (float2 result, uses
  `matWorldToClipPrev`, `viewportScale/BiasPrev`, `pixelOffset`). Everything else is lifted.

### forward_ps
Forward lighting: analytic lights with cascaded shadows (EvaluateShadowGather16, early-out when the coverage
reaches 1) and per-object shadows (EvaluateShadowGather16), light probes, a hemispherical ambient term, then
`o = diffuse * diffuseColor + specular + emissive`, alpha = opacity. When `any(g_Material.emissiveColor != 0)`,
the output becomes an additive glow for the engine exhaust: `(color + 10 * emissive) * noise1 * noise2 * NdotV^3 * opacity`.
The two noise layers are scrolling samples of `t_Diffuse.r` driven by `cbFrame.time`. Alpha is 0.
- Bindings: `c_Material` b0, `cbFrame` b0 space1, `c_Forward` b1; samplers s0..s3 (Material, Shadow, LightProbe, Brdf);
  textures t0..t3 (material), `t_ShadowMapArray` t4, `t_DiffuseLightProbe` t5, `t_SpecularLightProbe` t6, `t_EnvironmentBrdf` t7.
- `_ASTEROIDS` selects the input and material as in gbuffer_ps, but there is no dither or discard. `IS_SHIP` makes no difference.
- Light probe cube maps are sampled with a flipped Z (`float4(N.xy, -N.z, index)`, the same for R).
- Provenance: the light and probe loops follow the 2021 donut `forward_ps.hlsl`. ShadeSurface is the 2018 version:
  it returns scalar diffuse and specular, and the caller applies `light.color` and `surface.specularColor`.
  - irradiance = `radiance * 0.5 * halfAngularSize^2 * attenuation * spotlight`, the same formula for every light type.
    Point and spot lights always take `halfAngularSize = atan(min(radius / distance, 1))`.
  - specular = diffuse * GGX * 2*pi. GGX is exactly the `_GGX` that is commented out in the 2021 `lighting.hlsli`
    (sphere-light slerp correction, alpha = max(0.01, r^2), Schlick-G with k = (r+1)^2/8).
  - The shadow functions are copied from the 2021 `shadows.hlsli`, with one correction (see ShadowWithMaxDistance below).

### deferred_lighting_ps
Full-screen pass that does the same lighting as forward_ps from the G-buffer. Output alpha is 0.
- Bindings: `c_Deferred` b0; `t_GBufferDepth/0/1/2` t8..t11; `t_ShadowMapArray` t0, `t_DiffuseLightProbe` t1,
  `t_SpecularLightProbe` t2, `t_EnvironmentBrdf` t3, `t_IndirectDiffuse` t4; `s_ShadowSampler` s0,
  `s_ShadowSamplerComparison` s1 (SamplerComparisonState), `s_LightProbeSampler` s2, `s_BrdfSampler` s3.
- `GetSurfaceParams(int2 pixel, float2 uv)`: unprojects with `matClipToView` (divide by w), then `matViewToWorld`.
  GBuffer1.w selects specular or emissive.
- Cascades use `EvaluateShadowPoisson`: 16 taps, disk of 3 texels. The rotation is
  `sincos(noisePattern[(int)(pos.y+randomOffset.y) & 3][(int)(pos.x+randomOffset.x) & 3])`.
  Per-object shadows use `EvaluateShadowGather16`.
- When `indirectDiffuseScale > 0`, `indirectDiffuseScale * t_IndirectDiffuse[pixel].rgb` is added to the diffuse term.
- `gbufferArraySlice` and GBuffer0.a are not used.
- Provenance: the same as forward_ps. EvaluateShadowPoisson matches the 2021 donut function.

### shield_ps
Full-screen pass for the ship's shield bubble. The output is premultiplied.
- Bindings: `g_Shield` b0 (the cbuffer and its member are both named g_Shield), `t_Depth` Texture2D<float> t0.
- Reconstructs `worldPos = mul(clipPos, matClipToTranslatedWorld) / w` as a float4. Both the scene distance and
  the ray direction are taken from that float4 including w = 1, so they are `length`/`normalize` of (x, y, z, 1).
  This is what the DXIL computes (`x^2 + 1 + y^2 + z^2`).
- Intersects the ray with the sphere `raySphereIntersections(-shipPosition, rayDir, distanceToShip, shieldRadius)` and discards
  on a miss, when the scene is in front of the shield, or when the shield is behind the camera.
  The bubble is shaded at the entry and exit points with `shadeShield(normal, rayDir, front)`:
  - fresnel = f + (1 - f) * 0.01 with f = (1 - |N.V|)^5
  - pattern = iq value noise of (N*5 + time) through smoothstep(0.1,0.4) * smoothstep(0.8,0.65) * sin(noise * (65 + 15 sin 2t))
  - colors (0, 0.878, 0.69) for the front and (0.003, 0.188, 0.352) for the back
  - intensity = saturate(N.dir + 0.8)^2 * shieldIntensity
  The front and back colors are composited front-to-back. A hexagonal grid band (iq "hexagons - distance") in
  spherical coordinates is added; it sweeps with `saturate(shieldIntensity)`.
- Provenance: pure lifting. The noise and hexagon code are recognisable Shadertoy functions by Inigo Quilez.
- `o_color = 0` at the top reproduces the four zero `storeOutput`s at the entry of the original.

### accumulation_ps
Resolves 64 jittered frames (Texture2DArray `tex` t0, an 8x8 subpixel grid, one frame per slice) for the
high-quality screenshot path. A 5x5-pixel separable Lanczos-2 filter (sin(pi x) sin(pi x/2)/x^2, x clamped to
>= 1e-5, 0 beyond 2) runs in ST.2084/PQ space with 10000 nits = 1, and the result is converted back to linear nits.
Non-finite samples are replaced by 0. Alpha is 1.
- Provenance: pure lifting. The PQ constants are the SMPTE ST.2084 ones (m1 = 2610/16384, m2 = 78.84375, c1..c3).

## Deliberate reproduction of 2018 compiler behaviour

- `ShadowWithMaxDistance`: the donut source is
  `1 - (ref > v) * (maxDistance == 0 ? 1 : saturate((v + maxDistance - ref) * 10))`. DXC 1.2 typed that
  conditional as a scalar and kept only `.x` of the falloff. The shipped forward_ps, deferred_lighting_ps and the
  framework `passes_forward_ps` multiply all four gathered samples by that one scalar. The reconstruction writes this
  explicitly (marked `// deviation:`), so DXC 1.8 produces the same values.
- The 2-channel normal map path rebuilds Z from the unscaled texel (`sqrt(1 - saturate(dot(t.xy, t.xy)))` with
  `t` in 0..1), not from the [-1, 1] value. The 3-channel path's final transform uses the raw
  interpolated `m_normal`, not the normalized geometry normal. Both are kept as in the original.
- `GetLightProbeWeight` carries `[unroll]` because the original has the six plane tests unrolled. The 2021
  donut comments mention that this unroll was removed later because of a compiler crash.

## Verification

Script: `build/shader_check/surface/check_surface.py` (logs `check_hv2018.log`, `check_hv2021.log`, outputs
`build/shader_check/surface/<shader>/<perm>.dxil(.txt)`). It compiles every original permutation with
`dxc.exe -T ps_6_0 -E main -HV 2018|2021 -I asteroids/shaders/demo/include -D ...` (DXC 1.8.2502) and compares
`-dumpbin` against the original dump:

| check | result (all 15 permutations, -HV 2018 and -HV 2021) |
|---|---|
| input signature (name, index, mask, register, sysvalue, format) and interpolation modes | identical |
| output signature | identical |
| resource bindings (name, type, format, dim, ID, register/space, count) | identical, including the CB/T/S ID order |
| cbuffer layouts (every field type, name and offset, cbuffer sizes) | identical |
| dx.op histogram (texture ops, discards, derivatives, transcendental ops, ...) | identical after the known lowering differences below (`ops~:OK`), except shield_ps (see below) |

The ShaderMake configuration (`ps_6_5`, `-enable-16bit-types`, HLSL 2021 default) also compiles with `-WX` for all shaders.

Known DXC 1.2 → 1.8 differences in the instruction stream, all equivalent:
- `normalize()` used to be lowered as `x / sqrt(x.x)` (fmul/fadd, Sqrt, 3 fdiv). It is now Dot3/Dot4 + Rsqrt + fmul.
  This accounts for all Sqrt/Rsqrt/Dot3/fdiv count differences.
- `mul(float4(v, 1), M)` produced `FMad(1.0, m, acc)`. Now it produces `fadd`.
- DXC 1.8 folds `clip(1.0 - 0.5)` (asteroid gbuffer) and `UMax(x, 0)` from `clamp(uint, 0, 9)`, and it CSEs some
  cbuffer reloads.
- With `-HV 2018`, DXC 1.8 lowers several conditional operators to `select`, while DXC 1.2 branched. With
  `-HV 2021` (the ShaderMake default) the branch count of forward_ps matches exactly (95/95, 81/81).
- Floating-point reassociation differs slightly (for example `(offset * 3) * texelsInv` versus
  `offset * (texelsInv * 3)` in the Poisson loop).
- shield_ps: DXC 1.8 CSEs three subexpressions that DXC 1.2 computed twice: `sin(2 * time)`,
  `dot(frontNormal, shieldDirection)` and `saturate(shieldIntensity)`. That is one fewer Sin, Dot3 and Saturate.
  Everything else matches, including the branch count (15/15).

Spot checks of the IR beyond the histograms: the asteroid noise (half precision with `fptrunc` at the same
places, the hash constants, the 6.25e6 folding), fbmd matrices, the octave count, the material branches, the motion
vector expression, the ShadeSurface control flow, the PQ/Lanczos math and all shield constants match the
original.

## Open questions / uncertainties

1. **Ship emissive intensities.** IS_SHIP gbuffer reads `cbObjectInfo.lodBias`, `center.x` and `center.y` as
   multipliers for three ship-texture regions. The C++ side must fill ObjectConstants accordingly for the
   ship. Which engine or light each region is, and how it is animated, is still to be confirmed in the
   ship rendering code (`sub_14002BD60` creates these shaders).
2. **Toksvig formula.** The arithmetic is exact (`sp = 4/r^2 - 4`; `ft = max(len / lerp(sp, 1, len), 0.01)`;
   `r' = sqrt(2 / (0.5 * sp * ft + 2))`). The variable names and the 4/0.5 factoring are a guess.
3. **Irradiance factoring in ShadeSurface.** The product `radiance * 0.5 * theta^2 * atten * spot` and the
   specular factor `2*pi` (spec = diffuse * D*G/4 * 2*pi) are exact. How the original grouped the constants is unknown.
4. **Shield hex grid parameterization.** The compiled constants are reproduced to the last bit, except the
   coefficient of `atan(n.z/n.x)`: the original has 23.873274 (q.x) and 11.936637 (q.y), the reconstruction
   23.873260 / 11.936630, a relative difference of 6e-7. The source expression that produced the original
   rounding was not found. The shield code uses `PI = 3.14159`, as the folded constants show
   (0.31831015 = 1/3.14159).
5. **gbuffer RT1 selection.** The original uses one store per channel (a temporary, not two output writes),
   and the ship region scaling sits inside the emissive branch. The reconstruction uses a temporary
   `specularOrEmissive`. A conditional operator in the original would compile to the same code.
6. `SurfaceParams.clipPos` is never used by these shaders, so it cannot be determined whether the 2018 code
   wrote it.
7. With `-enable-16bit-types` (ShaderMake for SM 6.5) the asteroid noise core uses `float16_t`, which is what
   `min16float` maps to in that mode. The 2018 build used min-precision `min16float`, so hardware was free to
   run it at 32-bit.

## Asteroid surface noise: rotation matrices of fbmd (fix)

The asteroid material's fBm (`asteroid_material.hlsli`, `fbmd`) rotates the sample coordinate every octave with
`m3` and the derivative accumulator with `m3i = transpose(m3)`. The first reconstruction wrote this as
`x = f * mul(m3, x); m = f * mul(m3i, m)` on `static const float3x3` globals. `gbuffer_ps.hlsl` has
`#pragma pack_matrix(row_major)`, and under it DXC 1.8 compiled these products **transposed**: the coordinate was
rotated by `m3i` and the derivative matrix by `m3`, the reverse of the shipped DXIL (checked by reading both
listings: the original's first coordinate output is `0.8*y + 0.6*z`). The value `a` and the albedo hardly notice,
but the gradient (the fine-octave detail of the noise normal) is dominated by the last octaves, whose hash input
`floor(x)` / `frac(p*0.3183099+c)` is chaotic in the exact rounding of the coordinate chain. Every surface then
had a different fine normal pattern and, through the 85 % diffuse texture weight and the lighting, a different
look.

The fix keeps the idiomatic `static const float3x3` + `mul()` form but swaps the operand order (`mul(x, m3)`,
`mul(m, m3i)`, `d += b * mul(n.yzw, m)`), which under the pragma compiles to exactly the rotation of the shipped
DXIL (verified: byte-identical DXIL to an explicit `mad()`-chain transcription of the original instruction order,
and all three G-buffer targets bit-identical across a rebuild). Result at a converged frame
(`-view 2 -aa 0`, G-buffer targets dumped with `-dumpGBuffer`, compared with a run using the shipped DXIL through
`-shaderOverride`): albedo target 46 dB -> 80 dB, normal target 24 dB -> **bit-identical (99 dB)**, final image
vs the original capture 20.5 dB -> 44.6 dB (the original differs from itself by 45.3 dB between two captures).
`particles.hlsl` has the same code but no pragma; its matrix constants occur in the same order as in the original
DXIL, so it was left alone.

Method used to find this (reusable): `tools/nvsp2shadermake.py` converts the shipped blobs and
`Asteroids.exe -shaderOverride DIR` loads them in place of the rebuilt ones, so every shader can be A/B tested on the
real pipeline. At converged frames only the `gbuffer_ps` swap changed the image; the mesh/task shaders, Hi-Z, fx
and framework shaders gave 54-90 dB against the rebuilt ones.

## Cascade index copy (performance)

`LightConstants::shadowCascades` is an `int4` in the constant buffer, indexed by the cascade loop counter. DXC 1.8
lowers a dynamically indexed vector to a local array and, for `light.shadowCascades[cascade]`, re-copies the four
ints from the constant buffer into that array **on every loop iteration** (one extra `cbufferLoadLegacy` plus four
stores). The shipped 2018 DXIL (DXC 1.2) does the copy once before the loop. In `fog.hlsl` the fog march evaluates
this loop for every step, which cost about 0.4 ms per frame (view 0: Fog pass 1.75 ms -> 1.27 ms; with the shipped
`fog_ps_trace` it is 1.28 ms). All cascade loops (`fog`, `deferred_lighting_ps`, `forward_ps`, `lensflare`,
`particles`, `particles_ms_common`) now copy the indices into a local `int4 cascadeIndices` first. Pure
scheduling change: output identical (69 dB between two runs at different frames, i.e. no difference beyond TAA/noise).
Method: `-shaderOverride` with only `fog_ps_trace` or only `fog_ps_filter` from the original blob showed that the trace shader owns
the gap; a DXIL listing diff then found the loop-carried copy. Hoisting the extinction integral out of the march loop (also
tried) made no difference and was not kept.

## colorRamp palette (performance)

`colorRamp` in `asteroid_material.hlsli` used `static const float3 palette[4]` indexed dynamically. DXC 1.8 compiled it into a
writable global that every pixel refills with 12 stores before the lookup; the shipped DXIL has it as constant data. It is now
three `static const float` arrays (one per channel). Same values, `gbuffer_ps` GPU time in the asteroid field 3.26 -> 3.12 ms,
equal to the shipped shader (3.13 ms); image identical (90 dB between runs before/after).
