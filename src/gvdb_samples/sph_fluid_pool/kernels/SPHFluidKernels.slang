//-----------------------------------------------------------------------------
// FLUIDS v5.0 - SPH Fluid Simulator for CPU and GPU
// Copyright (C) 2012-2013, 2021. Rama Hoetzlein, http://fluids3.com
//-----------------------------------------------------------------------------
// Kernels of the SPH fluid (particles.cu of the reference), launched by SPHSolver.cpp; loaded as
// shaders/gvdb/kernels/SPHFluidKernels.nvsc.
//
// The reference reached the particle and grid buffers through tables of device pointers in __constant__ memory
// (FPnts = current / sorted particles, FPntTmp = the copy the counting sort reads, FAccel = acceleration grid) and
// the parameters through FParams. Here they are kernel arguments, passed by value (the same constant bank), with the
// layouts of SPHBufferTable and SPHFluidParams (SPHFluidParams.h).
//
// The logic is the reference's. Not ported: the unused device helpers (contributePressure, contributeForce,
// getGridCell, debugAccess). `register` and __mul24 are dropped (plain 32-bit multiply), and so is the
// __syncthreads() of computePressure (after threads may have returned; there is no shared memory to synchronise).

static const int SPH_GRID_UNDEF = 2147483647;   // particle outside the acceleration grid (max int)
#define SPH_SCAN_BLOCKSIZE 512                  // threads per block of the prefix-sum kernels
#define SPH_THREADS 512                         // threads per block of the particle kernels
#define SPH_MAX_GRID_ADJ 216
static const float EPSILON = 0.00001f;

// The particle slots of an SPHBufferTable (FPnts, FPntTmp: SPH_FPOS .. SPH_FGNDX).
struct SPHParticleBuffers
{
    float3*     pos;            // SPH_FPOS     position, world units
    float3*     vel;            // SPH_FVEL     velocity (leapfrog half step)
    uint*       clr;            // SPH_FCLR     packed RGBA8 colour
    float3*     veval;          // SPH_FVEVAL   velocity used to evaluate forces
    float3*     force;          // SPH_FFORCE
    float*      press;          // SPH_FPRESS   density (the reference calls it pressure)
    int*        gcell;          // SPH_FGCELL   grid cell, SPH_GRID_UNDEF when outside
    int*        gndx;           // SPH_FGNDX    index inside the grid cell
    uint64_t    unused[8];
};

// The grid slots of an SPHBufferTable (FAccel: SPH_AGRID .. SPH_AAUXSCAN2).
struct SPHGridBuffers
{
    int*        grid;           // SPH_AGRID    per particle: sorted particle index
    int*        gridcnt;        // SPH_AGRIDCNT per cell: particle count
    int*        gridoff;        // SPH_AGRIDOFF per cell: first particle
    uint64_t    unused[13];     // the prefix sum scratch (SPHSolver passes it to the scan kernels)
};

// Simulation parameters (FParams_t of the reference; SPHFluidParams.h).
struct SPHFluidParams
{
    int example;
    float time, dt, sim_scale;

    int numThreads, numBlocks;
    int pnum;

    float pdist, pmass, prest_dens, pintstiff;
    float pradius, psmoothradius, pspacing, r2, pvisc;

    float AL, AL2, VL, VL2;
    float d2, rd2, vterm;
    float poly6kern, spikykern, lapkern, gausskern;

    float grid_size, grid_density;
    float3 gridSize, gridDelta, gridMin, gridMax;
    int3 gridRes, gridScanMax;
    int gridSrch, gridTotal, gridAdjCnt;
    int gridAdj[SPH_MAX_GRID_ADJ];

    float bound_slope, bound_stiff, bound_friction, bound_damp;
    float bound_wall_force, bound_wall_freq;
    float3 bound_min, bound_max;
    float3 init_min, init_max;

    float3 grav_dir, grav_pos, gravity;
    float grav_amt;
};

[shader("compute")]
[numthreads(SPH_THREADS, 1, 1)]
void insertParticles(uniform SPHFluidParams FParams, uniform SPHParticleBuffers FPnts, uniform SPHGridBuffers FAccel,
                     uniform int pnum, uint3 tid : SV_DispatchThreadID)
{
    const uint i = tid.x;   // particle index
    if (i >= pnum) return;

    const float3 p = FPnts.pos[i];
    const float3 gcf = (p - FParams.gridMin) * FParams.gridDelta;
    const int3 gc = int3(int(gcf.x), int(gcf.y), int(gcf.z));
    const int gs = (gc.y * FParams.gridRes.z + gc.z) * FParams.gridRes.x + gc.x;

    if (gc.x >= 1 && gc.x <= FParams.gridScanMax.x && gc.y >= 1 && gc.y <= FParams.gridScanMax.y && gc.z >= 1 &&
        gc.z <= FParams.gridScanMax.z) {
        FPnts.gcell[i] = gs;   // grid cell insert
        int index;
        InterlockedAdd(FAccel.gridcnt[gs], 1, index);   // grid counts
        FPnts.gndx[i] = index;
    } else {
        FPnts.gcell[i] = SPH_GRID_UNDEF;
    }
}

[shader("compute")]
[numthreads(SPH_THREADS, 1, 1)]
void countingSortFull(uniform SPHFluidParams FParams, uniform SPHParticleBuffers FPnts,
                      uniform SPHParticleBuffers FPntTmp, uniform SPHGridBuffers FAccel, uniform int pnum,
                      uint3 tid : SV_DispatchThreadID)
{
    const uint i = tid.x;   // particle index
    if (i >= pnum) return;

    // This algorithm is O(2NK) in space, O(N/P) time, where K = sizeof(Fluid).
    // Copy the particle from the original, unsorted buffer (FPntTmp) into its
    // sorted memory location (FPnts).
    // **NOTE** Shared memory cannot be used for temporary storage since this is
    // a global reordering and there is no synchronization across blocks.

    const int icell = FPntTmp.gcell[i];

    if (icell != SPH_GRID_UNDEF) {
        // Determine the sort_ndx; location of the particle after the sort
        const int indx = FPntTmp.gndx[i];
        const int sort_ndx = FAccel.gridoff[icell] + indx;   // global_ndx = grid_cell_offset + particle_offset

        // Transfer data to the sort location
        FPnts.pos[sort_ndx] = FPntTmp.pos[i];
        FPnts.vel[sort_ndx] = FPntTmp.vel[i];
        FPnts.clr[sort_ndx] = FPntTmp.clr[i];
        FPnts.veval[sort_ndx] = FPntTmp.veval[i];
        FPnts.press[sort_ndx] = FPntTmp.press[i];
        FPnts.force[sort_ndx] = FPntTmp.force[i];
        FPnts.gcell[sort_ndx] = icell;
        FPnts.gndx[sort_ndx] = indx;

        FAccel.grid[sort_ndx] = sort_ndx;   // full sort, grid indexing becomes identity
    }
}

[shader("compute")]
[numthreads(SPH_THREADS, 1, 1)]
void computePressure(uniform SPHFluidParams FParams, uniform SPHParticleBuffers FPnts, uniform SPHGridBuffers FAccel,
                     uniform int pnum, uint3 tid : SV_DispatchThreadID)
{
    const uint i = tid.x;   // particle index
    if (i >= pnum) return;

    // Get search cell
    uint gc = FPnts.gcell[i];
    if (gc == SPH_GRID_UNDEF) return;   // particle out-of-range
    gc -= (1 * FParams.gridRes.z + 1) * FParams.gridRes.x + 1;

    // Sum Pressures
    const float3 pos = FPnts.pos[i];
    float sum = 0.0;
    for (int c = 0; c < FParams.gridAdjCnt; c++) {
        const int cell = gc + FParams.gridAdj[c];
        const int clast = FAccel.gridoff[cell] + FAccel.gridcnt[cell];
        for (int cndx = FAccel.gridoff[cell]; cndx < clast; cndx++) {
            const float3 dist = pos - FPnts.pos[FAccel.grid[cndx]];
            float dsq = (dist.x * dist.x + dist.y * dist.y + dist.z * dist.z);
            if (dsq < FParams.rd2 && dsq > 0.0) {
                dsq = (FParams.rd2 - dsq) * FParams.d2;
                sum += dsq * dsq * dsq;
            }
        }
    }

    // Compute Density & Pressure
    sum = sum * FParams.pmass * FParams.poly6kern;
    if (sum == 0.0) sum = 1.0;
    FPnts.press[i] = sum;
}

[shader("compute")]
[numthreads(SPH_THREADS, 1, 1)]
void computeForce(uniform SPHFluidParams FParams, uniform SPHParticleBuffers FPnts, uniform SPHGridBuffers FAccel,
                  uniform int pnum, uint3 tid : SV_DispatchThreadID)
{
    const uint i = tid.x;   // particle index
    if (i >= pnum) return;

    // Get search cell
    uint gc = FPnts.gcell[i];
    if (gc == SPH_GRID_UNDEF) return;   // particle out-of-range
    gc -= (1 * FParams.gridRes.z + 1) * FParams.gridRes.x + 1;

    // Sum Pressures
    float3 force = float3(0, 0, 0);
    for (int c = 0; c < FParams.gridAdjCnt; c++) {
        const int cell = gc + FParams.gridAdj[c];
        for (int cndx = FAccel.gridoff[cell]; cndx < FAccel.gridoff[cell] + FAccel.gridcnt[cell]; cndx++) {
            const int j = FAccel.grid[cndx];
            const float3 dist = (FPnts.pos[i] - FPnts.pos[j]);   // dist in cm
            float dsq = (dist.x * dist.x + dist.y * dist.y + dist.z * dist.z);
            if (dsq < FParams.rd2 && dsq > 0) {
                dsq = sqrt(dsq * FParams.d2);
                const float pi = (FPnts.press[i] - FParams.prest_dens) * FParams.pintstiff;
                const float pj = (FPnts.press[j] - FParams.prest_dens) * FParams.pintstiff;
                const float pterm = FParams.sim_scale * -0.5f * (FParams.psmoothradius - dsq) * FParams.spikykern * (pi + pj) / dsq;
                force += (pterm * dist + FParams.vterm * (FPnts.veval[j] - FPnts.veval[i])) *
                         (FParams.psmoothradius - dsq) / (FPnts.press[i] * FPnts.press[j]);
            }
        }
    }
    FPnts.force[i] = force;
}

[shader("compute")]
[numthreads(SPH_THREADS, 1, 1)]
void advanceParticles(uniform SPHFluidParams FParams, uniform SPHParticleBuffers FPnts, uniform float time,
                      uniform float dt, uniform float ss, uniform int numPnts, uint3 tid : SV_DispatchThreadID)
{
    const uint i = tid.x;   // particle index
    if (i >= numPnts) return;

    if (FPnts.gcell[i] == SPH_GRID_UNDEF) {
        FPnts.pos[i] = float3(-1000, -1000, -1000);
        FPnts.vel[i] = float3(0, 0, 0);
        return;
    }

    // Get particle vars
    float3 norm;
    const float3 pos = FPnts.pos[i];
    float3 vel = FPnts.vel[i];
    const float3 veval = FPnts.veval[i];
    float diff, adj, speed;

    // Leapfrog integration
    float3 accel = FPnts.force[i] * FParams.pmass;

    // Boundaries
    // Y-axis
    diff = FParams.pradius - (pos.y - (FParams.bound_min.y + (pos.x - FParams.bound_min.x) * FParams.bound_slope)) * ss;
    if (diff > EPSILON) {
        norm = float3(-FParams.bound_slope, 1.0 - FParams.bound_slope, 0);
        adj = FParams.bound_stiff * diff - FParams.bound_damp * dot(norm, veval);
        norm *= adj;
        accel += norm - veval * FParams.bound_friction;
    }

    diff = FParams.pradius - (FParams.bound_max.y - pos.y) * ss;
    if (diff > EPSILON) {
        norm = float3(0, -1, 0);
        adj = FParams.bound_stiff * diff - FParams.bound_damp * dot(norm, veval);
        norm *= adj;
        accel += norm - veval * FParams.bound_friction;
    }

    // X-axis
    const float wall = (sin(time * FParams.bound_wall_freq + pos.z / 400.f) * 0.5 + 0.5) * FParams.bound_wall_force;
    diff = FParams.pradius - (pos.x - (FParams.bound_min.x + wall)) * ss;
    if (diff > EPSILON) {
        norm = float3(1, 0, 0);
        adj = wall * FParams.bound_stiff * diff - FParams.bound_damp * dot(norm, veval);
        norm *= adj;
        accel += norm;
    }
    diff = FParams.pradius - (FParams.bound_max.x - pos.x) * ss;
    if (diff > EPSILON) {
        norm = float3(-1, 0, 0);
        adj = FParams.bound_stiff * diff - FParams.bound_damp * dot(norm, veval);
        norm *= adj;
        accel += norm;
    }

    // Z-axis
    diff = FParams.pradius - (pos.z - FParams.bound_min.z) * ss;
    if (diff > EPSILON) {
        norm = float3(0, 0, 1);
        adj = FParams.bound_stiff * diff - FParams.bound_damp * dot(norm, veval);
        norm *= adj;
        accel += norm;
    }
    diff = FParams.pradius - (FParams.bound_max.z - pos.z) * ss;
    if (diff > EPSILON) {
        norm = float3(0, 0, -1);
        adj = FParams.bound_stiff * diff - FParams.bound_damp * dot(norm, veval);
        norm *= adj;
        accel += norm;
    }

    // Gravity
    accel += FParams.gravity;

    // Accel Limit
    speed = accel.x * accel.x + accel.y * accel.y + accel.z * accel.z;
    if (speed > FParams.AL2) {
        accel *= FParams.AL / sqrt(speed);
    }
    // Velocity Limit
    speed = vel.x * vel.x + vel.y * vel.y + vel.z * vel.z;
    if (speed > FParams.VL2) {
        vel *= FParams.VL / sqrt(speed);
    }

    // Leap-frog Integration
    const float3 vnext = accel * dt + vel;          // v(t+1/2) = v(t-1/2) + a(t) dt
    FPnts.veval[i] = (vel + vnext) * 0.5;           // v(t+1) = [v(t-1/2) + v(t+1/2)] * 0.5
    FPnts.vel[i] = vnext;
    FPnts.pos[i] += vnext * (dt / ss);              // p(t+1) = p(t) + v(t+1/2) dt
}

[shader("compute")]
[numthreads(SPH_SCAN_BLOCKSIZE, 1, 1)]
void prefixFixup(uniform uint* input, uniform uint* aux, uniform int len, uint3 thread : SV_GroupThreadID,
                 uint3 group : SV_GroupID)
{
    const uint t = thread.x;
    const uint start = t + 2 * group.x * SPH_SCAN_BLOCKSIZE;
    if (start < len) input[start] += aux[group.x];
    if (start + SPH_SCAN_BLOCKSIZE < len) input[start + SPH_SCAN_BLOCKSIZE] += aux[group.x];
}

groupshared uint scan_array[SPH_SCAN_BLOCKSIZE << 1];

[shader("compute")]
[numthreads(SPH_SCAN_BLOCKSIZE, 1, 1)]
void prefixSum(uniform uint* input, uniform uint* output, uniform uint* aux, uniform int len, uniform int zeroff,
               uint3 thread : SV_GroupThreadID, uint3 group : SV_GroupID)
{
    const uint t1 = thread.x + 2 * group.x * SPH_SCAN_BLOCKSIZE;
    const uint t2 = t1 + SPH_SCAN_BLOCKSIZE;

    // Pre-load into shared memory
    scan_array[thread.x] = (t1 < len) ? input[t1] : 0;
    scan_array[thread.x + SPH_SCAN_BLOCKSIZE] = (t2 < len) ? input[t2] : 0;
    GroupMemoryBarrierWithGroupSync();

    // Reduction
    int stride;
    for (stride = 1; stride <= SPH_SCAN_BLOCKSIZE; stride <<= 1) {
        const int index = (thread.x + 1) * stride * 2 - 1;
        if (index < 2 * SPH_SCAN_BLOCKSIZE) scan_array[index] += scan_array[index - stride];
        GroupMemoryBarrierWithGroupSync();
    }

    // Post reduction
    for (stride = SPH_SCAN_BLOCKSIZE >> 1; stride > 0; stride >>= 1) {
        const int index = (thread.x + 1) * stride * 2 - 1;
        if (index + stride < 2 * SPH_SCAN_BLOCKSIZE) scan_array[index + stride] += scan_array[index];
        GroupMemoryBarrierWithGroupSync();
    }
    GroupMemoryBarrierWithGroupSync();

    // Output values & aux
    if (t1 + zeroff < len) output[t1 + zeroff] = scan_array[thread.x];
    if (t2 + zeroff < len)
        output[t2 + zeroff] =
            (thread.x == SPH_SCAN_BLOCKSIZE - 1 && zeroff != 0) ? 0 : scan_array[thread.x + SPH_SCAN_BLOCKSIZE];
    if (thread.x == 0) {
        if (zeroff != 0) output[0] = 0;
        if (aux != nullptr) aux[group.x] = scan_array[2 * SPH_SCAN_BLOCKSIZE - 1];
    }
}
