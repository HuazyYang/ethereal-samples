#ifndef SPH_FLUID_H
#define SPH_FLUID_H
// SPH fluid: port of fluids5.0 (Rama Hoetzlein, http://fluids3.com) to the
// nvrhi CUDA device and Slang kernels, following the conventions of the GVDB
// port (a gvdb::IComputeContext records the work).
//
// The reference kept everything in one class (Particles). Here the data
// structure and the algorithm are separate COM objects:
//
//   ISPHParticles   the data: particle buffers in structure-of-arrays form,
//                   the uniform acceleration grid, the simulation parameters.
//                   Host-side authoring (fill a volume with particles) is part
//                   of it because it is structure maintenance.
//                   (Particles' m_Points / m_PointsTemp / m_Accel / m_Params,
//                   ReallocateParticles, RebuildAccelGrid, AddPointsInVolume.)
//   ISPHSolver      the operations: the six stages of one simulation step
//                   and the kernel module that implements them.
//                   (Particles::Run, InsertParticles ... Advance.)
//
// A solver is created for one particle set and uses only its public
// interface, so other operations (surfacing, sampling, emitters) can be added
// as further objects without touching the data structure.
//
// Scene integration is in SPHFluidScene.h: SPHFluidInstance is the scene-graph
// leaf that places a particle set in the world, one scene may hold several.
#include <nvrhi/nvrhi.h>
#include <nvrhi/interop.h>
#include <gvdb/GVDBCompute.h>
#include "SPHFluidParams.h"

namespace sph {

// Shader container of the fluid kernels (kernels/SPHFluidKernels.slang), served by the compute context's file system.
#define SPH_KERNELS "shaders/gvdb/kernels/SPHFluidKernels.nvsc"

enum class ParticleBuffer : uint8_t {
    Position = SPH_FPOS,
    Velocity = SPH_FVEL,
    Color = SPH_FCLR,
    VelocityEval = SPH_FVEVAL,
    Force = SPH_FFORCE,
    Density = SPH_FPRESS,
    GridCell = SPH_FGCELL,
    GridIndex = SPH_FGNDX,
    Count = SPH_PARTICLE_BUFFERS
};

enum class GridBuffer : uint8_t {
    Grid = SPH_AGRID,
    GridCount = SPH_AGRIDCNT,
    GridOffset = SPH_AGRIDOFF,
    AuxArray1 = SPH_AAUXARRAY1,
    AuxScan1 = SPH_AAUXSCAN1,
    AuxArray2 = SPH_AAUXARRAY2,
    AuxScan2 = SPH_AAUXSCAN2,
    Count = SPH_GRID_BUFFERS
};

// Bytes per element of a particle buffer (12 for the float3 slots, 4 otherwise).
uint32_t particleBufferStride(ParticleBuffer buffer);

// Scene presets of the reference (SetupExampleParams).
enum class SPHExample : int {
    Regression = 0,   // N x N x N static grid, no gravity
    Tower = 1,
    WavePool = 2,     // "large beach front": sloped floor and a wave-making wall
    DamBreak = 3,
};

// SetupDefaultParams / SetupExampleParams of the reference. setupExampleParams
// expects defaults already set and overrides the domain, the fill volume and
// the wall forces.
void setupDefaultParams(SPHFluidParams &params);
void setupExampleParams(SPHFluidParams &params, SPHExample example);

struct SPHParticlesDesc {
    uint32_t maxParticles = 0;
    // When set, the Position, Color and Velocity buffers are vertex buffers of
    // the graphics device imported into the CUDA device (the reference's
    // CUDA / OpenGL interop buffers), so a rasterizer draws them without a
    // copy; configure() fails when the import is not possible (D3D12 and
    // Vulkan only). Without an interop device every buffer is a plain buffer of
    // the CUDA device and there are no render buffers.
    nvrhi::IInteropDevice *interop = nullptr;
};

// ---------------------------------------------------------------------------
// The data structure.
// ---------------------------------------------------------------------------
NVRHI_IID(ISPHParticles, "5aae500e-8cc1-44f7-85dc-eb610de95b80")
struct ISPHParticles : public nvrhi::IObject {
    NVRHI_DECLARE_UUID_TRAITS(ISPHParticles)

    // ---- configuration ----
    // (Re)allocates the particle buffers and the sort scratch copy for
    // desc.maxParticles and resets the particle count to zero. The parameters
    // are kept. (ReallocateParticles)
    virtual nvrhi::FRESULT configure(const SPHParticlesDesc &desc) = 0;
    virtual const SPHParticlesDesc &getDesc() const = 0;

    // ---- parameters ----
    virtual const SPHFluidParams &getParams() const = 0;
    // Replaces the parameters. Derived values (kernel constants, limits,
    // gravity; UpdateParams of the reference) are recomputed; the grid is not
    // rebuilt, call rebuildAccelGrid() after changing the domain or the
    // smoothing radius.
    virtual void setParams(const SPHFluidParams &params) = 0;
    // Simulation time, advanced by the solver.
    virtual void setTime(float time) = 0;
    // Incremented whenever the parameters change; operation objects compare it
    // to know when to upload the parameter block again.
    virtual uint64_t getParamsRevision() const = 0;

    // ---- acceleration grid ----
    // Derives the grid from the domain bounds and the smoothing radius and
    // allocates its buffers (RebuildAccelGrid). Needs configure() first: the
    // sorted index buffer has one entry per particle.
    virtual nvrhi::FRESULT rebuildAccelGrid() = 0;

    // ---- particles ----
    virtual uint32_t getNumParticles() const = 0;
    virtual uint32_t getMaxParticles() const = 0;
    // Removes all particles (host side; commit() makes it effective on the GPU).
    virtual void clear() = 0;
    // Fills the box with particles at rest spacing, bottom up, with the
    // reference's position jitter and colour gradient, until the capacity is
    // reached. Returns the number added. Updates pdist / pspacing / prest_dens
    // in the parameters as the reference does. (AddPointsInVolume)
    virtual uint32_t addPointsInVolume(dm::float3 boxMin, dm::float3 boxMax) = 0;
    // Uploads the host particle arrays to the device buffers (CommitAll).
    virtual nvrhi::FRESULT commit() = 0;
    // Reads `count` elements of one device buffer back, starting at `first`
    // (Retrieve; debugging and tests). `dst` holds count * particleBufferStride() bytes.
    virtual nvrhi::FRESULT retrieve(ParticleBuffer buffer, uint32_t first, uint32_t count, void *dst) = 0;

    // ---- device buffers (CUDA device) ----
    virtual nvrhi::IBuffer *getBuffer(ParticleBuffer buffer) = 0;
    // The scratch copy the counting sort reads from (m_PointsTemp).
    virtual nvrhi::IBuffer *getTempBuffer(ParticleBuffer buffer) = 0;
    virtual nvrhi::IBuffer *getGridBuffer(GridBuffer buffer) = 0;
    // The graphics device's vertex buffer behind a shared slot (Position /
    // Color / Velocity); nullptr when the particle set was configured without
    // interop or the slot is not shared. The rasterizer and the kernels share
    // its memory: the caller orders their work (GVDBApp's frame does).
    virtual nvrhi::IBuffer *getRenderBuffer(ParticleBuffer buffer) = 0;
    // Incremented whenever device buffers are reallocated; operation objects
    // compare it to know when to rebuild their pointer tables.
    virtual uint64_t getLayoutRevision() const = 0;

    // Domain bounds in world units (bound_min / bound_max).
    virtual dm::box3 getBounds() const = 0;

    // The context the particle set records its uploads into; its solvers use it too.
    virtual gvdb::IComputeContext *getContext() = 0;
};

// ---------------------------------------------------------------------------
// The operations.
// ---------------------------------------------------------------------------
NVRHI_IID(ISPHSolver, "e1b547eb-6f0a-4c76-8962-d6b3306b2835")
struct ISPHSolver : public nvrhi::IObject {
    NVRHI_DECLARE_UUID_TRAITS(ISPHSolver)

    virtual ISPHParticles *getParticles() = 0;

    // The stages of one step, in the order step() runs them. Each records its
    // kernels into the particle set's context and returns without waiting.
    virtual void insertParticles() = 0;      // particles -> grid cells, per-cell counts
    virtual void prefixScanParticles() = 0;  // per-cell counts -> per-cell offsets
    virtual void countingSort() = 0;         // reorder every particle buffer by grid cell
    virtual void computePressure() = 0;      // density at each particle
    virtual void computeForce() = 0;         // pressure and viscosity forces
    virtual void advance() = 0;              // boundaries, gravity, leapfrog integration

    // One simulation step: the six stages, then time += dt (Particles::Run).
    // Does not wait for the GPU; the caller synchronises (or lets the
    // presentation path do it) before reading results on the host.
    virtual void step() = 0;

    virtual float getTime() const = 0;
    virtual uint32_t getFrame() const = 0;
    // Back to time 0 / frame 0 (after the particle set was refilled).
    virtual void reset() = 0;
};

// Creates an empty particle set on a compute context.
nvrhi::FRESULT createSPHParticles(gvdb::IComputeContext *context, ISPHParticles **outParticles);
// Creates the solver of a particle set; its kernels are SPH_KERNELS of the particle set's context.
nvrhi::FRESULT createSPHSolver(ISPHParticles *particles, ISPHSolver **outSolver);

}  // namespace sph

#endif /* SPH_FLUID_H */
