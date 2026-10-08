# Meshlet geometry shaders: reconstruction notes

Source: `assets/shaders_split/demo/<shader>/<permutation>.dxil(.txt)`. These are the 2018 Asteroids binaries:
DXC 1.2, SM 6.0, full reflection, no debug info. The task and mesh shaders were NVAPI "mesh shaders",
compiled as vertex shaders that drive the hardware through the fake UAV `g_NvidiaExt`. They exist twice here:
- `demo/nvapi/*.hlsl`: the reconstructed NVAPI originals (`vs_6_0`), used by the NVAPI mesh-shader mode (see
  "NVAPI originals").
- `demo/*.hlsl`: a port to standard D3D12 amplification and mesh shaders (`as_6_5` / `ms_6_5`), used by the D3D12
  mode.

The algorithm, data layouts and resource bindings are the same in both. `create_hi_z_cs` is an ordinary compute shader and is reconstructed as-is
(`cs_6_0`).

| file | entry | profile | original |
|---|---|---|---|
| `asteroidTS.hlsl` | `ts_main` | as_6_5 | `asteroidTS_ts_main` (4 perms) |
| `asteroidMS.hlsl` | `ms_main` | ms_6_5 | `asteroidMS_ms_main` (2) |
| `basicTS.hlsl` | `ts_main` | as_6_5 | `basicTS_ts_main` (2) |
| `basicMS.hlsl` | `ms_main` | ms_6_5 | `basicMS_ms_main` (2) |
| `debugTS.hlsl` | `ts_main` | as_6_5 | `debugTS_ts_main` (1) |
| `debugMS.hlsl` | `ms_main` | ms_6_5 | `debugMS_ms_main` (1) |
| `particles_ms.hlsl` | `ms_main` | ms_6_5 | `particles_ms_main` (1) |
| `create_hi_z_cs.hlsl` | `main` | cs_6_0 | `create_hi_z_cs` (1) |
| `include/meshlet_cb.h` | | HLSL + C++ | FrameCB, MeshletInfoCB, InstanceCB, BoundingBox, ObjectConstants, SectorInfo, LODInfo, AsteroidInstance |
| `include/meshlet_common.hlsli` | | HLSL | resource declarations, meshlet decode, culling/LOD helpers, `AsteroidTaskPayload` |
| `include/particles_cb.h`, `include/light_cb.h` | | HLSL + C++ | not written by this group: owned by the particles reconstruction and reused unchanged by `particles_ms.hlsl` (ParticleConstants, ParticleInfo, 2018 LightConstants/ShadowConstants). The layouts were checked against the `particles_ms_main` reflection. |
| `include/particles_ms_common.hlsli` | | HLSL | particle resources, `PS_Input`, shadow / setup helpers shared by `particles_ms.hlsl` and `nvapi/particles.hlsl` |
| `meshlet_shaders.cfg` | | ShaderMake | one line per source, original permutation sets |
| `nvapi/asteroidTS.hlsl` … `nvapi/debugMS.hlsl`, `nvapi/particles.hlsl` | `ts_main` / `ms_main` | vs_6_0 | the same originals, NVAPI form |
| `nvapi/nv_meshlet_extns.hlsli`, `nvapi/nv_outputs.hlsli` | | HLSL | NVAPI mesh-shader opcode helpers; dummy VS output structs |
| `nvapi/nvapi_shaders.cfg` | | ShaderMake | the original permutation sets, `-T vs -m 6_0` |

The entry point names are the originals; they are recorded in the DXIL entry-point metadata.

## Shared layouts

All struct names, field names, order and offsets come from the reflection. On the HLSL side the matrices are
declared `row_major`, as the reflection records them (macro `MESHLET_ROW_MAJOR`), so the layout does not depend
on `-Zpr`. The C++ side follows the donut convention: include `donut/core/math/math.h`, add
`using namespace donut::math;`, then include the header. `meshlet_cb.h` contains `static_assert`s on sizes and
offsets. It was checked, together with `particles_cb.h`, with `clang++ -std=c++17 -fsyntax-only` against
`external/donut/include`.

- `ObjectConstants.padding` is `uint padding[2]` at offset 384 in a cbuffer, so each element occupies one register
  and the cbuffer size is 404. The C++ branch spells this out (`_alignPadding[2]`, then `uint4 padding[2]`).
- The strings in `Asteroids.exe` (`FrameRendererConstants`, `MeshletInfoConstants`, `InstanceConstants`,
  `InstanceConstantsPrevious`, `SectorConstants`, `ObjectConstants`, `StatsUAV`, `DebugUAV`) are buffer debug names.
  The struct names used here are the HLSL reflection names.

## How the NVAPI extension ops map to SM 6.5

NVAPI calling convention (`nvHLSLExtnsInternal.h`): `i = g_NvidiaExt.IncrementCounter()`, then write `opcode` and
the `srcNu` fields of element `i`, then `result = g_NvidiaExt.IncrementCounter()`. When an op returns several
values (`numOutputsForIncCounter = n`), each further `IncrementCounter()` returns the next one. Every original
entry point also writes `12345.0` to all of its dummy vertex-shader outputs. That code was only there to satisfy
the VS stage and is dropped.

Opcodes 1 and 7 are public (`nvShaderExtnEnums.h`). Opcodes 34–48 are not public. Their meaning below is
inferred from how the operands are used across all eight shaders.

| op | operands (as observed) | inferred meaning | SM 6.5 replacement |
|---|---|---|---|
| 1 `NV_EXTN_OP_SHFL` | src0u.x = value, .y = source lane, .z = 0x1F (width 32) | warp shuffle | `WaveReadLaneAt(value, 0)` (asteroidMS) |
| 7 `NV_EXTN_OP_VOTE_BALLOT` | src0u.x = predicate | warp ballot | `WaveActiveCountBits` + `WavePrefixCountBits` (particles; the original used `countbits(ballot)` and `countbits(ballot & ((1<<lane)-1))`) |
| 34 | src0u.x = count; TS, lane 0 only | set the number of mesh tasks to launch | `DispatchMesh(count, 1, 1, payload)`, called by the whole group after a groupshared barrier |
| 36 | returns a value | workgroup index. TS: task-group index in the draw (indexes `instanceInfoBuffer`). MS: index of the mesh group among those its task launched (the draw index for particles, which has no TS) | `SV_GroupID.x` |
| 37 | returns a value | thread index in the 32-thread group (lane) | `SV_GroupThreadID.x` |
| 38 | src0u.x = 0 (SV_Position) or 6 (generic attribute), .y = attribute slot = output register in the dummy VS signature, .z = 0xFF (mask), src1u.x = vertex index, src2u = 4 dwords | write a per-vertex output | `verts[v].<field> = value` |
| 39 | as 38, but src1u.x = primitive index | write a per-primitive output | `out primitives` struct (debugMS) |
| 42 | src0u.x = primitive count | set the primitive count (the vertex count is never set) | `SetMeshOutputCounts(vertexCount, primCount)`, moved before the first output write as SM 6.5 requires, called once and uniformly |
| 43 | src0u.x = flat index slot (3·prim + corner), src1u.x = vertex index | write one index of the index buffer | `tris[prim] = uint3(...)` |
| 46 | src0u.x = dword count, src0u.y = 1 for float / 0 for uint, src1u.x = byte offset, src2u = data; TS only | write task output (payload) | fields of the groupshared `AsteroidTaskPayload` |
| 47 | src0u.x = 0 for uint / 1 for float, src1u.x = byte offset, numOutputsForIncCounter = dword count; MS only | read task output (payload) | `in payload AsteroidTaskPayload` |
| 48 | no operands; issued once after all vertex and index writes, just before op 42 | presumably an output commit or barrier | nothing needed |

The payload offsets are confirmed by the reads and writes. The TS writes 4 floats at byte 8, then 2 uints at
byte 0. The MS reads 1 uint at byte 0, 4 floats at byte 8, and (debugMS only) 1 uint at byte 4. That gives
`AsteroidTaskPayload = { uint asteroidIndex; uint numMeshTasks; float lod; float lodAlpha; float distanceAlpha;
float numMeshletsFirstLod; }` (24 bytes).

In the 2018 code only lane 0 of a task group runs the whole evaluation and issues op 34. In SM 6.5,
`DispatchMesh` must be called by the whole group, so thread 0 writes the payload and the task count to
groupshared memory, then the group syncs and every thread calls `DispatchMesh`.

Generic attribute slot N of op 38 is output register N of the dummy VS signature, so the PS links to it by that
register's semantic. The ported output structs reproduce those registers and semantics. The resulting MS output
signatures equal the originals' VS output signatures for asteroidMS, basicMS (DEPTH_PRE_PASS=0) and particles_ms.

## Per shader

Bindings below are copied from the reflection and verified against the rebuilt binaries (see Verification).
Every NVAPI shader also had `g_NvidiaExt` at `u7`, or at `u0` for particles. That binding is removed.

### asteroidTS (`ts_main`)
Purpose: one task group per asteroid instance. Thread 0 does the following:
1. Computes the world-space bounding sphere: `center = mul(instanceMat, float4(cbObjectInfo.center,1)) + sectorOffset`,
   `radius = cbObjectInfo.radius * uniformScale`.
2. Frustum-culls the sphere against `frustumPlanes[0..5]` if `enableAsteroidsCulling` is set
   (`dot(n,c) - w > r` means culled).
3. Applies the view-distance fade if `enableViewDistanceFade` is set. `t_ViewDistance` is sampled at the
   sphere's top and bottom points: `saturate((dist(u = dir.y*0.5+0.5) - d) * 0.001)`, and the maximum of the
   two is taken. A result `<= 1e-4` culls the asteroid.
4. Runs the Hi-Z test when `_MESHLETS_HI_Z=1` (`IsOcclusionCulled`):
   - Projects the 8 corners of `cbObjectInfo.bbox` and builds the screen rectangle and the minimum NDC z.
   - Gives up (not culled) if any `w <= 0`.
   - Picks `mip = ceil(log2(maxRectSide * invZFarTileSize))` and gives up if `mip >= zFarNumLevels`.
   - Samples `t_ZFar` at the rectangle centre at that mip. The asteroid is culled if `minZ > zFar`.
   - On a cull, `u_Stats[0]` is incremented if `enableZCullStats` is set. `enableZCull` is not read; the
     permutation is selected on the CPU.
5. Selects the LOD:
   - `screenDiameter = 2r * max(P00*rtDims.x, P11*rtDims.y)`, divided by `max(0.01, |viewVec|)` unless
     `orthographicProjection` is set.
   - `lod = screenDiameter < 1 ? 0 : clamp((log2(sd) + obj.lodBias) * lodSlope + lodBias, 0, maxLevelToRender)`.
   - If `enableLod` is 0, the LOD is `clamp(forcedLod, 0, maxLevelToRender)` instead.
   - If `transitionRange > 0`, `alpha = saturate((frac(lod) - (1 - range)) / range)` and `lod = floor(lod) + alpha`.
     Otherwise `lod = floor(lod)`.
   - The number of mesh tasks is `numMinfo(floor(lod))`, plus `numMinfo(floor(lod) + 1)` when `alpha != 0`.

Bindings: `cbFrame b0`, `cbObjectInfo b4`, `cbSectorInfo b5`, `s_ViewDistanceSampler s0`, `t_ViewDistance t16`,
`lodInfoBuffer t10`, `instanceInfoBuffer t11` (all space1). With HI_Z=1 it also uses `s_ZFarSampler s1`,
`t_ZFar t17` and `u_Stats u1`.

Permutations: `DEPTH_PRE_PASS={0,1}` only changed the dummy VS output struct of the 2018 binary (Output vs
DepthOutput); the instruction streams are otherwise identical, so the define has no effect here.
`_MESHLETS_HI_Z={0,1}` toggles step 4.

### asteroidMS (`ms_main`)
Purpose: renders one meshlet per mesh group. Groups below `numMeshletsFirstLod` draw LOD `floor(lod)`; the
rest draw LOD `ceil(lod)`, indexed relative to `numMeshletsFirstLod`.
- `LODInfo` gives `minfoStart`, `indexStart`, `vertexStart` and `primStart`.
- Thread 0 culls the meshlet sphere (`earlyCullUsingSphere`). The sphere comes from the quantized meshlet AABB:
  `w0` bytes are the minimum and `w1` bytes the maximum, each `/255` across `bboxLods[lod]`. The radius is
  `0.5 * |max-min| * uniformScale`. The plane test is not gated by `enableAsteroidsCulling`.
- The result is broadcast with `WaveReadLaneAt`. A culled meshlet outputs nothing.
- Vertices: `vertexIndex = indexBuffer[indexStart + meshlet.z + i] + vertexStart`, then
  `world = mul(instanceMat, (pos,1)) + sectorOffset` and `SV_Position = mul((world,1), matWorldToClip)`.
- Outputs:
  - `ATTR1 = (world, u)`
  - `ATTR2 = (mul(instanceMat, (n,0)), 1 - v)`
  - `ALPHA_LOD = (enableLod ? lodAlpha : 0, lodIndex 0/1, distanceAlpha, lod)`, nointerpolation
- Triangles: 3 × u8 at byte `primStart + meshlet.w + 3p`, read with an unaligned dword load.

Bindings: `cbFrame b0`, `cbObjectInfo b4`, `cbSectorInfo b5`, `vertexPositionBuffer t0`, `vertexNormalBuffer t1`,
`vertexTexcoord1Buffer t2`, `indexBuffer t6`, `primBuffer t7`, `meshletBuffer t8`, `lodInfoBuffer t10`,
`instanceInfoBuffer t11`. DEPTH_PRE_PASS=1 drops t1 and t2.

Permutations: with `DEPTH_PRE_PASS=1` only SV_Position is written (DepthOutput).

### basicTS (`ts_main`)
Purpose: launches `cbMeshletInfo.numMeshlets` mesh groups. There is no culling.
Bindings: `cbMeshletInfo b1, space1`.
Permutations: `DEPTH_PRE_PASS={0,1}`, `_MESHLETS_HI_Z=0`. Neither define has any effect.
basicMS reads no payload; a 1-uint dummy payload is required by `DispatchMesh`.

### basicMS (`ms_main`)
Purpose: generic meshlet objects, e.g. the ship.
- Each group draws `meshlet = meshletBuffer[firstMeshlet + SV_GroupID.x]`.
- `world = mul(cbInstance.instanceMat, (p,1))`; `prev = mul(cbInstancePrev.instanceMat, (p,1))`.
- Normal, tangent and bitangent are snorm8x3 values decoded by hand and transformed with `cbInstance` (w = 0).
- Outputs: `Output {SV_POSITION, POS, UV, centroid NORMAL/TANGENT/BITANGENT, PREV_WORLD_POS}`.

Bindings: `cbFrame b0`, `cbMeshletInfo b1`, `cbInstance b2`, `cbInstancePrev b3`, `vertexPositionBuffer t0`,
`vertexNormalBuffer t1`, `vertexTexcoord1Buffer t2`, `vertexTangentBuffer t4`, `vertexBitangentBuffer t5`,
`indexBuffer t6`, `primBuffer t7`, `meshletBuffer t8`. DEPTH_PRE_PASS=1 uses only b0, b1, b2, t0, t2, t6, t7 and t8.

Permutations: with DEPTH_PRE_PASS=1 the 2018 shader declared `DepthOutput {i_position}` but still wrote the UV to
attribute slot 1. The port keeps that as `float2 m_uv : UV` in DepthOutput.

### debugTS (`ts_main`)
Purpose: per-asteroid bounding-box debug view. It repeats asteroidTS steps 1–3 and the LOD computation, writing
`lod`, `lodAlpha`, `distanceAlpha` and `numMeshletsFirstLod`. It then launches exactly `showBBoxes ? 1 : 0` mesh
groups, regardless of culling. When the asteroid is culled or faded out, the float part of the payload is left
unwritten, as in the original. There is no Hi-Z and no two-LOD task count.
Bindings: as asteroidTS without HI_Z.
Permutations: only `DEPTH_PRE_PASS=0 _MESHLETS_HI_Z=0` was shipped.

### debugMS (`ms_main`)
Purpose: draws `cbObjectInfo.bbox` (the whole-object box, not `bboxLods`) of the asteroid as 8 vertices and 12
triangles, using the static index list `bboxIB[36]` copied from the binary. The box is drawn only if
`showBBoxes` is set and this is the last mesh group (`SV_GroupID.x == numMeshTasks - 1`). Otherwise the original
set no counts at all; the port calls `SetMeshOutputCounts(0, 0)`. This was `DrawAsteroidsBBox` in the original.
- Threads 0–7: corner i is projected with `matWorldToView` and then `matViewToClip`, and written to the original
  slots:
  - POS = world position
  - UV = (1, 0)
  - NORMAL = (1, 0, 0)
  - BITANGENT (slot 5) = `world + preViewTranslationPrevious - preViewTranslation`
  - PREV_WORLD_POS (slot 6) is not written by the original; the port sets it to 0.
- Threads 8–19: triangle t gets the per-primitive slot 4 (TANGENT) value `(lodAlpha, lodIndex, distanceAlpha, lod)`
  and indices `bboxIB[3t..3t+2]`.

Bindings: `cbFrame b0`, `cbObjectInfo b4`, `cbSectorInfo b5`, `instanceInfoBuffer t11`.

### particles_ms (`ms_main`, no task shader)
Purpose: each group handles 32 particles, `t_Particles[group*32 + lane]`.
- `p = position + positionOffset` (camera-relative) and `clip = mul((p,1), matWorldToClip)`.
- A particle is visible if `w > 0`, `|p| <= maxDistance`, `|x| <= w` and `|y| <= w`.
- Visible particles are compacted with the ballot. Each emits an equilateral triangle
  (`TrianglePoints`, inradius 1), offset in clip space by `screenScale * max(0.3, 0.003 w)`.
  `UV` = the triangle point, `COLOR` = nointerpolation.
- Colour:
  - `rgb = radiance/8 * angularSize² * brightness * light.color * (shadow * phase + 0.2)`
  - Phase: `82.5119705 t³ + 0.795774698`, with `t = (1-0.9f) / sqrt(max(1.81 - 1.8 cosθ, 0))`; this is g = 0.9
    forward scattering plus an isotropic term. The constants are kept bit-exact.
  - Shadow: up to 4 cascades from `light.shadowCascades`, each a single `SampleCmpLevelZero` with edge fade,
    accumulated with `saturate(s * (1.0001 - acc.y) + acc)`, then `acc.x + (1 - acc.y) * outOfBoundsShadow`.
  - `a = saturate((1 - d/maxDist) * 10) * pow(0.3 / max(0.3, 0.003 w), 1.5) * saturate(0.02 d - 1)`.

Bindings (space0): `g_Particles b0`, `s_ShadowSampler s0` (comparison), `t_ShadowMapArray t0`, `t_Particles t1`.

### create_hi_z_cs (`main`, 16×16×1)
Purpose: builds the 5-level max-depth pyramid.
- Each thread computes the maximum over an 8×8 depth tile with 16 `Gather`s (offsets 0, 2, 4, 6) at
  `uv = (tile*8 + 1) / dims`, starting from 0, and writes it to `u_ZFar[0][DTid]`.
- Four groupshared 2×2 reductions follow, writing `u_ZFar[1..4][groupId * (8,4,2,1) + tid]`, with
  `GroupMemoryBarrierWithGroupSync` between the steps. The barrier sequence and the `all(tid < size)` guards
  match the original exactly.

Bindings: `s_Sampler s0`, `t_ZBuffer t0` (Texture2D<float>), `u_ZFar[5] u0` (RWTexture2D<float>).

## NVAPI originals (`demo/nvapi/`)

These reproduce the 2018 binaries, NVAPI calls included, and are compiled exactly as shipped (`vs_6_0`, entry
`ts_main` / `ms_main`, same permutations). `nvHLSLExtns.h` comes from the NVAPI SDK (found or fetched by nvrhi through EPM), which is added to the
ShaderMake include paths in `asteroids/CMakeLists.txt`.
- **Opcode helpers.** `nv_meshlet_extns.hlsli` defines `NV_EXTN_OP_MESH_*` 34..48 and the helpers
  (`__NvMeshGetGroupId`, `__NvMeshGetThreadId`, `__NvMeshSetTaskCount`, `__NvMeshSetPrimitiveCount`,
  `__NvMeshCommitOutputs`, `__NvMeshSetPrimitiveIndex`, `__NvMeshSetPosition`, `__NvMeshSetVertexAttribute`,
  `__NvMeshSetPrimitiveAttribute`, `__NvMeshWriteTaskOutput*`, `__NvMeshReadTaskInput*`). They store in the
  original order, opcode first and then the operands (the public `NvShfl` / `NvBallot` store the operands first,
  as the binaries show).
- **Extension slot.** `NV_SHADER_EXTN_SLOT` is u7 by default; `particles.hlsl` uses u0.
- **Shared logic.** The algorithm code is shared with the SM 6.5 files through `include/meshlet_common.hlsli`
  (now also `IsOcclusionCulled`, `earlyCullUsingSphere` and the payload byte offsets) and
  `include/particles_ms_common.hlsli`.
- **Mesh shader structure.** Each one is `void ms_body()` plus `ms_main()`, which calls it and returns the 12345.0
  dummy output (`nv_outputs.hlsli`: Output / DepthOutput / basic Output with the original semantics and
  interpolation modes).

Verification (`build/shader_check/meshlet/check_nvapi.py`, dxc 1.8.2502.8): every permutation is compiled and
compared with the original listing. Compared items:
- the output signature;
- the buffer definitions;
- the resource bindings, including `g_NvidiaExt` and the resource IDs;
- the exact sequence of NV extension operations (opcode and operand stores to `g_NvidiaExt`, in order);
- the dx.op histogram and an instruction-stream similarity.

| permutation | signature / buffers / bindings / NV op sequence | instructions (rebuilt vs original) | stream similarity |
|---|---|---|---|
| asteroidTS DPP=0 HI_Z=0 / DPP=1 HI_Z=0 | identical | 261 vs 258 / 249 vs 246 | 0.37 / 0.39 |
| asteroidTS DPP=0 HI_Z=1 / DPP=1 HI_Z=1 | identical | 663 vs 635 / 651 vs 623 | 0.53 / 0.54 |
| asteroidMS DPP=0 / 1 | identical | 449 vs 445 / 348 vs 349 | 0.77 / 0.82 |
| basicTS DPP=0 / 1 | identical | 32 vs 32 / 15 vs 15 | 1.00 / 1.00 (histogram identical) |
| basicMS DPP=0 / 1 | identical | 422 vs 419 / 176 vs 176 | 0.72 / 0.96 |
| debugTS | identical | 256 vs 254 | 0.80 |
| debugMS | identical | 312 vs 312 | 0.94 |
| particles | identical | 342 vs 338 | 0.86 |

The remaining dx.op differences all come from the newer compiler:
- `FMad(1.0, x, y)` is folded (fewer FMad);
- one `floor` is CSE'd (`Round_ni` 3 vs 2);
- cbuffer reloads differ by 1 to 3.

The low asteroidTS similarity is instruction scheduling of the long straight-line frustum / Hi-Z code; the
histograms match apart from the items above.

## Uncertain points / open questions

1. The meaning of opcodes 34–48 is inferred, as described above. Op 48 in particular is assumed to be a
   harmless commit or barrier.
2. Wave size: the SHFL and ballot replacements assume one wave covers the 32-thread group (wave size ≥ 32, true
   on NVIDIA and AMD). SM 6.5 cannot express `[WaveSize]`. On hardware with narrower waves, asteroidMS's
   broadcast and particles' compaction would need a groupshared fallback.
3. `vertexTexcoord2Buffer` is declared but never referenced in any binary. Its register (`t3, space1`) is inferred
   from the gap. A `DebugUAV` exists on the C++ side but appears in none of these binaries; it is not declared.
4. In basicMS DEPTH_PRE_PASS=1, the original writes the UV to slot 1 although its declared output has only
   SV_Position. The port emits it as `UV`. Which PS (if any) is bound in the ship depth pre-pass is unknown.
5. debugMS slot use: slot 5 (BITANGENT) holds the previous position, slot 4 (TANGENT) holds per-primitive LOD
   data, and slot 6 is never written. This looks like an off-by-one in the original or a debug-only PS layout;
   it is kept slot-faithful. The PS paired with debugTS/MS is unknown (`debugTS`/`debugMS` do not appear as
   strings in `Asteroids.exe`, so the pipeline may be unused). debugTS also leaves the payload floats undefined
   for culled asteroids.
6. `DispatchMesh` allows at most 65535 groups per dimension. basicTS dispatches `numMeshlets` in X. The CPU must
   launch asteroidTS with ≤ 65535 asteroids per X dimension (`SV_GroupID.x` indexes `instanceInfoBuffer`).
   The 2018 CPU code launched them with NVAPI DispatchMeshTasks (0x140001AD0): `instanceCount` task groups per
   asteroid draw, 1 per space-object instance, `numParticles >> 5` mesh groups for particles.
7. Mesh output limits: asteroidMS and basicMS declare 64 vertices / 100 primitives (meshlet limits), debugMS
   8 / 12, and particles_ms 96 / 32 (one triangle per thread). The 2018 PSOs declared 64 / 100 for all meshlet
   pipelines (including debugMS) and 96 / 32 for particles (0x140067DA0), with 32 threads.
8. `IsOcclusionCulled(float3x4, uint)`: the second parameter is unused after inlining in the shipped code and is
   kept as `asteroidIndex`. The `SetupParticle` / `DrawAsteroidsBBox` parameter lists are approximations
   built from the mangled names; they do not affect the logic.
9. HLSL 2021 (the dxc 1.8 default) short-circuits `&&`/`||`. All such conditions here are side-effect free, so
   `-HV 2018` is not needed.

## Verification (dxc 1.8.2502.8)

Scratch tools are in `build/shader_check/meshlet/`:
- `check.py` compiles every permutation and diffs the "Buffer Definitions" and "Resource Bindings" of
  `dxc -dumpbin` against the original listing. It strips `g_NvidiaExt`, normalizes `dx.alignment.legacy.` and
  `hostlayout.` prefixes, and masks UAV IDs, which shift by one without `g_NvidiaExt`.
- `ophist.py` compares intrinsic histograms.
- `cmp_body.py` produces a normalized instruction diff.

| permutation | compile | cbuffer/struct layouts | bindings (incl. resource IDs) |
|---|---|---|---|
| asteroidTS DEPTH_PRE_PASS=0/1 × _MESHLETS_HI_Z=0/1 (4) | OK, no warnings | identical | identical |
| asteroidMS DEPTH_PRE_PASS=0/1 (2) | OK | identical | identical |
| basicTS DEPTH_PRE_PASS=0/1, HI_Z=0 (2) | OK | identical | identical |
| basicMS DEPTH_PRE_PASS=0/1 (2) | OK | identical | identical |
| debugTS DEPTH_PRE_PASS=0, HI_Z=0 (1) | OK | identical | identical |
| debugMS DEPTH_PRE_PASS=0 (1) | OK | identical | identical |
| particles_ms (1) | OK | identical | identical |
| create_hi_z_cs (1) | OK | identical | identical |

- **create_hi_z_cs (not ported):** the signatures match (none). The instruction streams have the same length,
  310 instructions. The only differences are the scheduling of two `groupId`/`threadIdInGroup` reads and one
  `uitofp`. The Gather offsets, max tree, barrier sequence and stores are identical.
- **Ported shaders:** the MS output signatures equal the originals' VS output signatures. The exceptions are
  debugMS (TANGENT moves to the per-primitive signature) and basicMS DEPTH_PRE_PASS=1 (UV added). Apart from the
  replaced NVAPI ops, the intrinsic histograms differ only for these reasons:
  - `BufferLoad` became `RawBufferLoad` (newer DXIL).
  - `FMad(1.0, x, y)` and `FMad(0, …)` were folded into `fadd`.
  - Redundant cbuffer reloads and one `floor` were CSE'd.
  - particles' two `Countbits` became wave ops.

  Hand review confirmed the control flow and constants against the original listings. The particle constants
  are bit-identical (e.g. `0x3FB9999A…`, `0x4054A0C4…`, `0x3FEFFFFDE…`).
