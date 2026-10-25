//-----------------------------------------------------------------------------
// NVIDIA(R) GVDB VOXELS
// Copyright 2018 NVIDIA Corporation
// SPDX-License-Identifier: Apache-2.0
//-----------------------------------------------------------------------------
// point-fusion: the simulated depth scanner of gPointFusion
// (point_fusion_cuda.cu). One thread per scan pixel shoots a jittered view
// ray against the axis-aligned boxes of the city and writes the nearest hit
// (or (0,0,0) for a miss, as the reference did) and the box colour into the
// point lists. Loaded by the sample as shaders/gvdb/PointFusionScan.nvsc.
//
// Differences from the reference: the per-scan constants and the buffers are
// kernel arguments instead of a constant block with device pointers, and the
// hit counter is a buffer instead of a module global.

struct ScanObj
{
    float3      pos;
    float3      size;
    float3      loc;
    uint        clr;
    uint        pad0;          // 48 bytes, as the __align__(16) struct of the reference
    uint        pad1;
};

// Per-scan constants (ScanInfo on the host): the cell grid of the city and
// the camera rays of the scanner (top-left ray, right / down increments).
struct ScanInfo
{
    int3        gridRes;
    float3      gridSize;
    float3      cams;
    float3      camu;
    float3      camv;
};

static const float NOHIT = 1.0e10f;

// The reference's LCG jitter (lcg() / rnd() of cuda_math.cuh): a float in [0, 1).
float rnd(inout uint prev)
{
    prev = 1664525u * prev + 1013904223u;
    return float(prev & 0x00FFFFFFu) / float(0x01000000);
}

// Get view ray
float3 getViewRay(ScanInfo scan, float x, float y)
{
    float3 v = x * scan.camu + y * scan.camv + scan.cams;
    return normalize(v);
}

// Ray box intersection: (tnear, tfar, NOHIT when missed)
float3 rayBoxIntersect(float3 rpos, float3 rdir, float3 vmin, float3 vmax)
{
    float ht[8];
    ht[0] = (vmin.x - rpos.x) / rdir.x;
    ht[1] = (vmax.x - rpos.x) / rdir.x;
    ht[2] = (vmin.y - rpos.y) / rdir.y;
    ht[3] = (vmax.y - rpos.y) / rdir.y;
    ht[4] = (vmin.z - rpos.z) / rdir.z;
    ht[5] = (vmax.z - rpos.z) / rdir.z;
    ht[6] = max(max(min(ht[0], ht[1]), min(ht[2], ht[3])), min(ht[4], ht[5]));
    ht[7] = min(min(max(ht[0], ht[1]), max(ht[2], ht[3])), max(ht[4], ht[5]));
    ht[6] = (ht[6] < 0) ? 0.0f : ht[6];
    return float3(ht[6], ht[7], (ht[7] < ht[6] || ht[7] < 0) ? NOHIT : 0);
}

// Pixel jitter from a 128x128 seed table (advanced in place), indexed by the thread within its group as in the
// reference.
float3 jitter_sample(uint* rnd_seeds, uint3 thread)
{
    uint index = (thread.y % 128) * 128 + (thread.x % 128);
    uint seed = rnd_seeds[index];
    float uu = rnd(seed);
    float vv = rnd(seed);
    float ww = rnd(seed);
    rnd_seeds[index] = seed;
    return float3(uu, vv, ww);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void scanBuildings(uniform ScanInfo scan, uniform float3 pos, uniform int3 res, uniform int num_obj, uniform float tmax,
                   uniform ScanObj* objList, uniform float3* pntList, uniform uint* pntClrs, uniform uint* rnd_seeds,
                   uniform int* pntout, uint3 pixel : SV_DispatchThreadID, uint3 thread : SV_GroupThreadID)
{
    int x = int(pixel.x);
    int y = int(pixel.y);
    if (x >= res.x || y >= res.y) return;

    float3 jit = jitter_sample(rnd_seeds, thread);
    float3 dir = getViewRay(scan, float(x + jit.x) / float(res.x), float(y + jit.y) / float(res.y));

    // scanner outside the city grid: nothing to scan (as the reference)
    int gcell = int(pos.z / scan.gridSize.y) * scan.gridRes.x + int(pos.x / scan.gridSize.x);
    if (gcell < 0 || gcell > scan.gridRes.x * scan.gridRes.y) return;

    float3 t, tnearest = float3(NOHIT, 0, 0);
    uint clr = 0;

    for (int n = 0; n < num_obj; n++) {
        const ScanObj bldg = objList[n];
        t = rayBoxIntersect(pos, dir, bldg.pos, bldg.pos + bldg.size);
        if (t.x < tnearest.x && t.x < tmax && t.z != NOHIT) {
            tnearest = t;
            clr = bldg.clr;
        }
    }
    if (tnearest.x == NOHIT) { pntList[y * res.x + x] = float3(0, 0, 0); return; }

    InterlockedAdd(pntout[0], 1);

    pntList[y * res.x + x] = pos + tnearest.x * dir;
    pntClrs[y * res.x + x] = clr;
}
