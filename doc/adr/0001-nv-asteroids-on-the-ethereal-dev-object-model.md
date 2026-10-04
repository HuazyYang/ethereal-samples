# 0001. nv_asteroids builds on the ethereal-dev object model, not a second Donut

- Status: Accepted
- Date: 2026-10-04

## Context

`demos/nv_asteroids` is a source reconstruction of the 2018 NVIDIA *Asteroids* meshlet demo. It was
developed in a standalone repository against **vanilla donut `main`** (`721014c`) with its nvrhi
(`c8d34b4`), and it reached the original's own capture-to-capture PSNR in four of six camera presets
while running 1.4 % faster than the original on the reference GPU.

This tree's Donut is the `ethereal-dev` fork (`d4e24c0`) with `nvrhi/ethereal-dev` (`0734789`). The
difference is not a version bump but an object model:

| Vanilla donut `main` | `ethereal-dev` |
| --- | --- |
| `std::shared_ptr<T>`, `std::make_shared<T>` | `nvrhi::AutoPtr<T>`, `MAKE_RC_OBJ_PTR`, raw `T*` at API boundaries |
| plain classes | `nvrhi::ObjectImpl` / `WeakReferenceSourceImpl` with explicit interface tables |
| handle-returning factories | out-parameter factories returning `FRESULT` |
| `vfs::IBlob` | `nvrhi::IDataBlob` |
| `dynamic_cast` | `QueryInterface` (nvrhi ADR 0006) |
| `NativeObject` union with `.integer` | `void*` |
| ShaderMake, `donut_compile_shaders()` | ShaderTool, `ethereal_compile_shaders()` |
| `app::ImGui_Renderer` with a font registry | `app::ImGuiRenderPass`, no font registry |
| camera basis `right = cross(dir, up)` (mirrored) | left-handed basis `right = cross(up, dir)` |

A demo cannot straddle both: each Donut defines the same `donut_core` / `nvrhi` CMake targets, so one
configure can only contain one of them.

## Decision

1. **Port the demo onto this tree's Donut.** The aggregate keeps exactly one Donut, and nv_asteroids
   builds and links alongside every other sample.
2. **Convert only donut-owned types.** `std::shared_ptr` becomes `nvrhi::AutoPtr` where the type
   argument names a refcounted donut class. The demo's own classes — `SceneNode`, `SpaceObject`,
   `SpaceScene`, `SceneLight`, the `*2018` passes, `chunk2018::MeshSet`, `fx::*` — keep
   `std::shared_ptr`, because they are not nvrhi objects and converting them would mean giving each
   one an interface table for no gain. The demo defines its own `MeshInfo`, `MeshInstance` and
   `Material` in `src/meshlets/ChunkMeshSet.h`, which is why those donut names are deliberately
   outside the conversion.
3. **Six demo classes gain `NVRHI_INHERIT_INTERFACE_TABLE()`**, because they derive from a refcounted
   donut class: `FeatureDemo`, `UIRenderer`, `SQLiteFileSystem`, `OverlayFileSystem`,
   `MeshletDrawStrategy`, `DemoLightProbe`. None of them adds an interface or a class ID, so each
   inherits its base's table rather than declaring one.
4. **Split the shaders into two ShaderTool configs by shader model.** ShaderTool takes the shader
   model per invocation, so `shaders/shaders_sm65.cfg` holds the seven meshlet amplification/mesh
   rows and `shaders/shaders_sm60.cfg` the other 49. Keeping the second group at 6.0 is a
   correctness requirement, not inertia: from SM 6.2 ShaderTool passes `-enable-16bit-types`, which
   would change `min16float` away from the "may be 32-bit" semantics the 2018 shaders were compiled
   with. Both configs write into one output directory, which is what the demo's `ShaderFactory` base
   path points at.
5. **The demo is opt-in** behind `ETHEREAL_BUILD_NV_ASTEROIDS` (default OFF), because it only
   configures with the original 2018 demo installed: it reads the original's `media.db` and links
   import libraries generated from the DLLs the original shipped (`NV_ASTEROIDS_ORIGINAL_DIR`).
6. **Targets and cache variables are prefixed `nv_asteroids_` / `NV_ASTEROIDS_`**, because
   `benchmark/Asteroids` already owns `asteroids_core` and the `ASTEROIDS_*` variables. The C++
   macros the demo's sources read (`ASTEROIDS_SHADER_DIR`, `ASTEROIDS_WITH_NVAPI`, ...) keep their
   names: they are private to the target and renaming them would churn the reconstructed sources for
   nothing.

7. **Render left-handed; reconcile the content's handedness in the projection.** Donut's camera
   builds the left-handed basis (`right = cross(up, dir)`); donut `main` built the mirrored one, and
   the 2018 content was authored through it. The render pipeline is left-handed: every
   `engine::IView` carries Donut's camera matrix unchanged, so all view-space work happens in
   Donut's view space. Since Donut's camera is a proper rotation, one reflection has to sit between
   world and clip to reproduce the original; it is placed in the projection
   (`demo::MirrorProjectionX`, `src/app/RenderHandedness.h`), so world-to-clip, the winding, the 2018
   rasterizer states and the reconstructed shaders stay as they were. Code that reads the
   projection's focal terms as sizes takes their magnitudes (`demo::ProjectionScale`); HBAO+, which
   assumes positive focal terms, gets the same world-to-clip transform with the reflection on its
   world-to-view matrix instead.

## Consequences

### Positive

- One Donut in the tree. The demo is part of the normal build and moves with the framework.
- The port is a measurable regression test for the fork: the demo renders a 264 k-object meshlet
  scene through 20-odd passes, so an image or timing difference against the `recon` build points at
  the object model or the shader build, not at the demo.
- It found two defects in the framework, both of which would have bitten the next user:
  `ChunkFile::deserialize` released a blob it never referenced (a use-after-free on every chunk
  asset), and `render/SsaoPass.h` / `render/TemporalAntiAliasingPass.h` hold an
  `AutoPtr<FramebufferFactory>` behind a forward declaration, which `AutoPtr`'s destructor cannot
  instantiate.

### Negative

- The demo's sources are no longer a faithful diff against the 2018 reconstruction: a reader
  comparing them to the binary now also has to see through the object-model changes. The deviations
  are commented where they are not mechanical.
- The `recon` build stays the performance and image baseline, so it has to be kept available to
  re-verify the demo.
- `AutoPtr` destroys through a complete type where `std::shared_ptr` did not, so several headers now
  include a definition where a forward declaration used to do.

### Risks

- The conversion was applied by script over 79 files. The guard against a mis-conversion is the
  verification: every shader blob is byte-identical to the `recon` build's, and the rendered image
  is compared per camera preset against both `recon` and the 2018 original.
- `nvrhi`'s factories return `FRESULT` and the port does not check it at every call site, matching
  what the old handle-returning API gave the demo (a null handle). A failure is therefore reported
  where the handle is used, not where it was created.

## Alternatives considered

**A nested second Donut at `main` under `demos/nv_asteroids/external/`, built exclusively.** No API
work, and the demo's PSNR and timings would carry over by construction. Rejected: it would freeze the
demo against a Donut the tree does not otherwise build, duplicate the framework in the tree, and the
demo would stop being a test of anything the aggregate ships.

**Point the aggregate's Donut submodule at `main`.** One Donut, no API work. Rejected: every other
sample in `src/` and `benchmark/` requires the `ethereal-dev` object model and ShaderTool, so the
tree would no longer build.

**Convert the demo's own classes to nvrhi objects as well.** Rejected: it would add interface tables
to about 30 classes that no donut interface ever sees, and `SceneNode`'s
`std::enable_shared_from_this` graph would have to be rebuilt on `WeakPtr` for no benefit.

For decision 7, the reflection between the 2018 content and Donut's camera could sit in three other
places:

**On the view matrix** (the camera's matrix times an X mirror where it enters `IView`). Exact, and no
consumer needs adjusting, but every pass then works in the 2018 mirrored view space rather than
Donut's, and `IsMirrored()` reports the main view as mirrored. Superseded by the projection, which
keeps view space left-handed.

**In the content** (mirror every world-space input once at load). The only placement with no
reflection anywhere in the renderer, but it reaches the object transforms, camera presets, replay
paths, lights, the star catalogue and the PhysX colliders; it needs the sky and probe cubemaps
face-mirrored, and it flips the world-space cross products in three of the reconstructed shaders
(the asteroid and scene geometry normals, the planet bitangent), which would then have to be edited.
Rejected for its size and its risk to the shaders' 2018 equivalence.

**Nowhere** (render the un-mirrored world with front faces flipped, and compare against flipped 2018
captures). The smallest change, but the demo would no longer show what the original showed.
Rejected.

## References

- Plan: [`../plans/2026-10-04-nv-asteroids-migration.md`](../plans/2026-10-04-nv-asteroids-migration.md)
- `demos/nv_asteroids/README.md` — verification results and the developer options
- `demos/nv_asteroids/CONVENTIONS.md` — the reconstruction conventions, which still apply
- donut `nv_asteroids`: `core/chunk: ChunkFile::deserialize must AddRef the blob it keeps`
- nvrhi ADR 0006 (`QueryInterface` instead of `dynamic_cast`), ADR 0007 (explicit interface tables)
- The aggregate's `docs/conventions/naming.md` (snake_case directories)
