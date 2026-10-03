//-----------------------------------------------------------------------------
// NVIDIA(R) GVDB VOXELS
// Copyright 2018 NVIDIA Corporation
// SPDX-License-Identifier: Apache-2.0
//-----------------------------------------------------------------------------
// point-fusion: the simulated depth scanner of gPointFusion
// (point_fusion_cuda.cu). One thread per scan pixel shoots a jittered view
// ray against the axis-aligned boxes of the city and writes the nearest hit
// (or (0,0,0) for a miss, as the reference did) and the box colour into the
// point lists. Loaded by the sample as ptx/PointFusionScan.ptx.
//
// Differences from the reference: the buffers are kernel arguments instead of
// device pointers inside the constant block, and the hit counter is a buffer
// instead of a module global, so that the host only needs the gp API.

#include <gvdb/cuda_math.cuh>

struct __align__(16) ScanObj {
	float3		pos;
	float3		size;
	float3		loc;
	uint		clr;
};

// Per-scan constants (ScanInfo on the host): the cell grid of the city and
// the camera rays of the scanner (top-left ray, right / down increments).
struct __align__(16) ScanInfo {
	int3		gridRes;
	float3		gridSize;
	float3		cams;
	float3		camu;
	float3		camv;
};
__constant__ ScanInfo		scan;

#define NOHIT			1.0e10f

// lcg() / rnd() (the reference's LCG jitter) come from cuda_math.cuh.

// Get view ray
inline __device__ float3 getViewRay ( float x, float y )
{
	float3 v = x*scan.camu + y*scan.camv + scan.cams;
	return normalize(v);
}

// Ray box intersection: (tnear, tfar, NOHIT when missed)
inline __device__ float3 rayBoxIntersect ( float3 rpos, float3 rdir, float3 vmin, float3 vmax )
{
	float ht[8];
	ht[0] = (vmin.x - rpos.x)/rdir.x;
	ht[1] = (vmax.x - rpos.x)/rdir.x;
	ht[2] = (vmin.y - rpos.y)/rdir.y;
	ht[3] = (vmax.y - rpos.y)/rdir.y;
	ht[4] = (vmin.z - rpos.z)/rdir.z;
	ht[5] = (vmax.z - rpos.z)/rdir.z;
	ht[6] = fmaxf(fmaxf(fminf(ht[0], ht[1]), fminf(ht[2], ht[3])), fminf(ht[4], ht[5]));
	ht[7] = fminf(fminf(fmaxf(ht[0], ht[1]), fmaxf(ht[2], ht[3])), fmaxf(ht[4], ht[5]));
	ht[6] = (ht[6] < 0 ) ? 0.0f : ht[6];
	return make_float3( ht[6], ht[7], (ht[7]<ht[6] || ht[7]<0) ? NOHIT : 0 );
}

// Pixel jitter from a 128x128 seed table (advanced in place)
inline __device__ float3 jitter_sample ( uint* rnd_seeds )
{
	uint index = (threadIdx.y % 128) * 128 + (threadIdx.x % 128);
	unsigned int seed = rnd_seeds[ index ];
	float uu = rnd( seed );
	float vv = rnd( seed );
	float ww = rnd( seed );
	rnd_seeds[ index ] = seed;
	return make_float3(uu,vv,ww);
}

extern "C" __global__ void scanBuildings ( float3 pos, int3 res, int num_obj, float tmax,
	ScanObj* objList, float3* pntList, uint* pntClrs, uint* rnd_seeds, int* pntout )
{
	int x = blockIdx.x * blockDim.x + threadIdx.x;
	int y = blockIdx.y * blockDim.y + threadIdx.y;
	if ( x >= res.x || y >= res.y ) return;

	float3 jit = jitter_sample( rnd_seeds );
	float3 dir = getViewRay( float(x+jit.x)/float(res.x), float(y+jit.y)/float(res.y) );

	// scanner outside the city grid: nothing to scan (as the reference)
	int gcell = int(pos.z/scan.gridSize.y) * scan.gridRes.x + int(pos.x/scan.gridSize.x);
	if ( gcell < 0 || gcell > scan.gridRes.x*scan.gridRes.y)  return;

	float3 t, tnearest;
	uint clr = 0;
	tnearest.x = NOHIT;

	for (int n=0; n < num_obj; n++) {
		const ScanObj& bldg = objList[n];
		t = rayBoxIntersect ( pos, dir, bldg.pos, bldg.pos + bldg.size );
		if ( t.x < tnearest.x && t.x < tmax && t.z != NOHIT ) {
			tnearest = t;
			clr = bldg.clr;
		}
	}
	if ( tnearest.x == NOHIT) { pntList[ y*res.x + x] = make_float3(0,0,0); return; }

	atomicAdd(pntout, 1);

	pntList[ y*res.x + x] = pos + tnearest.x * dir;
	pntClrs[ y*res.x + x] = clr;
}
