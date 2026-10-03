//-----------------------------------------------------------------------------
// FLUIDS v.3 - SPH Fluid Simulator for CPU and GPU
// Copyright (C) 2012-2013. Rama Hoetzlein, http://fluids3.com
//
// NVIDIA(R) GVDB VOXELS
// Copyright 2017 NVIDIA Corporation
// SPDX-License-Identifier: Apache-2.0
//-----------------------------------------------------------------------------
// GPU path of the fluid system (fluid_system_cuda.cu of the reference): the
// kernels launched by FluidSystem.cpp. The buffers come from __constant__
// tables (fbuf = current / sorted, ftemp = copy before the counting sort) and
// the parameters from fparam, all uploaded through gp setConstantBuffer().
// The curand based randomInit / emitParticles, sampleParticles and
// computeQuery of the reference are not part of its simulation step and are
// not ported.
#define FLUID_KERNEL
#include "FluidSystem.h"
#include <gvdb/cuda_math.cuh>

#define EPSILON 0.00001f

__constant__ FluidParams fparam;   // simulation parameters
__constant__ FluidBufs fbuf;       // particle buffers (sorted / current)
__constant__ FluidBufs ftemp;      // particle buffers (unsorted copy)

#define BUF_F3(t, n) ((float3 *)(t).ptr[n])
#define BUF_F(t, n) ((float *)(t).ptr[n])
#define BUF_I(t, n) ((uint *)(t).ptr[n])

extern "C" __global__ void insertParticles(int pnum) {
    uint i = blockIdx.x * blockDim.x + threadIdx.x;   // particle index
    if (i >= pnum) return;

    float3 gridMin = fparam.gridMin;
    float3 gridDelta = fparam.gridDelta;
    int3 gridRes = fparam.gridRes;
    int3 gridScan = fparam.gridScanMax;

    float3 gcf = (BUF_F3(fbuf, FPOS)[i] - gridMin) * gridDelta;
    int3 gc = make_int3(int(gcf.x), int(gcf.y), int(gcf.z));
    int gs = (gc.y * gridRes.z + gc.z) * gridRes.x + gc.x;

    if (gc.x >= 1 && gc.x <= gridScan.x && gc.y >= 1 && gc.y <= gridScan.y && gc.z >= 1 && gc.z <= gridScan.z) {
        BUF_I(fbuf, FGCELL)[i] = gs;                                        // grid cell insert
        BUF_I(fbuf, FGNDX)[i] = atomicAdd(&BUF_I(fbuf, FGRIDCNT)[gs], 1);   // grid counts
    } else {
        BUF_I(fbuf, FGCELL)[i] = FLUID_GRID_UNDEF;
    }
}

// Counting sort - full (deep copy from ftemp into the sorted slots of fbuf)
extern "C" __global__ void countingSortFull(int pnum) {
    uint i = blockIdx.x * blockDim.x + threadIdx.x;   // particle index
    if (i >= pnum) return;

    uint icell = BUF_I(ftemp, FGCELL)[i];
    if (icell != FLUID_GRID_UNDEF) {
        // location of the particle after the sort: cell offset + index inside the cell
        uint indx = BUF_I(ftemp, FGNDX)[i];
        int sort_ndx = BUF_I(fbuf, FGRIDOFF)[icell] + indx;

        BUF_I(fbuf, FGRID)[sort_ndx] = sort_ndx;   // full sort: grid indexing becomes identity
        BUF_F3(fbuf, FPOS)[sort_ndx] = BUF_F3(ftemp, FPOS)[i];
        BUF_F3(fbuf, FVEL)[sort_ndx] = BUF_F3(ftemp, FVEL)[i];
        BUF_F3(fbuf, FVEVAL)[sort_ndx] = BUF_F3(ftemp, FVEVAL)[i];
        BUF_F3(fbuf, FFORCE)[sort_ndx] = BUF_F3(ftemp, FFORCE)[i];
        BUF_F(fbuf, FPRESS)[sort_ndx] = BUF_F(ftemp, FPRESS)[i];
        BUF_F(fbuf, FDENSITY)[sort_ndx] = BUF_F(ftemp, FDENSITY)[i];
        BUF_I(fbuf, FCLR)[sort_ndx] = BUF_I(ftemp, FCLR)[i];
        BUF_I(fbuf, FGCELL)[sort_ndx] = icell;
        BUF_I(fbuf, FGNDX)[sort_ndx] = indx;
    }
}

__device__ float contributePressure(int i, float3 p, int cell) {
    if (BUF_I(fbuf, FGRIDCNT)[cell] == 0) return 0.0f;

    float3 dist;
    float dsq, c, sum = 0.0f;
    float d2 = fparam.psimscale * fparam.psimscale;
    float r2 = fparam.r2 / d2;

    int clast = BUF_I(fbuf, FGRIDOFF)[cell] + BUF_I(fbuf, FGRIDCNT)[cell];
    for (int cndx = BUF_I(fbuf, FGRIDOFF)[cell]; cndx < clast; cndx++) {
        int pndx = BUF_I(fbuf, FGRID)[cndx];
        dist = p - BUF_F3(fbuf, FPOS)[pndx];
        dsq = (dist.x * dist.x + dist.y * dist.y + dist.z * dist.z);
        if (dsq < r2 && dsq > 0.0f) {
            c = (r2 - dsq) * d2;
            sum += c * c * c;
        }
    }
    return sum;
}

extern "C" __global__ void computePressure(int pnum) {
    uint i = blockIdx.x * blockDim.x + threadIdx.x;   // particle index
    if (i >= pnum) return;

    // search cell (the lower corner of the search block)
    int nadj = (1 * fparam.gridRes.z + 1) * fparam.gridRes.x + 1;
    uint gc = BUF_I(fbuf, FGCELL)[i];
    if (gc == FLUID_GRID_UNDEF) return;   // particle out of range
    gc -= nadj;

    // sum pressures
    float3 pos = BUF_F3(fbuf, FPOS)[i];
    float sum = 0.0f;
    for (int c = 0; c < fparam.gridAdjCnt; c++) sum += contributePressure(i, pos, gc + fparam.gridAdj[c]);
    __syncthreads();

    // density and pressure
    sum = sum * fparam.pmass * fparam.poly6kern;
    if (sum == 0.0f) sum = 1.0f;
    BUF_F(fbuf, FPRESS)[i] = (sum - fparam.prest_dens) * fparam.pintstiff;
    BUF_F(fbuf, FDENSITY)[i] = 1.0f / sum;
}

__device__ float3 contributeForce(int i, float3 ipos, float3 iveleval, float ipress, float idens, int cell) {
    if (BUF_I(fbuf, FGRIDCNT)[cell] == 0) return make_float3(0, 0, 0);

    float dsq, c, pterm;
    float3 dist, force = make_float3(0, 0, 0);
    int j;

    int clast = BUF_I(fbuf, FGRIDOFF)[cell] + BUF_I(fbuf, FGRIDCNT)[cell];
    for (int cndx = BUF_I(fbuf, FGRIDOFF)[cell]; cndx < clast; cndx++) {
        j = BUF_I(fbuf, FGRID)[cndx];
        dist = (ipos - BUF_F3(fbuf, FPOS)[j]);   // dist in cm
        dsq = (dist.x * dist.x + dist.y * dist.y + dist.z * dist.z);
        if (dsq < fparam.rd2 && dsq > 0) {
            dsq = sqrtf(dsq * fparam.d2);
            c = (fparam.psmoothradius - dsq);
            pterm = fparam.psimscale * -0.5f * c * fparam.spikykern * (ipress + BUF_F(fbuf, FPRESS)[j]) / dsq;
            force += (pterm * dist + fparam.vterm * (BUF_F3(fbuf, FVEVAL)[j] - iveleval)) * c * idens *
                     (BUF_F(fbuf, FDENSITY)[j]);
        }
    }
    return force;
}

extern "C" __global__ void computeForce(int pnum) {
    uint i = blockIdx.x * blockDim.x + threadIdx.x;   // particle index
    if (i >= pnum) return;

    uint gc = BUF_I(fbuf, FGCELL)[i];
    if (gc == FLUID_GRID_UNDEF) return;   // particle out of range
    gc -= (1 * fparam.gridRes.z + 1) * fparam.gridRes.x + 1;

    float3 force = make_float3(0, 0, 0);
    for (int c = 0; c < fparam.gridAdjCnt; c++) {
        force += contributeForce(i, BUF_F3(fbuf, FPOS)[i], BUF_F3(fbuf, FVEVAL)[i], BUF_F(fbuf, FPRESS)[i],
                                 BUF_F(fbuf, FDENSITY)[i], gc + fparam.gridAdj[c]);
    }
    BUF_F3(fbuf, FFORCE)[i] = force;
}

extern "C" __global__ void advanceParticles(float time, float dt, float ss, int numPnts) {
    uint i = blockIdx.x * blockDim.x + threadIdx.x;   // particle index
    if (i >= numPnts) return;

    if (BUF_I(fbuf, FGCELL)[i] == FLUID_GRID_UNDEF) {
        BUF_F3(fbuf, FPOS)[i] = make_float3(-1000, -1000, -1000);
        BUF_F3(fbuf, FVEL)[i] = make_float3(0, 0, 0);
        return;
    }

    float3 accel, norm;
    float diff, adj, speed;
    float3 pos = BUF_F3(fbuf, FPOS)[i];
    float3 veval = BUF_F3(fbuf, FVEVAL)[i];

    // leapfrog integration
    accel = BUF_F3(fbuf, FFORCE)[i];
    accel *= fparam.pmass;

    // boundaries
    // Y axis (sloped ground)
    diff = fparam.pradius - (pos.y - (fparam.pboundmin.y + (pos.x - fparam.pboundmin.x) * fparam.pground_slope)) * ss;
    if (diff > EPSILON) {
        norm = make_float3(-fparam.pground_slope, 1.0f - fparam.pground_slope, 0);
        adj = fparam.pextstiff * diff - fparam.pdamp * dot(norm, veval);
        norm *= adj;
        accel += norm;
    }
    diff = fparam.pradius - (fparam.pboundmax.y - pos.y) * ss;
    if (diff > EPSILON) {
        norm = make_float3(0, -1, 0);
        adj = fparam.pextstiff * diff - fparam.pdamp * dot(norm, veval);
        norm *= adj;
        accel += norm;
    }

    // X axis (wave generator walls)
    diff = fparam.pradius -
           (pos.x - (fparam.pboundmin.x + (sinf(time * fparam.pforce_freq) + 1) * 0.5f * fparam.pforce_min)) * ss;
    if (diff > EPSILON) {
        norm = make_float3(1, 0, 0);
        adj = (fparam.pforce_min + 1) * fparam.pextstiff * diff - fparam.pdamp * dot(norm, veval);
        norm *= adj;
        accel += norm;
    }
    diff = fparam.pradius -
           ((fparam.pboundmax.x - (sinf(time * fparam.pforce_freq) + 1) * 0.5f * fparam.pforce_max) - pos.x) * ss;
    if (diff > EPSILON) {
        norm = make_float3(-1, 0, 0);
        adj = (fparam.pforce_max + 1) * fparam.pextstiff * diff - fparam.pdamp * dot(norm, veval);
        norm *= adj;
        accel += norm;
    }

    // Z axis
    diff = fparam.pradius - (pos.z - fparam.pboundmin.z) * ss;
    if (diff > EPSILON) {
        norm = make_float3(0, 0, 1);
        adj = fparam.pextstiff * diff - fparam.pdamp * dot(norm, veval);
        norm *= adj;
        accel += norm;
    }
    diff = fparam.pradius - (fparam.pboundmax.z - pos.z) * ss;
    if (diff > EPSILON) {
        norm = make_float3(0, 0, -1);
        adj = fparam.pextstiff * diff - fparam.pdamp * dot(norm, veval);
        norm *= adj;
        accel += norm;
    }

    // gravity
    accel += fparam.pgravity;

    // acceleration limit
    speed = accel.x * accel.x + accel.y * accel.y + accel.z * accel.z;
    if (speed > fparam.AL2) accel *= fparam.AL / sqrtf(speed);

    // velocity limit
    float3 vel = BUF_F3(fbuf, FVEL)[i];
    speed = vel.x * vel.x + vel.y * vel.y + vel.z * vel.z;
    if (speed > fparam.VL2) {
        speed = fparam.VL2;
        vel *= fparam.VL / sqrtf(speed);
    }

    // leap-frog integration
    float3 vnext = accel * dt + vel;                   // v(t+1/2) = v(t-1/2) + a(t) dt
    BUF_F3(fbuf, FVEVAL)[i] = (vel + vnext) * 0.5f;    // v(t+1) = [v(t-1/2) + v(t+1/2)] * 0.5
    BUF_F3(fbuf, FVEL)[i] = vnext;
    BUF_F3(fbuf, FPOS)[i] += vnext * (dt / ss);        // p(t+1) = p(t) + v(t+1/2) dt
}

extern "C" __global__ void prefixFixup(uint *input, uint *aux, int len) {
    unsigned int t = threadIdx.x;
    unsigned int start = t + 2 * blockIdx.x * FLUID_SCAN_BLOCKSIZE;
    if (start < len) input[start] += aux[blockIdx.x];
    if (start + FLUID_SCAN_BLOCKSIZE < len) input[start + FLUID_SCAN_BLOCKSIZE] += aux[blockIdx.x];
}

extern "C" __global__ void prefixSum(uint *input, uint *output, uint *aux, int len, int zeroff) {
    __shared__ uint scan_array[FLUID_SCAN_BLOCKSIZE << 1];
    unsigned int t1 = threadIdx.x + 2 * blockIdx.x * FLUID_SCAN_BLOCKSIZE;
    unsigned int t2 = t1 + FLUID_SCAN_BLOCKSIZE;

    // pre-load into shared memory
    scan_array[threadIdx.x] = (t1 < len) ? input[t1] : 0;
    scan_array[threadIdx.x + FLUID_SCAN_BLOCKSIZE] = (t2 < len) ? input[t2] : 0;
    __syncthreads();

    // reduction
    int stride;
    for (stride = 1; stride <= FLUID_SCAN_BLOCKSIZE; stride <<= 1) {
        int index = (threadIdx.x + 1) * stride * 2 - 1;
        if (index < 2 * FLUID_SCAN_BLOCKSIZE) scan_array[index] += scan_array[index - stride];
        __syncthreads();
    }

    // post reduction
    for (stride = FLUID_SCAN_BLOCKSIZE >> 1; stride > 0; stride >>= 1) {
        int index = (threadIdx.x + 1) * stride * 2 - 1;
        if (index + stride < 2 * FLUID_SCAN_BLOCKSIZE) scan_array[index + stride] += scan_array[index];
        __syncthreads();
    }
    __syncthreads();

    // output values and aux
    if (t1 + zeroff < len) output[t1 + zeroff] = scan_array[threadIdx.x];
    if (t2 + zeroff < len)
        output[t2 + zeroff] =
            (threadIdx.x == FLUID_SCAN_BLOCKSIZE - 1 && zeroff) ? 0 : scan_array[threadIdx.x + FLUID_SCAN_BLOCKSIZE];
    if (threadIdx.x == 0) {
        if (zeroff) output[0] = 0;
        if (aux) aux[blockIdx.x] = scan_array[2 * FLUID_SCAN_BLOCKSIZE - 1];
    }
}
