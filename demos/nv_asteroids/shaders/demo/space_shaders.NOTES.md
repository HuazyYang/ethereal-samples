# Space / FX shaders: reconstruction notes

These sources were reconstructed from the DXIL that the 2018 Asteroids demo shipped (DXC 1.2, SM 6.0). The inputs are `recon/assets/shaders_split/demo/<shader>/default.dxil(.txt)`.

File names and entry points follow the strings in `Asteroids.exe`, as seen in `recon/build/decomp`:

| Exe string | Entry point(s) |
|---|---|
| `real_stars_vs.hlsl` | `VS` |
| `real_stars_ps.hlsl` | `PS` |
| `fog.hlsl` | `ps_trace`, `ps_filter` |
| `lensflare.hlsl` | `vs_main`, `ps_main` |
| `particles.hlsl` | `vs_main`, `ps_main`, `ms_main`, `update_cs_main` |
| `PlanetFx_on_ps.hlsl`, `PlanetWithRingsFx_on_ps.hlsl` | `main` |
| `RectPass_vs.hlsl`, `sun_disk_ps.hlsl`, `texturedstarfield_ps.hlsl`, `hq_blit_ps.hlsl`, `show_cubemap_ps.hlsl` | `main` |

With these names, ShaderMake writes outputs whose names match the shipped blob names: `real_stars_vs_VS`, `fog_ps_trace`, `lensflare_ps_main`, `PlanetFx_on_ps`, and so on.

## Files

| File | Entry points | Original blobs |
|---|---|---|
| `real_stars_vs.hlsl` | VS (vs_6_0) | real_stars_vs_VS |
| `real_stars_ps.hlsl` | PS (ps_6_0) | real_stars_ps_PS |
| `texturedstarfield_ps.hlsl` | main (ps_6_0) | texturedstarfield_ps |
| `RectPass_vs.hlsl` | main (vs_6_0) | RectPass_vs |
| `sun_disk_ps.hlsl` | main (ps_6_0) | sun_disk_ps |
| `PlanetFx_on_ps.hlsl` | main (ps_6_0) | PlanetFx_on_ps |
| `PlanetWithRingsFx_on_ps.hlsl` | main (ps_6_0), which is `PlanetFx_on_ps.hlsl` built with `PLANET_WITH_RINGS=1` | PlanetWithRingsFx_on_ps |
| `lensflare.hlsl` | vs_main (vs_6_0), ps_main (ps_6_0) | lensflare_vs_main, lensflare_ps_main |
| `fog.hlsl` | ps_trace, ps_filter (ps_6_0) | fog_ps_trace, fog_ps_filter |
| `particles.hlsl` | vs_main (vs_6_0), ps_main (ps_6_0), update_cs_main (cs_6_0) | particles_vs_main, particles_ps_main, particles_update_cs_main |
| `hq_blit_ps.hlsl` | main (ps_6_0) | hq_blit_ps |
| `show_cubemap_ps.hlsl` | main (ps_6_0) | show_cubemap_ps |
| `include/space_cb.h` | Struct layouts: StarInstance, CelestialConstants (C++ only), RectConstants, SunConstants, PlanetConstants, StarFieldConstants, LensFlareConstants, FogConstants, ShowCubemapConstants (C++ only) | |
| `include/particles_cb.h` | ParticleInfo and ParticleConstants, shared with `particles_ms.hlsl` | |
| `include/light_cb.h` | The 2018 LightConstants, ShadowConstants and LightProbeConstants | |
| `include/space_shadows.hlsli` | `EvaluateShadowPCF`, which matches donut's | |
| `include/real_stars.hlsli` | The `VSOut` struct shared by the real-star VS and PS | |
| `space_shaders.cfg` | ShaderMake fragment, with paths relative to `asteroids/shaders/` | |

The headers follow the donut convention: in C++ they need `donut/core/math/math.h` and `using namespace donut::math;` first. They use explicit `row_major` on the HLSL side, behind the `*_ROW_MAJOR` macros, so `-Zpr` / `--matrixRowMajor` is not required. Where HLSL puts an array element on its own 16-byte register (`LensFlareConstants::pad[3]`), the C++ side gets explicit padding. The `static_assert`s in each header check the offsets, and the headers syntax-check as C++17 with clang.

Some cbuffers declare their members directly instead of wrapping them in a struct: `CelestialConstants` in real_stars_vs, and `CB` in show_cubemap_ps. The shaders keep that form so the reflection matches. The structs with the same layout in `space_cb.h` exist under `#ifdef __cplusplus` only.

## Per-shader notes

### real_stars_vs.hlsl / real_stars_ps.hlsl: catalogue star field

* Bindings:
  * `cbuffer CelestialConstants : b0` (84 B): `row_major float4x4 ViewProjectionToClip; float ST, SinLAT, CosLAT, Aspect, Brightness;`
  * `StructuredBuffer<StarInstance> t_StarInstances : t0` (stride 16)
  * The PS has no resources.
* Draw: non-indexed, 3 vertices per star, 64 stars per instance. `star = floor((vid+0.5)/3) + instance*64`, `corner = floor(frac((vid+0.5)/3)*3)`.
* VS:
  * `dir = (cos ra cos dec, sin dec, sin ra cos dec)`, with Ra and Dec in degrees.
  * `clip = mul(float4(dir,1), ViewProjectionToClip)`, and `pos.xy = clip.xy/clip.w + TrianglePoints[corner]*0.0033*float2(1,Aspect)*max(1,size)` with `size = 2 exp(-0.3 Mag)`, `z = w = 1`.
  * `uv.xy = TrianglePoints[corner]` (an equilateral triangle around the unit circle).
  * `uv.z = 1000 * i^3 * exp(-0.2 Mag) * Brightness`, where `i = size/max(1,size)`.
* PS: `r = length(uv.xy)`; returns 0 outside the unit circle, otherwise `float4((uv.z*0.001*exp(-10 r)).xxx, 1)`.
* `ST`, `SinLAT`, `CosLAT` and `StarInstance::CatNumber` are not referenced by the shaders. The sky rotation must be baked into `ViewProjectionToClip` on the CPU.

#### Stars.buf record encoding

`media/SkyAndStars/Stars.buf` is a raw array of `StarInstance`. The layout comes from the structured-buffer reflection of `t_StarInstances`; it is not `{x,y,z,packed}`:

```
struct StarInstance { float Ra; float Dec; float Mag; int CatNumber; };   // 16 bytes, little endian
```

What the data shows (4,143,104 bytes = 258,944 records, exactly 4,046 instances of 64):

| Field | Meaning | Range in the file |
|---|---|---|
| `Ra` | Right ascension, degrees (J2000) | 0.00013 to 359.99 |
| `Dec` | Declination, degrees | -89.77 to +89.77 |
| `Mag` | Apparent visual magnitude, with one decimal | -1.6 to 11.9 |
| `CatNumber` | **SAO catalogue number** | 1 to 258,997, all unique |

* The records are sorted by ascending `Mag`.
* Record 0 is Sirius (101.287°, -16.716°, -1.6, SAO 151881). It is followed by Canopus (SAO 234480), Vega (SAO 67174), α Cen (SAO 252838), Capella (SAO 40186) and Arcturus (SAO 100944).
* `CatNumber` is a signed `int` in the reflection, so nothing is bit-packed into it.

### texturedstarfield_ps.hlsl: nebula plus tiled star texture background

* Bindings:
  * `cbuffer c_Sky { StarFieldConstants g_Sky; } : b0` (92 B)
  * `TextureCube t_EnvironmentMap : t0`, `Texture2D t_StarMap : t1`, `SamplerState s_Sampler : s0`
* The view direction is `normalize(mul(float4(clip, 0.5, 1), matClipToTranslatedWorld))` after the perspective divide. The nebula sample uses `float3(-d.x, d.y, d.z)`.
* Stars are drawn only if `starLinearBrightness > 0 || starSquareBrightness > 0`:
  * Cube-face projection: `mask = step(maxAbs, abs(d))`, `uv = (dot(mask,(d.z,-d.y,d.x)), dot(mask,(d.y,d.x,-d.y)))/maxAbs + 1`.
  * The star texture is sampled at `uv*2`, `uv*4` and `uv*7` and summed into `s`. The result is `(s*square + linear)*s`, faded by `1 - saturate(8*luma601(nebula)^2)`.
* Nebula term: `nebulaLinearBrightness * env * saturate(1/(1-luma709(env)) - 1)`. Alpha is 0.
* `directionToSun` is unused.

### RectPass_vs.hlsl: corner-quad vertex shader for the sky, planet and sun passes

* Binding: `cbuffer cbRectConstants { RectConstants g_param; } : b0` (128 B).
* A 4-vertex strip:
  * `SV_Position = vertices[id & 3]`
  * `DIRECTION = directions[id & 3].xyz`
  * `ST = (((id<<1)&2) - 1, 1 - (id&2))`
* The outputs are `out` parameters; the type annotations contain no struct.

### sun_disk_ps.hlsl

* Binding: `cbuffer cbSunDisk { SunConstants g_Sun; } : b0`.
* `r2 = dot(ST,ST)`, `I = pow(saturate(2(1-r2)), 4) * exp(-16 r2)`. The pixel is discarded if `I == 0`; otherwise the output is `float4(color*I, I)`.

### PlanetFx_on_ps.hlsl / PlanetWithRingsFx_on_ps.hlsl: planet seen from space

* Bindings:
  * `cbuffer cbPlanetFx { PlanetConstants g_param; } : b0` (112 B)
  * `Texture2D Surface : t0`, `Normals : t1`, `Rings : t2` (rings variant only), `SamplerState SurfaceSampler : s0`
* Function names and signatures come from the mangled labels: `integratePlanetFromSpace(float3) -> float4`, `raySphereIntersections(float3,float3,float,float,out float2) -> bool`, `ringColor(float3,float,float) -> float4`.
* The pixel is skipped when `dot(ST,ST) > 1`. Positions are camera relative: the ray starts at `-planetPosition` relative to the planet center.
* Radii: `outer = atmosphericRadius`, `inner = radiusRatio*outer`, `fScale = 1/(outer-inner)`.
* **Surface:**
  * Lat-long UV: `u = (atan(-n.z/n.x)/π + (n.x<0)) * 0.5 + rotation`, `v = acos(n.y)/π`, both with `SampleLevel(textureLOD)`.
  * Tangent frame: `T = normalize(n.z,0,-n.x)`, `B = normalize(cross(n,T))`. The normal map is applied as `T*ns.x + B*ns.y + n*ns.z` with **no** `*2-1` decode. This is as compiled.
  * Lighting: `NdotL = saturate(saturate(dot(n,L)+stretchAmount)^2 * dot(N,L))`, `color = (sunColor*NdotL*litBrightness + ambientBrightness) * albedo`.
* **Atmosphere** (only when `radiusRatio < 1`): Sean O'Neil's GPU Gems 2 GroundFromSpace and SkyFromSpace.
  * Each runs 2 samples, with scale depth 0.25 and the usual polynomial `scale()`.
  * Constants: Kr 0.0025, Km 0.001, ESun 20, Mie g = -0.99.
  * `v3InvWavelength = (5.602, 9.4733, 19.6438)`. These values come from the compiled constants (`0.2801 = 5.602*0.05` exactly), not from `1/pow(λ,4)`.
  * Ground: `color = lerp(surface, (c0 + 0.25*attenuate)*atmosphereBrightness*sunColor, 1-exp(-(tPlanet-tAtmo)*atmosphericAlpha))`.
  * Sky: `color = frontColor*(invW*KrESun + KmESun*miePhase)*atmosphereBrightness*sunColor` with `alpha = 1-exp(-(tFar-tNear)*atmosphericAlpha)`. No Rayleigh phase term.
  * Every float constant in the rebuilt shader (wavelength terms, Mie terms, scale polynomial) is bit-identical to the original.
* **Rings:**
  * Ray/plane hit with `ringsPlane` from the camera at the origin. The hit point is taken relative to the planet center, and the ring is drawn when `t>0` and `inner² <= r² <= outer²`.
  * Over the planet disk, the ring is drawn only where `dot(ringPos, rayDir) < 0`, so the part behind the planet center is hidden.
  * `ringColor`: the planet's shadow comes from a sun-ray/planet-sphere test (shadowed when both hits are > 0). `u = saturate((r²-inner²)/(outer²-inner²))`, and the texture is sampled at `(u,u)`. Lighting is `|dot(ringN, L)|`. Output: `litBrightness*ring.rgb*(sunColor*light*litBrightness + ambient)` with alpha = `ring.a`.
  * Compositing: `rgb = lerp(rgb, ring.rgb, ring.a)`. Alpha becomes 1 over the planet and `ring.a` elsewhere; the sky alpha is replaced, as compiled.
* JSON hookup, from the decomp of `sub_1400081F0`: the keys read are Azimuth, Elevation, AngularSize, RadiusRatio, AtmosphericAlpha, LitBrightness, AmbientBrightness, AtmosphereBrightness, Rotation, StretchAmount, InnerRingRadius, AtmosphereRingsRatio, RingsElevation, RingsAzimuth, SurfaceMap, NormalMap and RingsPattern. `AtmosphereRadius` and `VerticalComponent` in planets.json are not read by the exe. SurfaceMap goes to `Surface`, NormalMap to `Normals` and RingsPattern to `Rings`.

### lensflare.hlsl: procedural sun flare

* Bindings:
  * `ConstantBuffer<LensFlareConstants> g_LensFlare : b0` (804 B, with `shadows[5]`)
  * VS only: `Texture2DArray t_ShadowMapArray : t0`, `SamplerComparisonState s_ShadowSampler : s0`
* **VS:**
  * `OCCLUSION = GetShadow(float3(0,0,0))`, sampled at the camera position, which is the origin of the camera-relative world. The zero position is visible in the DXIL as `fmad(0, …)` chains. `GetShadow` combines the cascades with the 2018 deferred-lighting cascade loop, adds `outOfBoundsShadow`, and multiplies the `perObjectShadows` terms.
  * If the result is ≤ 0 the quad is collapsed (`pos=(0,0,-1,1)`, `uv=0`). Otherwise the VS emits a full-screen strip with `uv = (id&1, (id>>1)&1)`.
  * The cascade loop runs to 5 (the number of shadow slots) although `shadowCascades` is an `int4`. This is as compiled.
* **PS** (`lensflare(float2,float2,float,float) -> float3` from the mangled name):
  * `uv = (UV-0.5)` and `pos = g_LensFlare.pos`, each with x scaled by `screenScale.y/screenScale.x`.
  * The sun position is fish-eye distorted by a Rodrigues rotation of `(pos,0)` about `cross((uv,0),(0,0,1))/|uv|` by `2.221*|uv|`.
  * The flare is made of these terms, each with per-channel offsets `(0.05, 0, -0.05)`:
    * soft-disc ghosts `max(0.01 - |p|^2.4 * k, 0)` at `uv + (t-δ)*pos'`
    * a 5-ghost series with `t=i/(i+1)`, `k=i+0.1`, weights `(6-i/2, 6, 3+i/2)`
    * a halo `1/(1+32|uvd ± pos|²)` with `uvd = uv*|uv|`
  * The result is multiplied by `irradiance*0.5`, raised to `pow(…,1.5)` and multiplied by occlusion. Alpha is the occlusion.
  * The per-channel offsets are confirmed by float rounding: `0.4+0.05` gives the original's 0.45000002 while `0.5-0.05` gives 0.44999998. Both are bit-identical to the original.

### fog.hlsl: half-resolution volumetric height fog

* Bindings:
  * `ConstantBuffer<FogConstants> g_Fog : b0` (704 B, with `shadows[3]`)
  * `Texture2D<float> t_DepthBuffer : t0`
  * `t1`: `Texture2DArray t_ShadowMapArray` in ps_trace, `Texture2D t_UnfilteredFog` in ps_filter. Each entry point uses only one of them.
  * `SamplerComparisonState s_ShadowSampler : s0`
* Shared pixel pattern: low-res pixel p reads depth at `2p + (p.y&1, (p.x&1)^1)`.
* **ps_trace:**
  * Reconstructs the world position from depth, then `distance = 0.95*|ray|`.
  * Phase: HG with g = 0.75, normalized to 1 in the forward direction, times 0.5973151 (the exact compiled float).
  * The ray is clipped to the slab `meanHeight ± 10*thickness` (`IntersectRayWithXZPlanes`). The trace returns 0 if the slab starts beyond the scene depth.
  * Step: `max(1, lerp(0.15, 0.02, density) * offset)`. The `lerp` is confirmed by the folded constant `0.02f-0.15f = -0.13000001`.
  * The start is dithered with `noisePattern[y&3][x&3]` plus `frac(randomOffset.z)*3`.
  * Per step:
    * `density = densityScale*exp(-(y-mean)²/(2 thickness²))`
    * cascaded shadow via `GetShadow(pos, offset, inout useShadowMaps)`, which stops sampling once beyond `maxShadowDistance` and outside every cascade
    * sun extinction through the layer: `exp(-densityScale * 0.75 * sqrt(π/2) * σ_t * (1 - erf(-t0/(√2 σ_t))))`, using the Abramowitz-Stegun erf; the function name `extinctionIntegralInf` takes 5 floats
    * ambient: `smoothstep(mean-th, mean+th, y)*0.001`
    * `inscatter += step*(1-transparency)*density*(phase*shadow + ambient)` and `transparency *= exp(-step*density)`
  * The march stops at the scene depth or when `transparency < visibilityThreshold`.
  * Output: `float4(light.color*inscatter*color*irradiance*intensity, 1-transparency)`, where `irradiance = radiance*angularSize²/8`.
* **ps_filter:** 4×4 low-res neighbourhood with weight `saturate(1 - 10*|linZ_s - linZ|/linZ)`, where `linZ = projectionB/(depth-projectionA)`. Sky pixels (depth == 1) use only sky samples. If every weight is 0 the result is the plain average (÷16).

### particles.hlsl: space dust (VS/PS path and simulation)

* Bindings:
  * `ConstantBuffer<ParticleConstants> g_Particles : b0` (656 B)
  * `Texture2DArray t_ShadowMapArray : t0`, `StructuredBuffer<ParticleInfo> t_Particles : t1` and `SamplerComparisonState s_ShadowSampler : s0` (VS)
  * `RWStructuredBuffer<ParticleInfo> u_Particles : u0` (CS)
* **vs_main:**
  * `index = particlesPerBatch*instance + floor((vid+0.5)/3)` and `worldPos = position + positionOffset`.
  * The particle is culled (`pos=(0,0,-1,1)`, color 0, uv 0) if `clip.w <= 0` or `|worldPos| > maxDistance`.
  * `SetupParticle(float3,float4,float,float,out float2,out float4)` (from the mangled name):
    * `size = max(0.3, 0.003*w)`
    * `alpha = (0.3/size)^1.5 * saturate(10(1-d/maxDistance)) * saturate(0.02d-1)`
    * phase: HG with g = 0.9, normalized forward, `t³*82.51197 + 10/(4π)`
    * color: `irradiance*brightness*light.color*(shadow*phase + 0.2)`
  * Shadows use the 2018 cascade loop over 4 slots. The quad corner is `clip.xy + screenScale*size*TrianglePoints[c]`.
* **ps_main:** `r = |uv|`; returns 0 outside the unit circle, otherwise `float4(color.rgb, color.a*exp(-5r))`. COLOR is `nointerpolation`.
* **update_cs_main**, `[numthreads(256,1,1)]`:
  * `pos += vel*dt`; x and z are wrapped into `[0, volumeSize]` (`wrap(inout float, float)`).
  * The force is `10*safeNormalize(fbmd(pos*0.05 + time, 3).yzw)`, using Inigo Quilez's 3D value noise with analytic derivatives (cubic interpolation, hash `50*frac(p*0.3183099+(0.71,0.113,0.419))`) and fbm with rotation `m3`/`m3i`, f = 1.98, s = 0.49.
  * `vel += force*dt` then `vel -= vel*dt*0.1`.
* The original source also included NVAPI `nvHLSLExtns.h`. `NvShaderExtnStruct`, a 580-byte annotation, appears in all three entry points, but nothing here uses it.

### hq_blit_ps.hlsl: Lanczos-3 resampling blit

* Bindings: `Texture2D tex : t0`, `SamplerState samp : s0`.
* 5 taps per axis, with the two center texels merged into one bilinear tap, sampled at the 13 points of a 5×5 diamond (`|x-2|+|y-2| <= 2`). The weights come from one sin/cos pair through angle-addition identities, and the result is normalized by the weight sum. Alpha is 1.
* The outermost weight (`Weight[0]`) is half its true relative value: it lacks the factor 2 the other five carry. This is kept as compiled.

### show_cubemap_ps.hlsl: lat-long debug view of a cube-map array

* Bindings: `cbuffer CB { uint g_ArrayIndex; uint g_MipLevel; } : b0`, `TextureCubeArray t_SourceTexture : t0`, `SamplerState s_Sampler : s0`.
* `dir = (cos az cos el, sin el, -sin az cos el)` with `az = 2π u` and `el = π(0.5-v)`. Sampled with `SampleLevel(dir, arrayIndex, mip)`.

## Verification

* Compiler: `C:\Program Files\dxc\bin\x64\dxc.exe` 1.8.2502 (`-T <ps|vs|cs>_6_0 -E <entry>`, no other flags). The original was built with DXC 1.2.
* Script: `recon/build/shader_check/space/check_space_shaders.py`. Outputs and dumps are in the same folder, and the last run's output is in `compare_report.txt`. For each shader the script compares:
  * the input/output signatures, including PSV interpolation modes
  * the `Buffer Definitions` and `Resource Bindings` sections
  * a histogram of `dx.op` intrinsics and LLVM instructions
  * the set of float constants in the function body
* ShaderMake check: `ShaderMake --config asteroids/shaders/demo/space_shaders.cfg --sourceDir asteroids/shaders --ignoreConfigDir --platform DXIL --binary --shaderModel 6_0 ...` compiles 16/16. The output names equal the original blob names.

**Result: all 16 entry points match exactly on signatures, interpolation modes, resource bindings (name, type, register, space, count) and cbuffer layouts (every field name, type and offset, and every total size).**

Normalisation applied to the comparison, all of it dumper differences between DXC 1.2 and 1.8:
* DXC 1.2 left the "Used" column of input signatures empty.
* 1.2 calls the legacy-layout wrapper `dx.alignment.legacy.*` where 1.8 calls it `hostlayout.*`.

Remaining instruction-level differences come from the compiler version; they do not change the logic:

| Shader | Difference | Cause |
|---|---|---|
| real_stars_vs, particles_vs, fog_ps_trace, texturedstarfield | Fewer `FMad` | DXC 1.8 folds `fmad(1,x,y)` (the w=1 term of `mul`) into `fadd` |
| lensflare_vs | `FMad` 24 → 0, fewer cbuffer loads | DXC 1.8 folds `fmad(0,x,y)` (world position 0); 1.2 kept them |
| lensflare_ps (-6 Exp/Log), texturedstarfield (-1 Exp/Log) | Fewer Exp/Log | DXC 1.8 expands `pow(x, 2.0)` to `x*x`; 1.2 emitted `exp2(2*log2 x)` |
| PlanetFx, PlanetWithRingsFx, texturedstarfield | `Rsqrt` instead of `Sqrt` + `fdiv` | `normalize()` lowering changed (1.2: `x/sqrt(dot)`) |
| hq_blit | Fewer `fmul`, no allocas | 1.8 shares `wx*wy` and fully scalarizes the `Weight[]` / `Sample[]` arrays |
| PlanetWithRingsFx | 1 ring `Sample` instead of 4 | DXC 1.2 tail-duplicated the single ring block into the 4 planet / atmosphere / sky / empty paths; the logic is the same, including alpha = 1 over the planet and `ring.a` elsewhere |
| fog_ps_trace | 4 instead of 8 `StoreOutput` | The early `return 0` merges with the final store in 1.8 |

Texture and buffer operations agree everywhere: kind, count (except the duplication above), coordinates, LOD and compare values. Every float constant is bit-identical to the original, apart from constants that 1.8 folded differently (`-1.0`, `0.0`, `-log2(e)` from `exp(-x)`).

## Open questions / uncertainties

* **HG scale constants.** The particle phase is `t³ * 82.51197 + 10/(4π)` and the fog phase is `t³ * 0.5973151`, with `t = (1-g)/sqrt(1+g²-2g cos)`. `10/(4π)` reproduces the original bits; the other two constants are written as their exact compiled float values. No simple product of the usual terms (π, g, 1±g, 1-g², 4π, …) reproduces them, so their symbolic origin is unresolved. The `particles_ms.hlsl` reconstruction arrived at the same values independently.
* **Fog extinction.** The `0.75` factor in the fog sun extinction may be an extinction coefficient; only `0.75*sqrt(π/2)` (folded) is visible. The parameter split of `extinctionIntegralInf` (5 floats) is a best guess.
* **Odd behaviour kept as compiled.** fog `inscatter` is weighted by `(1 - transparency)` rather than `transparency`. hq_blit's `Weight[0]` is half-weighted. lensflare's cascade loop runs to 5. The planet normal map has no `*2-1` decode. The ring alpha replaces the sky alpha.
* **Lens flare parameters.** The PS helper decomposition (`Ghost3`, `Halo3`, `GhostSeries`) is a reconstruction. Only `lensflare(float2,float2,float,float)` is attested, and its last two parameters are assumed to be (intensity, occlusion).
* **Unattested names.** The parameter names of `raySphereIntersections` (two floats: start distance², radius) and `ringColor` (float3, float, float) are inferred. Helper names `GetDensity`, `GetStepSize`, `GetHiResPixel`, `GetLinearDepth`, `Ghost*`, `Halo*` and `RotateAroundAxis` are invented. Attested names (from mangled labels) are SetupParticle, EvaluateShadowPCF, GetShadow (lensflare and fog only; the particles helper of the same name is borrowed), wrap, fbmd, safeNormalize, IntersectRayWithXZPlanes, extinctionIntegralInf, integratePlanetFromSpace, raySphereIntersections, ringColor and lensflare.
* **particles ms_main.** The exe loads `ms_main` from `particles.hlsl`, the same file as vs/ps/cs. The mesh shader currently lives in `particles_ms.hlsl`, which another agent owns. To reproduce the shipped layout, merge it into `particles.hlsl` and drop its duplicate declarations, or `#include` it from there.
* **Rings blob duplication.** The duplicated ring code in the original rings blob is attributed to DXC 1.2 jump threading. It is possible, though unlikely, that the source repeated the ring block in each branch.
