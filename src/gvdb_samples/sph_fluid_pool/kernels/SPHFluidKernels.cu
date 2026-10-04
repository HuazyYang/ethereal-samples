//-----------------------------------------------------------------------------
// FLUIDS v5.0 - SPH Fluid Simulator for CPU and GPU
// Copyright (C) 2012-2013, 2021. Rama Hoetzlein, http://fluids3.com
//-----------------------------------------------------------------------------
// Kernels of the SPH fluid (particles.cu of the reference), launched by
// SPHSolver.cpp. The particle and grid buffers are reached through tables of
// device pointers in __constant__ memory (FPnts = current / sorted particles,
// FPntTmp = the copy the counting sort reads, FAccel = acceleration grid) and
// the parameters through FParams; the solver uploads all four with
// IDeviceQueue::setConstantBuffer().
//
// The logic is the reference's. Not ported: the unused device helpers
// (contributePressure, contributeForce, getGridCell, debugAccess) and the
// curand includes. `register` and __mul24 are dropped (plain 32-bit multiply).
#include <gvdb/cuda_math.cuh>

#define SPH_KERNEL
#include "../SPHFluidParams.h"

#define EPSILON 0.00001f

__constant__ SPHFluidParams FParams;
__constant__ SPHBufferTable FPnts;
__constant__ SPHBufferTable FPntTmp;
__constant__ SPHBufferTable FAccel;

extern "C" __global__ void insertParticles(int pnum) {
    uint i = blockIdx.x * blockDim.x + threadIdx.x;   // particle index
    if (i >= pnum) return;

    float3 p;
    float3 gcf;
    int3 gc;
    int gs;

    p = FPnts.bufF3(SPH_FPOS)[i];
    gcf = (p - FParams.gridMin) * FParams.gridDelta;
    gc = make_int3(int(gcf.x), int(gcf.y), int(gcf.z));
    gs = (gc.y * FParams.gridRes.z + gc.z) * FParams.gridRes.x + gc.x;

    if (gc.x >= 1 && gc.x <= FParams.gridScanMax.x && gc.y >= 1 && gc.y <= FParams.gridScanMax.y && gc.z >= 1 &&
        gc.z <= FParams.gridScanMax.z) {
        FPnts.bufI(SPH_FGCELL)[i] = gs;                                           // grid cell insert
        FPnts.bufI(SPH_FGNDX)[i] = atomicAdd(&FAccel.bufI(SPH_AGRIDCNT)[gs], 1);   // grid counts
    } else {
        FPnts.bufI(SPH_FGCELL)[i] = SPH_GRID_UNDEF;
    }
}

extern "C" __global__ void countingSortFull(int pnum) {
    uint i = blockIdx.x * blockDim.x + threadIdx.x;   // particle index
    if (i >= pnum) return;

    // This algorithm is O(2NK) in space, O(N/P) time, where K = sizeof(Fluid).
    // Copy the particle from the original, unsorted buffer (FPntTmp) into its
    // sorted memory location (FPnts).
    // **NOTE** Shared memory cannot be used for temporary storage since this is
    // a global reordering and there is no synchronization across blocks.

    int icell = FPntTmp.bufI(SPH_FGCELL)[i];

    if (icell != SPH_GRID_UNDEF) {
        // Determine the sort_ndx; location of the particle after the sort
        int indx = FPntTmp.bufI(SPH_FGNDX)[i];
        int sort_ndx = FAccel.bufI(SPH_AGRIDOFF)[icell] + indx;   // global_ndx = grid_cell_offset + particle_offset

        // Transfer data to the sort location
        FPnts.bufF3(SPH_FPOS)[sort_ndx] = FPntTmp.bufF3(SPH_FPOS)[i];
        FPnts.bufF3(SPH_FVEL)[sort_ndx] = FPntTmp.bufF3(SPH_FVEL)[i];
        FPnts.bufI(SPH_FCLR)[sort_ndx] = FPntTmp.bufI(SPH_FCLR)[i];
        FPnts.bufF3(SPH_FVEVAL)[sort_ndx] = FPntTmp.bufF3(SPH_FVEVAL)[i];
        FPnts.bufF(SPH_FPRESS)[sort_ndx] = FPntTmp.bufF(SPH_FPRESS)[i];
        FPnts.bufF3(SPH_FFORCE)[sort_ndx] = FPntTmp.bufF3(SPH_FFORCE)[i];
        FPnts.bufI(SPH_FGCELL)[sort_ndx] = icell;
        FPnts.bufI(SPH_FGNDX)[sort_ndx] = indx;

        FAccel.bufI(SPH_AGRID)[sort_ndx] = sort_ndx;   // full sort, grid indexing becomes identity
    }
}

extern "C" __global__ void computePressure(int pnum) {
    uint i = blockIdx.x * blockDim.x + threadIdx.x;   // particle index
    if (i >= pnum) return;

    // Get search cell
    uint gc = FPnts.bufI(SPH_FGCELL)[i];
    if (gc == SPH_GRID_UNDEF) return;   // particle out-of-range
    gc -= (1 * FParams.gridRes.z + 1) * FParams.gridRes.x + 1;

    float3 dist;
    float dsq, sum = 0.0;
    int cell;

    // Sum Pressures
    float3 pos = FPnts.bufF3(SPH_FPOS)[i];

    for (int c = 0; c < FParams.gridAdjCnt; c++) {
        cell = gc + FParams.gridAdj[c];
        int clast = FAccel.bufI(SPH_AGRIDOFF)[cell] + FAccel.bufI(SPH_AGRIDCNT)[cell];
        for (int cndx = FAccel.bufI(SPH_AGRIDOFF)[cell]; cndx < clast; cndx++) {
            dist = pos - FPnts.bufF3(SPH_FPOS)[FAccel.bufI(SPH_AGRID)[cndx]];
            dsq = (dist.x * dist.x + dist.y * dist.y + dist.z * dist.z);
            if (dsq < FParams.rd2 && dsq > 0.0) {
                dsq = (FParams.rd2 - dsq) * FParams.d2;
                sum += dsq * dsq * dsq;
            }
        }
    }
    __syncthreads();

    // Compute Density & Pressure
    sum = sum * FParams.pmass * FParams.poly6kern;
    if (sum == 0.0) sum = 1.0;
    FPnts.bufF(SPH_FPRESS)[i] = sum;
}

extern "C" __global__ void computeForce(int pnum) {
    uint i = blockIdx.x * blockDim.x + threadIdx.x;   // particle index
    if (i >= pnum) return;

    // Get search cell
    uint gc = FPnts.bufI(SPH_FGCELL)[i];
    if (gc == SPH_GRID_UNDEF) return;   // particle out-of-range
    gc -= (1 * FParams.gridRes.z + 1) * FParams.gridRes.x + 1;

    // Sum Pressures
    int cell, c, j, cndx;
    float3 force, dist;
    float pterm, dsq;
    float pi, pj;

    force = make_float3(0, 0, 0);

    for (c = 0; c < FParams.gridAdjCnt; c++) {
        cell = gc + FParams.gridAdj[c];

        for (cndx = FAccel.bufI(SPH_AGRIDOFF)[cell];
             cndx < FAccel.bufI(SPH_AGRIDOFF)[cell] + FAccel.bufI(SPH_AGRIDCNT)[cell]; cndx++) {
            j = FAccel.bufI(SPH_AGRID)[cndx];
            dist = (FPnts.bufF3(SPH_FPOS)[i] - FPnts.bufF3(SPH_FPOS)[j]);   // dist in cm
            dsq = (dist.x * dist.x + dist.y * dist.y + dist.z * dist.z);
            if (dsq < FParams.rd2 && dsq > 0) {
                dsq = sqrt(dsq * FParams.d2);
                pi = (FPnts.bufF(SPH_FPRESS)[i] - FParams.prest_dens) * FParams.pintstiff;
                pj = (FPnts.bufF(SPH_FPRESS)[j] - FParams.prest_dens) * FParams.pintstiff;
                pterm = FParams.sim_scale * -0.5f * (FParams.psmoothradius - dsq) * FParams.spikykern * (pi + pj) / dsq;
                force += (pterm * dist + FParams.vterm * (FPnts.bufF3(SPH_FVEVAL)[j] - FPnts.bufF3(SPH_FVEVAL)[i])) *
                         (FParams.psmoothradius - dsq) / (FPnts.bufF(SPH_FPRESS)[i] * FPnts.bufF(SPH_FPRESS)[j]);
            }
        }
    }
    FPnts.bufF3(SPH_FFORCE)[i] = force;
}

extern "C" __global__ void advanceParticles(float time, float dt, float ss, int numPnts) {
    uint i = blockIdx.x * blockDim.x + threadIdx.x;   // particle index
    if (i >= numPnts) return;

    if (FPnts.bufI(SPH_FGCELL)[i] == SPH_GRID_UNDEF) {
        FPnts.bufF3(SPH_FPOS)[i] = make_float3(-1000, -1000, -1000);
        FPnts.bufF3(SPH_FVEL)[i] = make_float3(0, 0, 0);
        return;
    }

    // Get particle vars
    float3 accel, norm;
    float3 pos = FPnts.bufF3(SPH_FPOS)[i];
    float3 vel = FPnts.bufF3(SPH_FVEL)[i];
    float3 veval = FPnts.bufF3(SPH_FVEVAL)[i];
    float diff, adj, speed;

    // Leapfrog integration
    accel = FPnts.bufF3(SPH_FFORCE)[i] * FParams.pmass;

    // Boundaries
    // Y-axis
    diff = FParams.pradius -
           (pos.y - (FParams.bound_min.y + (pos.x - FParams.bound_min.x) * FParams.bound_slope)) * ss;
    if (diff > EPSILON) {
        norm = make_float3(-FParams.bound_slope, 1.0 - FParams.bound_slope, 0);
        adj = FParams.bound_stiff * diff - FParams.bound_damp * dot(norm, veval);
        norm *= adj;
        accel += norm - veval * FParams.bound_friction;
    }

    diff = FParams.pradius - (FParams.bound_max.y - pos.y) * ss;
    if (diff > EPSILON) {
        norm = make_float3(0, -1, 0);
        adj = FParams.bound_stiff * diff - FParams.bound_damp * dot(norm, veval);
        norm *= adj;
        accel += norm - veval * FParams.bound_friction;
    }

    // X-axis
    float wall = (sin(time * FParams.bound_wall_freq + pos.z / 400.f) * 0.5 + 0.5) * FParams.bound_wall_force;
    diff = FParams.pradius - (pos.x - (FParams.bound_min.x + wall)) * ss;
    if (diff > EPSILON) {
        norm = make_float3(1, 0, 0);
        adj = wall * FParams.bound_stiff * diff - FParams.bound_damp * dot(norm, veval);
        norm *= adj;
        accel += norm;
    }
    diff = FParams.pradius - (FParams.bound_max.x - pos.x) * ss;
    if (diff > EPSILON) {
        norm = make_float3(-1, 0, 0);
        adj = FParams.bound_stiff * diff - FParams.bound_damp * dot(norm, veval);
        norm *= adj;
        accel += norm;
    }

    // Z-axis
    diff = FParams.pradius - (pos.z - FParams.bound_min.z) * ss;
    if (diff > EPSILON) {
        norm = make_float3(0, 0, 1);
        adj = FParams.bound_stiff * diff - FParams.bound_damp * dot(norm, veval);
        norm *= adj;
        accel += norm;
    }
    diff = FParams.pradius - (FParams.bound_max.z - pos.z) * ss;
    if (diff > EPSILON) {
        norm = make_float3(0, 0, -1);
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
    float3 vnext = accel * dt + vel;                         // v(t+1/2) = v(t-1/2) + a(t) dt
    FPnts.bufF3(SPH_FVEVAL)[i] = (vel + vnext) * 0.5;        // v(t+1) = [v(t-1/2) + v(t+1/2)] * 0.5
    FPnts.bufF3(SPH_FVEL)[i] = vnext;
    FPnts.bufF3(SPH_FPOS)[i] += vnext * (dt / ss);           // p(t+1) = p(t) + v(t+1/2) dt
}

extern "C" __global__ void prefixFixup(uint *input, uint *aux, int len) {
    unsigned int t = threadIdx.x;
    unsigned int start = t + 2 * blockIdx.x * SPH_SCAN_BLOCKSIZE;
    if (start < len) input[start] += aux[blockIdx.x];
    if (start + SPH_SCAN_BLOCKSIZE < len) input[start + SPH_SCAN_BLOCKSIZE] += aux[blockIdx.x];
}

extern "C" __global__ void prefixSum(uint *input, uint *output, uint *aux, int len, int zeroff) {
    __shared__ uint scan_array[SPH_SCAN_BLOCKSIZE << 1];
    unsigned int t1 = threadIdx.x + 2 * blockIdx.x * SPH_SCAN_BLOCKSIZE;
    unsigned int t2 = t1 + SPH_SCAN_BLOCKSIZE;

    // Pre-load into shared memory
    scan_array[threadIdx.x] = (t1 < len) ? input[t1] : 0.0f;
    scan_array[threadIdx.x + SPH_SCAN_BLOCKSIZE] = (t2 < len) ? input[t2] : 0.0f;
    __syncthreads();

    // Reduction
    int stride;
    for (stride = 1; stride <= SPH_SCAN_BLOCKSIZE; stride <<= 1) {
        int index = (threadIdx.x + 1) * stride * 2 - 1;
        if (index < 2 * SPH_SCAN_BLOCKSIZE) scan_array[index] += scan_array[index - stride];
        __syncthreads();
    }

    // Post reduction
    for (stride = SPH_SCAN_BLOCKSIZE >> 1; stride > 0; stride >>= 1) {
        int index = (threadIdx.x + 1) * stride * 2 - 1;
        if (index + stride < 2 * SPH_SCAN_BLOCKSIZE) scan_array[index + stride] += scan_array[index];
        __syncthreads();
    }
    __syncthreads();

    // Output values & aux
    if (t1 + zeroff < len) output[t1 + zeroff] = scan_array[threadIdx.x];
    if (t2 + zeroff < len)
        output[t2 + zeroff] =
            (threadIdx.x == SPH_SCAN_BLOCKSIZE - 1 && zeroff) ? 0 : scan_array[threadIdx.x + SPH_SCAN_BLOCKSIZE];
    if (threadIdx.x == 0) {
        if (zeroff) output[0] = 0;
        if (aux) aux[blockIdx.x] = scan_array[2 * SPH_SCAN_BLOCKSIZE - 1];
    }
}
