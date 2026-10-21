# GVDB public interface migration inventory (historical)

The migration is complete for the production GVDB targets. The former `GVDB.h`,
legacy device layer, affine scene/volume implementation and handwritten CUDA source
were retired after the portable NVRHI/Slang acceptance gates passed. This file
remains as historical design context; the supported interfaces are the
`GVDBNvrhi*` and `GVDBHostVolume` APIs in this directory.

This is the historical initial integration inventory. The CPU owner, public volume,
transactional VBX and owned voxel/filter APIs have since been implemented and
validated on Windows. Current entry-level retirement gaps are tracked in the
[CUDA entry inventory](../../../../docs/gvdb-cuda-entry-inventory.md), and the
aggregate `docs/slang-next-status.md` records accepted checkpoints. The table
below preserves the earlier migration analysis rather than current completion.

The NVRHI public device and command-list design is retained. The historical
public header exposed backend-specific resource and queue types that could not
become NVRHI objects merely by changing the backend implementation. A second
device abstraction is not needed. The next
integration step is to make the GVDB factory/resource exposure points accept
NVRHI objects directly and route operations to the existing Slang programs.

| Existing GVDB area | Portable implementation now present | Integration still required |
| --- | --- | --- |
| Configuration, CPU nodes/child pools, activation, extents, root, bounds | `buildTopology` and `buildScalarVolume` construct sparse CPU hierarchies without CUDA; `GVDBPortableData` validates/packs them; `GVDBLegacyAdapter` snapshots finalized host pools | Move CPU topology storage out of the CUDA-initializing volume implementation; preserve 64-byte node serialization and existing CPU method semantics |
| `commitPools`, `fetchPools`, `prepareVDB` | NVRHI raw nodes/children plus structured level metadata and offset references | Own these resources in the NVRHI volume implementation; explicit upload/readback and topology barriers replace pointer-bearing VDBInfo |
| Channels and atlas updates | `SlangAtlas` supports R8, RGBA8, R32, RGB32, RGBA32 raw storage, apron, fill, clear, float smoothing, byte and color expansion | Connect dynamic atlas allocation/repacking and all channels to volume ownership; expose raw buffer channel views instead of assuming texture formats |
| Scalar grow/noise/cut/resample/downsample/reduce | `GVDBOperators.slang`, `GVDBAlgorithms.slang`, `SlangVolume` | Production wrappers include resampling, downsampling, cut and interior-only reduction; connect existing IGVDB calls and their placement policy |
| Point keys and unique discovery | `discoverBrickKeys`, stable uint2 radix sort, unique markers and scanned compaction | Allocate/link newly discovered nodes, radius-expanded topology, incremental rebuild bookkeeping |
| Point insertion and scatter | `GVDBParticles.slang` insertion, support insertion, counts, stable position transform | `binPointSubcells` now expands support boxes and scatters positions/colors; velocity payload transport and public API integration remain; atomic insertion order is unspecified |
| Density and level-set gather | `SlangPointFusion` validates disjoint explicit subcells and evaluates CUDA gather formulas | Production subcell binning and byte/ushort/float/software-double position conversion are present; connect resulting density/color channels and velocity gathers |
| Triangle solid voxelization | `voxelizeMesh`: transform, conservative y bins, scan/scatter, triangle-box SAT, oriented parity, R8/R32/S32 dense result | `buildScalarVolume` activates sparse bricks and fills scalar atlas/aprons from CPU dense data; connect mesh result readback/upload or implement direct GPU activation |
| VBX serializer | Standalone `loadRawVBX`/`saveRawVBX` preserves all five formats and ordered channels, transform, original atlas slots, normalized bitmask topology | Connect these CPU snapshots to the NVRHI volume factory; multi-grid/compression/topology reuse remain rejected |
| SPH | `SlangSphSimulation`: binning, scan, density, force, leapfrog and boundaries | Production fluid-pool sample uses this pipeline; scalable neighbor search when cells are smaller than smoothing radius remains |
| Volume and surface raycast | Compute raycast for all four APIs; shared sampling; DXR/Vulkan RT/OptiX procedural library | Tricubic, section, advanced level-set, refraction/reflection, deep volume/mesh continuation and full legacy material behavior |
| FLIP | `transferGridVelocity` implements sparse staggered MAC interpolation, PIC/FLIP blending and midpoint advection | Particle-to-grid transfer, boundary/pressure projection, solver orchestration and full sample integration |

The current scalar and raw atlas helpers borrow `ComputeRuntime`, which owns an
NVRHI device and command list. They do not wrap or create another GPU device.
New factories should take `nvrhi::IDevice*` and the shader artifact location (or
existing VFS shader service). Resource-taking methods should use NVRHI buffers
and explicit typed layouts; kernel-taking methods should use reflected shader
programs/bindings. Compatibility wrappers must not reinterpret old gp pointers
as NVRHI handles or silently fall back to CUDA.

## Observable behavior and validation boundaries

- SIMD/wave intrinsics, native 64-bit shader arithmetic, bindless resource arrays,
  and floating-point atomics are not required by the compute baseline.
- Byte outputs have one writer per 32-bit word. Neighborhood operations use
  separate source/destination buffers, avoiding legacy cross-block write races.
- Point keys retain the serialized ushort ordering z/y/x/level. Invalid,
  nonfinite, or overflowing 16-bit coordinates produce the explicit invalid
  sentinel; they never wrap into another brick.
- Gather retains the legacy max/min distance formulas and color arithmetic.
  Empty subcells return their initialized value instead of dividing by zero.
- Solid voxelization uses the oriented shared-edge rule and conservative SAT.
  CUDA uses separate round-to-nearest multiply/add operations to match graphics
  targets. The original CUDA round-toward-zero instructions are not a portable
  bit-for-bit contract for triangles exactly on numerical decision boundaries.
- Kernel compilation and CPU fixtures are separate from execution tests. The
  root GPU harness is responsible for proving each behavior on actual devices;
  neither this inventory nor a compiled shader claims all legacy samples are
  migrated.
