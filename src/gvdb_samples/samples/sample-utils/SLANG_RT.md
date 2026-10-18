# Portable mixed-scene ray tracing

`SlangSceneRT` renders indexed triangle meshes and sparse GVDB volumes in one NVRHI scene on D3D12, Vulkan RT and CUDA/OptiX. Application code uses NVRHI resources and offline Slang artifacts. D3D11 has no implementation of this native RT renderer; its existing compute volume renderer remains available.

This is an implementation and validation record for the new foundation. It does **not** claim that the full legacy application migration or original-asset comparison gates are complete. Analytic optics, lighting, environment and random-state tests pass on all three APIs. Explosion/Lucy, point-cloud and fluid/ground comparisons and application integration remain separate gates.

## Scene and resource contract

- `SceneDescription` contains meshes, volume resource snapshots, instances and materials. Mesh positions/normals are float4 records; indices are uint32. Missing normals use geometric triangle normals. Instance transforms support nonuniform scale; normals use the inverse transpose.
- A `SceneVolume` retains its scalar, topology, optional packed RGBA8 color and transfer resources. Scalar and color atlases use exactly the same voxel slots. Replace the snapshot after atlas or topology relocation.
- At most eight volume resource sets are supported. Six fixed descriptor arrays at t16/t24/t32/t40/t48/t56 hold scalar/nodes/children/levels/colors/transfer data. Every unused descriptor has a valid dummy allocation. Nonuniform indexing, descriptor counts, subranges and ownership are independently tested by `slang_resource_arrays` on all three native RT backends.
- Primary output is float4 color and float depth. Accumulation is a separate float4 buffer. Normal renders have alpha one. Miss depth is the retained legacy sentinel `1e10`; material shading reports distance to its biased shading point, which can differ from the primitive hit distance.
- Geometry and topology replacement currently waits for submitted GPU work before replacing BLAS/TLAS owners. Renderer destruction also waits. Pending output buffers remain valid after replacement/destruction, but these synchronization points are **not** an asynchronous update feature.
- No bindless/unbounded descriptor arrays, local material shader records, time-sampled transforms or motion blur are implied by this implementation.

The GPU structures are explicitly packed: volume metadata is 160 bytes, materials are 128 bytes, and scene constants are 272 bytes (the host allocates a 512-byte constant buffer). `SlangSceneRTData.h` is the host layout contract. `options.z` is an inclusive transfer-function endpoint, not the descriptor count. Shader payloads are ordinary 32-byte values; procedural hit attributes occupy 20 bytes. No device pointers occur in the public scene data or ray payloads.

## Volume controls and retained rendering rules

`SceneVolume.steps` contains direct step, shadow step, fine step and leaf-entry epsilon. `extinct` contains the signed extinction coefficient and albedo. `cutoff` contains minimum scalar contribution and the remaining-transmittance cutoff. `threshold` contains isovalue, minimum, maximum and the scalar background used for missing sparse cells.

The coefficient must be nonpositive; direct/shadow/fine steps are positive. Transfer alpha is nonnegative. The shader does not read stale apron values to fill sparse holes: missing corners use `threshold.w`. Positive-outside level sets generally need a positive missing-cell background, such as three for an isovalue of zero. Raw RGBA8 colors use nearest sampling at the hit/march position, matching the legacy point-filtered color channel.

The four volume modes retain the relevant OptiX reference behavior:

| Mode | Intersection and shading |
| --- | --- |
| Surface | First allocated sample at or above the isovalue; negative scalar gradient; shading point shifted two index-space units along that normal. |
| LevelSet | First allocated sample below the isovalue; positive scalar gradient; no two-voxel shift. Refraction rays use the reference density-sign intersection, and the second refraction depth excludes level sets. |
| Deep | Integrates allocated leaf samples, then composites the packed result over a continuation ray that excludes deep volumes. |
| EmptySkip | Debug shading of the first allocated brick entry. This is not an isosurface renderer. |

Every leaf restarts the direct-step sampling wavefront after its entry epsilon. For example, two adjacent width-two leaves traversed with step 0.25 and a positive small epsilon produce seven samples per leaf, not fifteen samples across their combined width.

Deep transfer coordinates are `saturate((scalar - isovalue) / (maximum - minimum))`. `transferLastIndex` defaults to `UINT32_MAX`, which resolves to the final entry of the supplied table. **Legacy adapters must explicitly set `transferLastIndex = 16300` when importing the original 16384-entry table.** This preserves the reference lookup without silently changing arbitrary-sized transfer tables. Explicit endpoints must be smaller than the table length.

For a contributing sample, the retained equations are:

```
a = exp(extinct.x * transfer.a * steps.x)
RGB += transfer.rgb * T * (1 - a) * extinct.y * color.rgb
T *= a
```

The accumulated RGB and opacity `1 - T` are clamped/rounded to RGBA8, preserving the legacy quantization. Closest-hit then evaluates `lerp(behindScene, packedRGB, packedOpacity)`. This applies opacity to the already accumulated RGB again, as the retained legacy shader does. It is not an assertion that this is the physically preferred compositing convention.

Some exposed legacy fields have no active effect in this OptiX-derived path. `steps.y` belongs to the separate legacy shadow-brick integrator, whereas the RT shadow group uses the same deep integrator. `steps.z` belongs to the legacy level-set fine loop, which immediately returned on its already-below-threshold starting sample; the current RT intersection stops at that sample. The values are retained and validated, not advertised as independently functioning quality controls. A future improved mode must be explicit and independently tested.

Primary deep integration and its non-deep continuation respect the camera near/far
interval. The integration limit is independent of the current nearest triangle,
so BVH traversal order does not alter the result. Integration runs in the hit
shader, where the payload distinguishes primary and secondary rays; the
intersection shader identifies the first allocated sample. Analytic tests check
clipping inside both leaves and a nearer triangle. Secondary rays retain their
own range. OptiX closest-hit transforms world rays to object space because its
object-ray getters are unavailable in that stage.

## Materials, lighting and secondary rays

Material fields retain the `SceneMaterial`/legacy parameter meanings and defaults. `setPointLight` supplies a world-space point position; the camera's compute-renderer directional-light field is not repurposed. Mesh normals face the incident ray. Degenerate gradients/normals use finite fallback directions.

The original depth limits are one reflection, two refractions and one shadow level. Each shaded surface sends two shadow samples. Reflection and refraction activation follows their respective positive width fields; a zero reflection width means disabled, not an ideal mirror. Refraction uses entering/exiting index-of-refraction handling and suppresses its contribution on total internal reflection. `refrOffset` must be positive when refraction is enabled.

`shadowWidth` is retained but unused by the original RT shader: both light and shadow jitter use `lightWidth`. Shadow rays follow the reference's distant ray limit rather than stopping at the point-light distance. The first accepted deep shadow hit returns its transmittance; opaque mesh/surface hits return zero. These are retained behaviors, not additional physical-lighting claims.

The environment texture is RGBA8 UNORM with linear filtering, wrapping U and clamping V. Its upper-hemisphere coordinates are `u = atan2(dir.x, dir.z) / pi`, `v = 1 - dir.y`. Below the horizon the reference color is 0.1; without a texture, the upper hemisphere is white. Miss radiance multiplies the environment by the camera background RGB. The material environment field also retains the reference's checkerboard-ground convention.

## Camera, samples and intentional corrections

Camera vectors follow `RaycastParameters`: `direction00` is the lower image-corner direction, with pixel centers at `(x + 0.5, y + 0.5)`. Primary rays add the reference's three-dimensional jitter scaled by `length(directionDx)`. The retained OptiX application used integer pixel-corner coordinates. An adapter seeking that exact raster convention can subtract half of `directionDx + directionDy` from `direction00`; its view-Z clipping center must remain consistent with the chosen convention. Comparisons should state the camera convention explicitly.

`direction00.w == 0` uses ray-distance near/far bounds. A value of one interprets them as view-Z planes using the normalized image-center direction. Other values are rejected. These bounds apply to the primary ray; secondary rays preserve the legacy limits.

`render(camera)` advances samples automatically. Explicit sample zero resets the sequence; any other explicit sample must equal the next expected sample index. Camera, scene, geometry, material, environment, point-light, seed and resolution changes reset accumulation.

Two corrections are intentional:

1. Sample zero initializes accumulation and sample one contributes to the average. The reference restarted both samples zero and one.
2. Random state is independent per pixel, initialized deterministically from pixel, sample, dataset frame and seed, then carried through the ordinary ray payload. It retains the original LCG and within-path jitter consumption (primary jitter does not advance the path seed), but removes the reference's shared modulo seed slots and host `srand` dependence. Exact random sequences therefore differ from the legacy program.

## Validation and remaining gates

The analytic suite passes on D3D12, Vulkan RT and OptiX. It checks distinct instance materials, deep attenuation and mesh continuation, all four volume groups, explicit transfer endpoints, resource slots zero/seven, negative-coordinate sparse reverse traversal, geometry/scene replacement lifetimes and invalid inputs. Its advanced cases verify deep shadow attenuation and opaque blockers, reflected/refracted scene targets, total internal reflection, inverse-transpose normals under nonuniform scale, bilinear environment filtering with wrap/clamp, deterministic seed/frame changes and an arithmetic progressive mean formed from independent samples. D3D12/Vulkan native validation is clean and CUDA Compute Sanitizer reports zero errors for the expanded suite.

Run `ctest -R '(gvdb_scene_rt|slang_resource_arrays)' --output-on-failure`. Native graphics checks use `gvdb_scene_rt_tests -dx12 --validation` and `-vk --validation`; CUDA memory checking uses `compute-sanitizer --tool memcheck --error-exitcode 91 gvdb_scene_rt_tests -cuda`.

Original-asset comparisons and production application migrations must still pass before declaring the complete mixed-scene project finished or removing retained legacy implementations. Passing analytic cases does not establish image parity for every material or asset.
