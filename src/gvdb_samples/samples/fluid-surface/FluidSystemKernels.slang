//-----------------------------------------------------------------------------
// FLUIDS v.3 - SPH Fluid Simulator for CPU and GPU
// Copyright (C) 2012-2013. Rama Hoetzlein, http://fluids3.com
//
// NVIDIA(R) GVDB VOXELS
// Copyright 2017 NVIDIA Corporation
// SPDX-License-Identifier: Apache-2.0
//-----------------------------------------------------------------------------
// GPU path of the fluid system (fluid_system_cuda.cu of the reference): the kernels launched by FluidSystem.cpp,
// loaded as shaders/gvdb/FluidSystemKernels.nvsc. The curand based randomInit / emitParticles, sampleParticles
// and computeQuery of the reference are not part of its simulation step and are not ported.
//
// The reference read the simulation parameters and the buffer tables from __constant__ memory (fparam, fbuf =
// current / sorted, ftemp = copy before the counting sort); here they are kernel arguments, passed by value (the
// same constant bank). Same layouts as FluidParams / FluidBufs in FluidSystem.h.

static const uint FLUID_GRID_UNDEF = 4294967295u;
#define FLUID_SCAN_BLOCKSIZE 512
#define FLUID_THREADS 512
static const float EPSILON = 0.00001f;

// The particle and grid buffers (FluidBuf of FluidSystem.h, in that order).
struct FluidBufs
{
    float3*     pos;            // FPOS     particle positions (simulation units = voxels)
    float3*     vel;            // FVEL     velocity v(t-1/2)
    float3*     veval;          // FVEVAL   velocity estimate at t
    float3*     force;          // FFORCE
    float*      press;          // FPRESS   pressure
    float*      density;        // FDENSITY 1 / density
    uint*       clr;            // FCLR     RGBA8 colour
    uint*       gcell;          // FGCELL   grid cell of the particle (FLUID_GRID_UNDEF outside)
    uint*       gndx;           // FGNDX    index of the particle inside its cell
    uint*       grid;           // FGRID    sorted particle index per grid slot
    uint*       gridcnt;        // FGRIDCNT particles per cell
    uint*       gridoff;        // FGRIDOFF first slot of each cell
    uint*       auxarray1;      // FAUXARRAY1 .. FAUXSCAN2: prefix sum levels 2 and 3 (FluidSystem.cpp passes them)
    uint*       auxscan1;
    uint*       auxarray2;
    uint*       auxscan2;
};

// Simulation parameters (FParams of the reference).
struct FluidParams
{
    int         numThreads;
    int         numBlocks;      // particle launches
    int         gridThreads;
    int         gridBlocks;     // grid launches
    int         szPnts;
    int         szGrid;         // allocated elements (rounded up to whole blocks)
    int         pnum;

    float       pdist;
    float       pmass;
    float       prest_dens;
    float       pextstiff;
    float       pintstiff;
    float       pradius;
    float       psmoothradius;
    float       r2;
    float       psimscale;
    float       pvisc;
    float       pforce_min;
    float       pforce_max;
    float       pforce_freq;
    float       pground_slope;
    float       pvel_limit;
    float       paccel_limit;
    float       pdamp;
    float3      pboundmin;
    float3      pboundmax;
    float3      pgravity;
    float       AL;
    float       AL2;
    float       VL;
    float       VL2;

    float       d2;             // force calculation
    float       rd2;
    float       vterm;
    float       poly6kern;
    float       spikykern;
    float       lapkern;
    float       gausskern;

    float3      gridSize;
    float3      gridDelta;
    float3      gridMin;
    float3      gridMax;
    int3        gridRes;
    int3        gridScanMax;
    int         gridSrch;
    int         gridTotal;
    int         gridAdjCnt;
    int         gridAdj[64];
};

[shader("compute")]
[numthreads(FLUID_THREADS, 1, 1)]
void insertParticles(uniform FluidParams fparam, uniform FluidBufs fbuf, uniform int pnum,
                     uint3 tid : SV_DispatchThreadID)
{
    const uint i = tid.x;   // particle index
    if (i >= pnum) return;

    const float3 gridMin = fparam.gridMin;
    const float3 gridDelta = fparam.gridDelta;
    const int3 gridRes = fparam.gridRes;
    const int3 gridScan = fparam.gridScanMax;

    const float3 gcf = (fbuf.pos[i] - gridMin) * gridDelta;
    const int3 gc = int3(int(gcf.x), int(gcf.y), int(gcf.z));
    const int gs = (gc.y * gridRes.z + gc.z) * gridRes.x + gc.x;

    if (gc.x >= 1 && gc.x <= gridScan.x && gc.y >= 1 && gc.y <= gridScan.y && gc.z >= 1 && gc.z <= gridScan.z) {
        fbuf.gcell[i] = gs;   // grid cell insert
        uint index;
        InterlockedAdd(fbuf.gridcnt[gs], 1, index);   // grid counts
        fbuf.gndx[i] = index;
    } else {
        fbuf.gcell[i] = FLUID_GRID_UNDEF;
    }
}

// Counting sort - full (deep copy from ftemp into the sorted slots of fbuf)
[shader("compute")]
[numthreads(FLUID_THREADS, 1, 1)]
void countingSortFull(uniform FluidParams fparam, uniform FluidBufs fbuf, uniform FluidBufs ftemp, uniform int pnum,
                      uint3 tid : SV_DispatchThreadID)
{
    const uint i = tid.x;   // particle index
    if (i >= pnum) return;

    const uint icell = ftemp.gcell[i];
    if (icell != FLUID_GRID_UNDEF) {
        // location of the particle after the sort: cell offset + index inside the cell
        const uint indx = ftemp.gndx[i];
        const int sort_ndx = fbuf.gridoff[icell] + indx;

        fbuf.grid[sort_ndx] = sort_ndx;   // full sort: grid indexing becomes identity
        fbuf.pos[sort_ndx] = ftemp.pos[i];
        fbuf.vel[sort_ndx] = ftemp.vel[i];
        fbuf.veval[sort_ndx] = ftemp.veval[i];
        fbuf.force[sort_ndx] = ftemp.force[i];
        fbuf.press[sort_ndx] = ftemp.press[i];
        fbuf.density[sort_ndx] = ftemp.density[i];
        fbuf.clr[sort_ndx] = ftemp.clr[i];
        fbuf.gcell[sort_ndx] = icell;
        fbuf.gndx[sort_ndx] = indx;
    }
}

// (Inlined by Slang: a call would copy the parameter blocks.)
[ForceInline]
float contributePressure(FluidParams fparam, FluidBufs fbuf, int i, float3 p, int cell)
{
    if (fbuf.gridcnt[cell] == 0) return 0.0f;

    float sum = 0.0f;
    const float d2 = fparam.psimscale * fparam.psimscale;
    const float r2 = fparam.r2 / d2;

    const int clast = fbuf.gridoff[cell] + fbuf.gridcnt[cell];
    for (int cndx = fbuf.gridoff[cell]; cndx < clast; cndx++) {
        const int pndx = fbuf.grid[cndx];
        const float3 dist = p - fbuf.pos[pndx];
        const float dsq = (dist.x * dist.x + dist.y * dist.y + dist.z * dist.z);
        if (dsq < r2 && dsq > 0.0f) {
            const float c = (r2 - dsq) * d2;
            sum += c * c * c;
        }
    }
    return sum;
}

// (The reference had a __syncthreads() after the neighbour loop, after threads may have returned; there is no
// shared memory to synchronise and it is left out.)
[shader("compute")]
[numthreads(FLUID_THREADS, 1, 1)]
void computePressure(uniform FluidParams fparam, uniform FluidBufs fbuf, uniform int pnum,
                     uint3 tid : SV_DispatchThreadID)
{
    const uint i = tid.x;   // particle index
    if (i >= pnum) return;

    // search cell (the lower corner of the search block)
    const int nadj = (1 * fparam.gridRes.z + 1) * fparam.gridRes.x + 1;
    uint gc = fbuf.gcell[i];
    if (gc == FLUID_GRID_UNDEF) return;   // particle out of range
    gc -= nadj;

    // sum pressures
    const float3 pos = fbuf.pos[i];
    float sum = 0.0f;
    for (int c = 0; c < fparam.gridAdjCnt; c++) sum += contributePressure(fparam, fbuf, i, pos, gc + fparam.gridAdj[c]);

    // density and pressure
    sum = sum * fparam.pmass * fparam.poly6kern;
    if (sum == 0.0f) sum = 1.0f;
    fbuf.press[i] = (sum - fparam.prest_dens) * fparam.pintstiff;
    fbuf.density[i] = 1.0f / sum;
}

[ForceInline]
float3 contributeForce(FluidParams fparam, FluidBufs fbuf, int i, float3 ipos, float3 iveleval, float ipress,
                       float idens, int cell)
{
    if (fbuf.gridcnt[cell] == 0) return float3(0, 0, 0);

    float3 force = float3(0, 0, 0);
    const int clast = fbuf.gridoff[cell] + fbuf.gridcnt[cell];
    for (int cndx = fbuf.gridoff[cell]; cndx < clast; cndx++) {
        const int j = fbuf.grid[cndx];
        const float3 dist = (ipos - fbuf.pos[j]);   // dist in cm
        float dsq = (dist.x * dist.x + dist.y * dist.y + dist.z * dist.z);
        if (dsq < fparam.rd2 && dsq > 0) {
            dsq = sqrt(dsq * fparam.d2);
            const float c = (fparam.psmoothradius - dsq);
            const float pterm = fparam.psimscale * -0.5f * c * fparam.spikykern * (ipress + fbuf.press[j]) / dsq;
            force += (pterm * dist + fparam.vterm * (fbuf.veval[j] - iveleval)) * c * idens * (fbuf.density[j]);
        }
    }
    return force;
}

[shader("compute")]
[numthreads(FLUID_THREADS, 1, 1)]
void computeForce(uniform FluidParams fparam, uniform FluidBufs fbuf, uniform int pnum,
                  uint3 tid : SV_DispatchThreadID)
{
    const uint i = tid.x;   // particle index
    if (i >= pnum) return;

    uint gc = fbuf.gcell[i];
    if (gc == FLUID_GRID_UNDEF) return;   // particle out of range
    gc -= (1 * fparam.gridRes.z + 1) * fparam.gridRes.x + 1;

    float3 force = float3(0, 0, 0);
    for (int c = 0; c < fparam.gridAdjCnt; c++) {
        force += contributeForce(fparam, fbuf, i, fbuf.pos[i], fbuf.veval[i], fbuf.press[i], fbuf.density[i],
                                 gc + fparam.gridAdj[c]);
    }
    fbuf.force[i] = force;
}

[shader("compute")]
[numthreads(FLUID_THREADS, 1, 1)]
void advanceParticles(uniform FluidParams fparam, uniform FluidBufs fbuf, uniform float time, uniform float dt,
                      uniform float ss, uniform int numPnts, uint3 tid : SV_DispatchThreadID)
{
    const uint i = tid.x;   // particle index
    if (i >= numPnts) return;

    if (fbuf.gcell[i] == FLUID_GRID_UNDEF) {
        fbuf.pos[i] = float3(-1000, -1000, -1000);
        fbuf.vel[i] = float3(0, 0, 0);
        return;
    }

    float3 norm;
    float diff, adj, speed;
    const float3 pos = fbuf.pos[i];
    const float3 veval = fbuf.veval[i];

    // leapfrog integration
    float3 accel = fbuf.force[i];
    accel *= fparam.pmass;

    // boundaries
    // Y axis (sloped ground)
    diff = fparam.pradius - (pos.y - (fparam.pboundmin.y + (pos.x - fparam.pboundmin.x) * fparam.pground_slope)) * ss;
    if (diff > EPSILON) {
        norm = float3(-fparam.pground_slope, 1.0f - fparam.pground_slope, 0);
        adj = fparam.pextstiff * diff - fparam.pdamp * dot(norm, veval);
        norm *= adj;
        accel += norm;
    }
    diff = fparam.pradius - (fparam.pboundmax.y - pos.y) * ss;
    if (diff > EPSILON) {
        norm = float3(0, -1, 0);
        adj = fparam.pextstiff * diff - fparam.pdamp * dot(norm, veval);
        norm *= adj;
        accel += norm;
    }

    // X axis (wave generator walls)
    diff = fparam.pradius - (pos.x - (fparam.pboundmin.x + (sin(time * fparam.pforce_freq) + 1) * 0.5f * fparam.pforce_min)) * ss;
    if (diff > EPSILON) {
        norm = float3(1, 0, 0);
        adj = (fparam.pforce_min + 1) * fparam.pextstiff * diff - fparam.pdamp * dot(norm, veval);
        norm *= adj;
        accel += norm;
    }
    diff = fparam.pradius - ((fparam.pboundmax.x - (sin(time * fparam.pforce_freq) + 1) * 0.5f * fparam.pforce_max) - pos.x) * ss;
    if (diff > EPSILON) {
        norm = float3(-1, 0, 0);
        adj = (fparam.pforce_max + 1) * fparam.pextstiff * diff - fparam.pdamp * dot(norm, veval);
        norm *= adj;
        accel += norm;
    }

    // Z axis
    diff = fparam.pradius - (pos.z - fparam.pboundmin.z) * ss;
    if (diff > EPSILON) {
        norm = float3(0, 0, 1);
        adj = fparam.pextstiff * diff - fparam.pdamp * dot(norm, veval);
        norm *= adj;
        accel += norm;
    }
    diff = fparam.pradius - (fparam.pboundmax.z - pos.z) * ss;
    if (diff > EPSILON) {
        norm = float3(0, 0, -1);
        adj = fparam.pextstiff * diff - fparam.pdamp * dot(norm, veval);
        norm *= adj;
        accel += norm;
    }

    // gravity
    accel += fparam.pgravity;

    // acceleration limit
    speed = accel.x * accel.x + accel.y * accel.y + accel.z * accel.z;
    if (speed > fparam.AL2) accel *= fparam.AL / sqrt(speed);

    // velocity limit
    float3 vel = fbuf.vel[i];
    speed = vel.x * vel.x + vel.y * vel.y + vel.z * vel.z;
    if (speed > fparam.VL2) {
        speed = fparam.VL2;
        vel *= fparam.VL / sqrt(speed);
    }

    // leap-frog integration
    const float3 vnext = accel * dt + vel;          // v(t+1/2) = v(t-1/2) + a(t) dt
    fbuf.veval[i] = (vel + vnext) * 0.5f;           // v(t+1) = [v(t-1/2) + v(t+1/2)] * 0.5
    fbuf.vel[i] = vnext;
    fbuf.pos[i] += vnext * (dt / ss);               // p(t+1) = p(t) + v(t+1/2) dt
}

// Adds the block sums of `aux` to the 2 * FLUID_SCAN_BLOCKSIZE elements of each block.
[shader("compute")]
[numthreads(FLUID_SCAN_BLOCKSIZE, 1, 1)]
void prefixFixup(uniform uint* input, uniform uint* aux, uniform int len, uint3 thread : SV_GroupThreadID,
                 uint3 group : SV_GroupID)
{
    const uint t = thread.x;
    const uint start = t + 2 * group.x * FLUID_SCAN_BLOCKSIZE;
    if (start < len) input[start] += aux[group.x];
    if (start + FLUID_SCAN_BLOCKSIZE < len) input[start + FLUID_SCAN_BLOCKSIZE] += aux[group.x];
}

groupshared uint scan_array[FLUID_SCAN_BLOCKSIZE << 1];

// Inclusive scan of 2 * FLUID_SCAN_BLOCKSIZE elements per block (exclusive when zeroff: shifted by one with a 0
// in front); the block sums go to `aux` (may be null).
[shader("compute")]
[numthreads(FLUID_SCAN_BLOCKSIZE, 1, 1)]
void prefixSum(uniform uint* input, uniform uint* output, uniform uint* aux, uniform int len, uniform int zeroff,
               uint3 thread : SV_GroupThreadID, uint3 group : SV_GroupID)
{
    const uint t1 = thread.x + 2 * group.x * FLUID_SCAN_BLOCKSIZE;
    const uint t2 = t1 + FLUID_SCAN_BLOCKSIZE;

    // pre-load into shared memory
    scan_array[thread.x] = (t1 < len) ? input[t1] : 0;
    scan_array[thread.x + FLUID_SCAN_BLOCKSIZE] = (t2 < len) ? input[t2] : 0;
    GroupMemoryBarrierWithGroupSync();

    // reduction
    int stride;
    for (stride = 1; stride <= FLUID_SCAN_BLOCKSIZE; stride <<= 1) {
        const int index = (thread.x + 1) * stride * 2 - 1;
        if (index < 2 * FLUID_SCAN_BLOCKSIZE) scan_array[index] += scan_array[index - stride];
        GroupMemoryBarrierWithGroupSync();
    }

    // post reduction
    for (stride = FLUID_SCAN_BLOCKSIZE >> 1; stride > 0; stride >>= 1) {
        const int index = (thread.x + 1) * stride * 2 - 1;
        if (index + stride < 2 * FLUID_SCAN_BLOCKSIZE) scan_array[index + stride] += scan_array[index];
        GroupMemoryBarrierWithGroupSync();
    }
    GroupMemoryBarrierWithGroupSync();

    // output values and aux
    if (t1 + zeroff < len) output[t1 + zeroff] = scan_array[thread.x];
    if (t2 + zeroff < len)
        output[t2 + zeroff] =
            (thread.x == FLUID_SCAN_BLOCKSIZE - 1 && zeroff != 0) ? 0 : scan_array[thread.x + FLUID_SCAN_BLOCKSIZE];
    if (thread.x == 0) {
        if (zeroff != 0) output[0] = 0;
        if (aux != nullptr) aux[group.x] = scan_array[2 * FLUID_SCAN_BLOCKSIZE - 1];
    }
}
