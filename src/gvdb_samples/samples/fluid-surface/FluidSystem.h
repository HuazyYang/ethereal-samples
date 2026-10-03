#ifndef FLUID_SURFACE_FLUIDSYSTEM_H
#define FLUID_SURFACE_FLUIDSYSTEM_H
// SPH fluid simulator (FLUIDS v.3 by Rama Hoetzlein, as shipped with the
// gFluidSurface sample of GVDB Voxels), GPU path only, on the gp compute
// abstraction: the particle and grid buffers are gp IBuffers, the kernels of
// FluidSystemKernels.cu are compiled to PTX (ptx/FluidSystemKernels.ptx) and
// launched through gp IKernel. One step = insert into the uniform grid,
// prefix sum of the cell counts, counting sort (deep copy into the sorted
// buffers), pressure / density, forces, leap-frog advance with the boundary
// forces of the "wave pool" example.
//
// The first part of this header is shared with the kernels (the .cu defines
// FLUID_KERNEL before including it): buffer ids, the buffer table and the
// parameter block that live in __constant__ memory (fbuf / ftemp / fparam of
// the reference).

// ---------------------------------------------------------------------------
// Shared host / device definitions
// ---------------------------------------------------------------------------

// Particle and grid buffers. FPOS..FGNDX exist twice (fbuf = sorted / current,
// ftemp = copy before the counting sort); FGRID.. once.
enum FluidBuf {
    FPOS = 0,      // float3  particle positions (simulation units = voxels)
    FVEL,          // float3  velocity v(t-1/2)
    FVEVAL,        // float3  velocity estimate at t
    FFORCE,        // float3  force
    FPRESS,        // float   pressure
    FDENSITY,      // float   1 / density
    FCLR,          // uint    RGBA8 colour
    FGCELL,        // uint    grid cell of the particle (GRID_UNDEF outside)
    FGNDX,         // uint    index of the particle inside its cell
    FGRID,         // uint    sorted particle index per grid slot (identity after the full sort)
    FGRIDCNT,      // uint    particles per cell
    FGRIDOFF,      // uint    first slot of each cell (prefix sum of FGRIDCNT)
    FAUXARRAY1,    // uint    prefix sum levels 2 and 3
    FAUXSCAN1,
    FAUXARRAY2,
    FAUXSCAN2,
    FLUID_NUM_BUF
};

#define FLUID_GRID_UNDEF 4294967295u
#define FLUID_SCAN_BLOCKSIZE 512
#define FLUID_THREADS 512

#ifdef FLUID_KERNEL
typedef float3 FluidFloat3;
typedef int3 FluidInt3;
#else
struct FluidFloat3 {
    float x, y, z;
};
struct FluidInt3 {
    int x, y, z;
};
#endif

// Device addresses of the buffers (CUdeviceptr values of the gp buffers).
struct FluidBufs {
    unsigned long long ptr[FLUID_NUM_BUF];
};

// Simulation parameters as the kernels read them (FParams of the reference).
struct FluidParams {
    int numThreads, numBlocks;       // particle launches
    int gridThreads, gridBlocks;     // grid launches
    int szPnts, szGrid;              // allocated elements (rounded up to whole blocks)
    int pnum;

    float pdist, pmass, prest_dens;
    float pextstiff, pintstiff;
    float pradius, psmoothradius, r2, psimscale, pvisc;
    float pforce_min, pforce_max, pforce_freq, pground_slope;
    float pvel_limit, paccel_limit, pdamp;
    FluidFloat3 pboundmin, pboundmax, pgravity;
    float AL, AL2, VL, VL2;

    float d2, rd2, vterm;            // force calculation
    float poly6kern, spikykern, lapkern, gausskern;

    FluidFloat3 gridSize, gridDelta, gridMin, gridMax;
    FluidInt3 gridRes, gridScanMax;
    int gridSrch, gridTotal, gridAdjCnt;
    int gridAdj[64];
};

#ifndef FLUID_KERNEL
// ---------------------------------------------------------------------------
// Host side
// ---------------------------------------------------------------------------
#include <gvdb/GPDevice.h>
#include <nvrhi/core/autoptr.h>
#include <donut/core/math/math.h>
#include <donut/core/vfs/VFS.h>
#include <vector>

class FluidSystem {
 public:
    FluidSystem(donut::gp::IDevice *device, donut::gp::IDeviceQueue *queue);
    ~FluidSystem();

    // Loads ptx/FluidSystemKernels.ptx from the file system and resolves the kernels.
    bool initialize(donut::vfs::IFileSystem *vfs);
    // (Re)starts the simulation with `numParticles` particles: the parameters of
    // the reference (wave-pool example), grid, buffers, initial particle block.
    bool start(int numParticles);
    // One simulation step on the GPU.
    void run();

    int getNumPoints() const { return m_numPoints; }
    donut::gp::IBuffer *getBuffer(FluidBuf id) const { return m_buf[id].Get(); }
    const FluidParams &getParams() const { return m_params; }
    dm::float3 getGridMin() const { return m_gridMin; }
    dm::float3 getGridMax() const { return m_gridMax; }
    float getTime() const { return m_time; }
    float getDT() const { return m_dt; }
    int getFrame() const { return m_frame; }

    // Positions, velocities and colours back to the host (TransferFromCUDA of
    // the reference); waits for the queue.
    void readback(std::vector<dm::float3> &pos, std::vector<dm::float3> &vel, std::vector<uint32_t> &clr);

 private:
    // Scalar / vector parameters of the reference (m_Param / m_Vec, by name).
    struct Params {
        float simScale, visc, restDensity, spacing, mass, radius, dist, smoothRadius;
        float intStiff, extStiff, extDamp, accelLimit, velLimit, grav;
        float groundSlope, forceMin, forceMax, forceFreq;
        float gridSize, gridDensity;
        dm::float3 volMin, volMax, initMin, initMax, boundMin, boundMax, planeGravDir;
    };

    void setupDefaultParams();
    void setupExampleParams();
    void setupSpacing();
    void setupGrid(dm::float3 vmin, dm::float3 vmax, float simScale, float cellSize);
    void setupFluidParams();       // FluidSetupCUDA + FluidParamCUDA
    bool allocateBuffers();        // AllocateParticles + AllocateGrid
    void setupAddVolume(dm::float3 vmin, dm::float3 vmax, float spacing, float offs, int total);
    bool uploadConstants();        // fbuf / ftemp / fparam

    void insertParticles();
    void prefixSumCells();
    void countingSortFull();
    void computePressure();
    void computeForce();
    void advance();

    bool createBuffer(nvrhi::AutoPtr<donut::gp::IBuffer> &buffer, size_t bytes);
    void launch(donut::gp::IKernel *kernel, int blocks, int threads, const donut::gp::KernelArg *args, size_t argc);

    nvrhi::AutoPtr<donut::gp::IDevice> m_device;
    nvrhi::AutoPtr<donut::gp::IDeviceQueue> m_queue;
    nvrhi::AutoPtr<donut::gp::IModule> m_module;
    nvrhi::AutoPtr<donut::gp::IKernel> m_kInsert, m_kCountingSort, m_kPressure, m_kForce, m_kAdvance, m_kPrefixSum,
        m_kPrefixFixup;

    nvrhi::AutoPtr<donut::gp::IBuffer> m_buf[FLUID_NUM_BUF];       // fbuf
    nvrhi::AutoPtr<donut::gp::IBuffer> m_temp[FLUID_NUM_BUF];      // ftemp (FPOS..FGNDX)
    nvrhi::AutoPtr<donut::gp::IBuffer> m_stagingPos, m_stagingVel, m_stagingClr;

    Params m_p = {};
    FluidParams m_params = {};
    int m_numPoints = 0;
    int m_maxPoints = 0;
    float m_time = 0.f;
    float m_dt = 0.003f;
    int m_frame = 0;

    // uniform grid
    int m_gridTotal = 0;
    dm::int3 m_gridRes = dm::int3::zero();
    dm::float3 m_gridMin = dm::float3::zero(), m_gridMax = dm::float3::zero();
    dm::float3 m_gridSize = dm::float3::zero(), m_gridDelta = dm::float3::zero();
    int m_gridSrch = 0;
    int m_gridAdjCnt = 0;

    // host copies of the initial particle block
    std::vector<dm::float3> m_hostPos;
    std::vector<uint32_t> m_hostClr;
};

#endif /* !FLUID_KERNEL */

#endif /* FLUID_SURFACE_FLUIDSYSTEM_H */
