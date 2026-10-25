//-----------------------------------------------------------------------------
// NVIDIA(R) GVDB VOXELS
// Copyright 2017 NVIDIA Corporation
// SPDX-License-Identifier: Apache-2.0
//
// Version 1.0: Rama Hoetzlein, 5/1/2017
// Version 1.1: Rama Hoetzlein, 3/25/2018
//-----------------------------------------------------------------------------
// GVDB Operators (port of cuda_gvdb_operators.cuh + kernelFillTex of cuda_gvdb_copydata.cu)
// - gvdbUpdateApron*   - update apron voxels (per atlas format)
// - gvdbClearAtlas*    - zero a whole atlas through its surface (per atlas format)
// - gvdbOpFill*        - fill a channel with a constant
// - gvdbOpSmooth / gvdbOpNoise / gvdbOpGrow / gvdbOpCut / gvdbOpClrExpand / gvdbOpExpandC
// - gvdbReduction      - sum of a float channel along z
// - gvdbResample       - dense source volume -> channel
// - gvdbDownsample     - dense source volume -> dense destination grid
//
// Every operator kernel has the arguments
//   (VDBInfo* gvdb, int3 atlasRes, uint8_t channel, float p1, float p2, float p3)
// and runs with 8x8x8 thread groups over the atlas (GVDB_VOXUNPACKED: full atlas
// resolution) or over the packed atlas resolution (GVDB_VOXPACKED: skipping
// the aprons; the host launches the groups for getAtlasPackedResDim()).
//-----------------------------------------------------------------------------
#include "GVDB.slangh"

//------- Random number generation (from the reference cuda_gvdb.cuh)

uint opHash(uint x)
{
    x += (x << 10u);    x ^= (x >> 6u);
    x += (x << 3u);     x ^= (x >> 11u);
    x += (x << 15u);
    return x;
}
uint opHash(int3 v) { return opHash(uint(v.x) ^ opHash(uint(v.y)) ^ opHash(uint(v.z))); }

// Construct a float with half-open range [0:1] using low 23 bits.
float opFloatConstruct(uint m)
{
    const uint ieeeMantissa = 0x007FFFFFu; // binary32 mantissa bitmask
    const uint ieeeOne = 0x3F800000u;      // 1.0 in IEEE binary32
    m &= ieeeMantissa;                     // Keep only mantissa bits (fractional part)
    m |= ieeeOne;                          // Add fractional part to 1.0
    float f = asfloat(m);                  // Range [1:2]
    return f - 1.0f;                       // Range [0:1]
}
// Pseudo-random value in half-open range [0:1].
float opRandom(float3 v)
{
    return opFloatConstruct(opHash(asint(v)));
}

//------- Apron update

// Updates the apron without UpdateApronFaces' neighbor table. Each of the
// voxels of the apron computes its world-space (= index-space) position and
// looks up what its value should be, sampling from a neighboring brick, or
// using the boundary value if no brick contains the voxel.
//
// Thread groups of 6 x 8 x 8, one group column per brick (group x): each yz
// plane of a group fills in the voxels of a different face.
void updateApron<T : ITexelElement>(VDBInfo* gvdb, uint8_t channel, int brickCount, int paddedBrickRes,
                                    T boundaryValue, uint3 group, uint3 thread)
{
    // The brick this group processes.
    const int brick = int(group.x);
    if (brick >= brickCount) return;

    // Determine which voxel of the apron to compute and write.
    uint3 brickVoxel = uint3(0, 0, 0); // In the local coordinate space of the brick
    {
        const int side = int(thread.x); // Side of the brick, from 0 to 5
        uint2 suv = uint2(group.y * 8 + thread.y, group.z * 8 + thread.z); // Position on the side of the brick
        if (suv.x >= uint(paddedBrickRes) || suv.y >= uint(paddedBrickRes) || side >= 6) return;

        switch (side) {
        case 0: brickVoxel = uint3(0, suv.x, suv.y); break;
        case 1: brickVoxel = uint3(suv.x, 0, suv.y); break;
        case 2: brickVoxel = uint3(suv.x, suv.y, 0); break;
        case 3: brickVoxel = uint3(uint(paddedBrickRes) - 1, suv.x, suv.y); break;
        case 4: brickVoxel = uint3(suv.x, uint(paddedBrickRes) - 1, suv.y); break;
        case 5: brickVoxel = uint3(suv.x, suv.y, uint(paddedBrickRes) - 1); break;
        default: break;
        }
    }

    // Compute the position of the voxel in the atlas
    uint3 atlasVoxel; // In the coordinate space of the entire atlas
    {
        VDBNode* node = getNode(gvdb, 0, brick);
        if (node == nullptr) return; // This brick ID didn't correspond to a known brick, which is invalid
        if (node->mFlags == 0) return; // Inactive brick: no atlas slot
        // (The (1,1,1) here accounts for the 1 unit of apron padding)
        atlasVoxel = brickVoxel + uint3(node->mValue) - uint3(1, 1, 1);
    }

    // Get the value of the voxel by converting to index-space and then
    // sampling the value at that index
    T value;
    {
        float3 worldPos;
        if (!getAtlasToWorld(gvdb, atlasVoxel, worldPos)) return;
        float3 offs, vmin;
        uint64_t nodeID;
        VDBNode* node = getNodeAtPoint(gvdb, worldPos, offs, vmin, nodeID);

        if (node == nullptr) {
            // Out of range, use the boundary value
            value = boundaryValue;
        } else {
            offs += (worldPos - vmin); // Get the atlas position
            value = atlasRead<T>(gvdb, channel, uint3(offs));
        }
    }

    // Write to the apron voxel
    atlasWrite<T>(gvdb, channel, atlasVoxel, value);
}

[shader("compute")]
[numthreads(6, 8, 8)]
void gvdbUpdateApronF(uniform VDBInfo* gvdb, uniform uint8_t chan, uniform int brickcnt, uniform int brickres,
                      uniform int brickwid, uniform float boundval, uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    updateApron<float>(gvdb, chan, brickcnt, brickres, boundval, group, thread);
}

[shader("compute")]
[numthreads(6, 8, 8)]
void gvdbUpdateApronF4(uniform VDBInfo* gvdb, uniform uint8_t chan, uniform int brickcnt, uniform int brickres,
                       uniform int brickwid, uniform float boundval, uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    updateApron<float4>(gvdb, chan, brickcnt, brickres, float4(boundval, boundval, boundval, boundval), group, thread);
}

[shader("compute")]
[numthreads(6, 8, 8)]
void gvdbUpdateApronC(uniform VDBInfo* gvdb, uniform uint8_t chan, uniform int brickcnt, uniform int brickres,
                      uniform int brickwid, uniform float boundval, uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    updateApron<uint8_t>(gvdb, chan, brickcnt, brickres, uint8_t(boundval), group, thread);
}

[shader("compute")]
[numthreads(6, 8, 8)]
void gvdbUpdateApronC4(uniform VDBInfo* gvdb, uniform uint8_t chan, uniform int brickcnt, uniform int brickres,
                       uniform int brickwid, uniform float boundval, uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    const uint8_t b = uint8_t(boundval);
    updateApron<uint8_t4>(gvdb, chan, brickcnt, brickres, uint8_t4(b, b, b, b), group, thread);
}

//------- Atlas clear (kernelFillTex of the reference, one entry per format:
// CUDA arrays cannot be memset, so zero is written through the surface.
// 8x8x8 thread groups over the whole atlas resolution.)

void clearAtlas<T : ITexelElement>(VDBInfo* gvdb, int3 atlasRes, uint8_t channel, T zero, uint3 t)
{
    if (t.x >= uint(atlasRes.x) || t.y >= uint(atlasRes.y) || t.z >= uint(atlasRes.z)) return;
    atlasWrite<T>(gvdb, channel, t, zero);
}

[shader("compute")]
[numthreads(8, 8, 8)]
void gvdbClearAtlasF(uniform VDBInfo* gvdb, uniform int3 atlasRes, uniform uint8_t channel, uint3 t : SV_DispatchThreadID)
{
    clearAtlas<float>(gvdb, atlasRes, channel, 0.0f, t);
}

[shader("compute")]
[numthreads(8, 8, 8)]
void gvdbClearAtlasF4(uniform VDBInfo* gvdb, uniform int3 atlasRes, uniform uint8_t channel, uint3 t : SV_DispatchThreadID)
{
    clearAtlas<float4>(gvdb, atlasRes, channel, float4(0.0f, 0.0f, 0.0f, 0.0f), t);
}

[shader("compute")]
[numthreads(8, 8, 8)]
void gvdbClearAtlasC(uniform VDBInfo* gvdb, uniform int3 atlasRes, uniform uint8_t channel, uint3 t : SV_DispatchThreadID)
{
    clearAtlas<uint8_t>(gvdb, atlasRes, channel, uint8_t(0), t);
}

[shader("compute")]
[numthreads(8, 8, 8)]
void gvdbClearAtlasC4(uniform VDBInfo* gvdb, uniform int3 atlasRes, uniform uint8_t channel, uint3 t : SV_DispatchThreadID)
{
    clearAtlas<uint8_t4>(gvdb, atlasRes, channel, uint8_t4(0, 0, 0, 0), t);
}

//------- Operators

// Loads the shared memory of a thread group, given the index of this thread in the shared memory and its location
// in the atlas. Called for values of ndx in the range [1,8]^3; makes additional loads as needed to fill in the voxels
// adjacent along an axis to [1,8]^3. Assumes thread groups of (8, 8, 8). (A macro: Slang passes no groupshared
// array by reference.)
#define LOAD_SHARED_MEMORY(T, sharedVoxels, gvdb, channel, ndx, voxI) \
    sharedVoxels[ndx.x][ndx.y][ndx.z] = atlasRead<T>(gvdb, channel, voxI); \
    if (ndx.x == 1) sharedVoxels[0][ndx.y][ndx.z] = atlasRead<T>(gvdb, channel, uint3(voxI.x - 1, voxI.y, voxI.z)); \
    else if (ndx.x == 8) sharedVoxels[9][ndx.y][ndx.z] = atlasRead<T>(gvdb, channel, uint3(voxI.x + 1, voxI.y, voxI.z)); \
    if (ndx.y == 1) sharedVoxels[ndx.x][0][ndx.z] = atlasRead<T>(gvdb, channel, uint3(voxI.x, voxI.y - 1, voxI.z)); \
    else if (ndx.y == 8) sharedVoxels[ndx.x][9][ndx.z] = atlasRead<T>(gvdb, channel, uint3(voxI.x, voxI.y + 1, voxI.z)); \
    if (ndx.z == 1) sharedVoxels[ndx.x][ndx.y][0] = atlasRead<T>(gvdb, channel, uint3(voxI.x, voxI.y, voxI.z - 1)); \
    else if (ndx.z == 8) sharedVoxels[ndx.x][ndx.y][9] = atlasRead<T>(gvdb, channel, uint3(voxI.x, voxI.y, voxI.z + 1)); \
    GroupMemoryBarrierWithGroupSync();

// For thread groups of (8, 8, 8): localIdx = thread + atlas_apron, atlasIdx = the voxel of the atlas this thread
// handles, skipping over aprons.
void getVoxelIndicesPacked(VDBInfo* gvdb, uint3 group, uint3 thread, out uint3 localIdx, out uint3 atlasIdx)
{
    const uint3 atlasApron = uint3(uint(gvdb->atlas_apron));
    localIdx = thread + atlasApron;

    // What atlasIdx would be if atlas_apron were 0
    const uint3 packedVox = group * 8 + thread;

    // Find the 3D index of the brick atlasIdx corresponds to
    const uint brickResNoApron = uint(gvdb->brick_res - 2 * gvdb->atlas_apron);
    const uint3 brick = packedVox / brickResNoApron;

    // Convert to a position in the full atlas
    atlasIdx = packedVox + brick * atlasApron * 2 + atlasApron;
}

// Sets up local and atlas coordinates and returns when they are outside the atlas. Skips over atlas boundaries, so it
// can use a smaller grid than GVDB_VOXUNPACKED. Assumes thread groups of (8,8,8).
#define GVDB_VOXPACKED \
    uint3 localIdx, atlasIdx; \
    getVoxelIndicesPacked(gvdb, group, thread, localIdx, atlasIdx); \
    const bool outside = atlasIdx.x >= uint(atlasRes.x) || atlasIdx.y >= uint(atlasRes.y) || atlasIdx.z >= uint(atlasRes.z);

// Sets up unpacked local and atlas coordinates and returns when they are outside the atlas. Covers the entire atlas.
#define GVDB_VOXUNPACKED \
    uint3 localIdx = thread + uint3(1, 1, 1); \
    uint3 atlasIdx = group * 8 + localIdx; \
    if (atlasIdx.x >= uint(atlasRes.x) || atlasIdx.y >= uint(atlasRes.y) || atlasIdx.z >= uint(atlasRes.z)) return;

#define OPERATOR_ARGUMENTS uniform VDBInfo* gvdb, uniform int3 atlasRes, uniform uint8_t channel, uniform float p1, \
    uniform float p2, uniform float p3, uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID

// NOTE(port): the reference returns from GVDB_VOXPACKED before the shared memory load and its __syncthreads(), so
// the threads outside the atlas leave the barrier; here they take part in the load (reading the atlas border) and
// return after the barrier.

groupshared float sharedF[10][10][10];
groupshared uint8_t4 sharedC4[10][10][10];
groupshared uint8_t sharedC[10][10][10];

[shader("compute")]
[numthreads(8, 8, 8)]
void gvdbOpGrow(OPERATOR_ARGUMENTS)
{
    GVDB_VOXPACKED
    LOAD_SHARED_MEMORY(float, sharedF, gvdb, channel, localIdx, atlasIdx)
    if (outside) return;

    float v = sharedF[localIdx.x][localIdx.y][localIdx.z];
    if (v != 0.0) v += p1 * 10.0;
    if (v < 0.01) v = 0.0;

    atlasWrite<float>(gvdb, channel, atlasIdx, v);
}

[shader("compute")]
[numthreads(8, 8, 8)]
void gvdbOpCut(OPERATOR_ARGUMENTS)
{
    GVDB_VOXPACKED
    LOAD_SHARED_MEMORY(float, sharedF, gvdb, channel, localIdx, atlasIdx)
    if (outside) return;

    // Determine block and index position
    float3 wpos;
    if (!getAtlasToWorld(gvdb, atlasIdx, wpos)) return;

    float v = sharedF[localIdx.x][localIdx.y][localIdx.z];
    if (wpos.x < 50 && v > 0 && v < 2) {
        v = 0.02;
        atlasWrite<float>(gvdb, channel, atlasIdx, v);
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void gvdbReduction(uniform VDBInfo* gvdb, uniform int3 res, uniform uint8_t chan, uniform int3 packres,
                   uniform float* outbuf, uint3 packvox : SV_DispatchThreadID)
{
    // voxels - not including apron
    uint bres = uint(gvdb->brick_res - gvdb->atlas_apron * 2);   // brick res minus apron
    uint3 brick = packvox / bres;
    uint3 vox = (brick * uint(gvdb->brick_res)) + (packvox % bres) + uint3(1, 1, 1);
    if (packvox.x >= uint(packres.x) || packvox.y >= uint(packres.y)) return;
    if (vox.x >= uint(res.x) || vox.y >= uint(res.y)) return;

    // integrate along z-axis
    float sum = 0.0;
    for (int z = 0; z < packres.z; z++) {
        vox.z = uint(int(uint(z) / bres) * gvdb->brick_res) + (uint(z) % bres) + 1;
        sum += atlasRead<float>(gvdb, chan, vox);
    }

    outbuf[packvox.y * uint(packres.x) + packvox.x] = sum;
}

[shader("compute")]
[numthreads(8, 8, 8)]
void gvdbResample(uniform VDBInfo* gvdb, uniform int3 atlasRes, uniform uint8_t chan, uniform int3 srcres,
                  uniform float* src, uniform float* xform, uniform float3 inr, uniform float3 outr,
                  uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    GVDB_VOXUNPACKED

    float3 wpos;
    if (!getAtlasToWorld(gvdb, atlasIdx, wpos)) return;
    wpos -= float3(.5, .5, .5);

    // transform to src index
    int3 ndx;
    ndx.x = int(wpos.x * xform[0] + wpos.y * xform[4] + wpos.z * xform[8] + xform[12]);
    ndx.y = int(wpos.x * xform[1] + wpos.y * xform[5] + wpos.z * xform[9] + xform[13]);
    ndx.z = int(wpos.x * xform[2] + wpos.y * xform[6] + wpos.z * xform[10] + xform[14]);

    // skip if outside src
    if (ndx.x < 0 || ndx.y < 0 || ndx.z < 0 || ndx.x >= srcres.x || ndx.y >= srcres.y || ndx.z >= srcres.z)
        return;

    // get value
    float v = src[(ndx.z * srcres.y + ndx.y) * srcres.x + ndx.x];
    v = outr.x + (v - inr.x) * (outr.y - outr.x) / (inr.y - inr.x);   // remap value

    atlasWrite<float>(gvdb, chan, atlasIdx, v);
}

[shader("compute")]
[numthreads(8, 8, 8)]
void gvdbDownsample(uniform int3 srcres, uniform float* src, uniform int3 destres, uniform float3 destmax,
                    uniform float* dest, uniform float* xform, uniform float3 inr, uniform float3 outr,
                    uint3 vox : SV_DispatchThreadID)
{
    if (vox.x >= uint(destres.x) || vox.y >= uint(destres.y) || vox.z >= uint(destres.z)) return;

    float3 dmin, dmax;
    dmin = float3(vox) * destmax / float3(destres + int3(1, 1, 1));
    dmax = float3(vox + uint3(1, 1, 1)) * destmax / float3(destres + int3(1, 1, 1)) - float3(1, 1, 1);

    // transform to src index
    int3 smin, smax;
    smin.x = int(dmin.x * xform[0] + dmin.y * xform[4] + dmin.z * xform[8] + xform[12]);
    smin.y = int(dmin.x * xform[1] + dmin.y * xform[5] + dmin.z * xform[9] + xform[13]);
    smin.z = int(dmin.x * xform[2] + dmin.y * xform[6] + dmin.z * xform[10] + xform[14]);

    smax.x = int(dmax.x * xform[0] + dmax.y * xform[4] + dmax.z * xform[8] + xform[12]);
    smax.y = int(dmax.x * xform[1] + dmax.y * xform[5] + dmax.z * xform[9] + xform[13]);
    smax.z = int(dmax.x * xform[2] + dmax.y * xform[6] + dmax.z * xform[10] + xform[14]);

    smin.x = (smin.x < 0) ? 0 : ((smin.x > srcres.x - 1) ? srcres.x - 1 : smin.x);
    smin.y = (smin.y < 0) ? 0 : ((smin.y > srcres.y - 1) ? srcres.y - 1 : smin.y);
    smin.z = (smin.z < 0) ? 0 : ((smin.z > srcres.z - 1) ? srcres.z - 1 : smin.z);

    smax.x = (smax.x < smin.x) ? smin.x : ((smax.x > srcres.x - 1) ? srcres.x - 1 : smax.x);
    smax.y = (smax.y < smin.y) ? smin.y : ((smax.y > srcres.y - 1) ? srcres.y - 1 : smax.y);
    smax.z = (smax.z < smin.z) ? smin.z : ((smax.z > srcres.z - 1) ? srcres.z - 1 : smax.z);

    // downsample
    float v = 0;
    for (int z = smin.z; z <= smax.z; z++)
        for (int y = smin.y; y <= smax.y; y++)
            for (int x = smin.x; x <= smax.x; x++) {
                v += outr.x + (src[(z * srcres.y + y) * srcres.x + x] - inr.x) * (outr.y - outr.x) / (inr.y - inr.x);
            }

    v /= float((smax.x - smin.x + 1) * (smax.y - smin.y + 1) * (smax.z - smin.z + 1));

    // output value
    dest[(vox.z * uint(destres.y) + vox.y) * uint(destres.x) + vox.x] = v;
}

[shader("compute")]
[numthreads(8, 8, 8)]
void gvdbOpFillF(OPERATOR_ARGUMENTS)
{
    GVDB_VOXUNPACKED

    if (p3 < 0) {
        float v = sin(float(atlasIdx.x * 12) / (3.141592 * 30.0));
        v += sin(float(atlasIdx.y * 12) / (3.141592 * 30.0));
        v += sin(float(atlasIdx.z * 12) / (3.141592 * 30.0));
        atlasWrite<float>(gvdb, channel, atlasIdx, v);
    } else {
        atlasWrite<float>(gvdb, channel, atlasIdx, p1);
    }
}

// Fill of a float4 channel (no reference kernel): (p1, p2, p3, 1).
[shader("compute")]
[numthreads(8, 8, 8)]
void gvdbOpFillF4(OPERATOR_ARGUMENTS)
{
    GVDB_VOXUNPACKED

    atlasWrite<float4>(gvdb, channel, atlasIdx, float4(p1, p2, p3, 1.0f));
}

[shader("compute")]
[numthreads(8, 8, 8)]
void gvdbOpFillC4(OPERATOR_ARGUMENTS)
{
    GVDB_VOXUNPACKED

    // CLR2INT of the reference: the colour packed into a uint, written as the 4 bytes of the texel.
    const float4 c = float4(p1, p2, p3, 1.0);
    const uint packed = (uint(c.w * 255.0f) << 24) | (uint(c.z * 255.0f) << 16) | (uint(c.y * 255.0f) << 8) | uint(c.x * 255.0f);
    atlasWrite<uint>(gvdb, channel, atlasIdx, packed);
}

[shader("compute")]
[numthreads(8, 8, 8)]
void gvdbOpFillC(OPERATOR_ARGUMENTS)
{
    GVDB_VOXUNPACKED

    atlasWrite<uint8_t>(gvdb, channel, atlasIdx, uint8_t(p1));
}

[shader("compute")]
[numthreads(8, 8, 8)]
void gvdbOpSmooth(OPERATOR_ARGUMENTS)
{
    GVDB_VOXPACKED
    LOAD_SHARED_MEMORY(float, sharedF, gvdb, channel, localIdx, atlasIdx)
    if (outside) return;

    //-- smooth
    float v = p1 * sharedF[localIdx.x][localIdx.y][localIdx.z];
    v += sharedF[localIdx.x - 1][localIdx.y][localIdx.z];
    v += sharedF[localIdx.x + 1][localIdx.y][localIdx.z];
    v += sharedF[localIdx.x][localIdx.y - 1][localIdx.z];
    v += sharedF[localIdx.x][localIdx.y + 1][localIdx.z];
    v += sharedF[localIdx.x][localIdx.y][localIdx.z - 1];
    v += sharedF[localIdx.x][localIdx.y][localIdx.z + 1];
    v = v / (p1 + 6.0) + p2;

    atlasWrite<float>(gvdb, channel, atlasIdx, v);
}

[shader("compute")]
[numthreads(8, 8, 8)]
void gvdbOpClrExpand(OPERATOR_ARGUMENTS)
{
    GVDB_VOXPACKED
    LOAD_SHARED_MEMORY(uint8_t4, sharedC4, gvdb, channel, localIdx, atlasIdx)
    if (outside) return;

    // As in the reference, int3 * float resolves to int3 * int: the weights are truncated.
    const int w1 = int(p1), w2 = int(p2);
    int3 c, cs;
    int cp;
    c = int3(sharedC4[localIdx.x][localIdx.y][localIdx.z].xyz);     cs = c * w1;
    c = int3(sharedC4[localIdx.x - 1][localIdx.y][localIdx.z].xyz); cs += c * w2;
    c = int3(sharedC4[localIdx.x + 1][localIdx.y][localIdx.z].xyz); cs += c * w2;
    c = int3(sharedC4[localIdx.x][localIdx.y - 1][localIdx.z].xyz); cs += c * w2;
    c = int3(sharedC4[localIdx.x][localIdx.y + 1][localIdx.z].xyz); cs += c * w2;
    c = int3(sharedC4[localIdx.x][localIdx.y][localIdx.z - 1].xyz); cs += c * w2;
    c = int3(sharedC4[localIdx.x][localIdx.y][localIdx.z + 1].xyz); cs += c * w2;
    cp = max(cs.x, max(cs.y, cs.z));
    cs = (cp > 255) ? int3(cs.x * 255 / cp, cs.y * 255 / cp, cs.z * 255 / cp) : cs;

    atlasWrite<uint8_t4>(gvdb, channel, atlasIdx, uint8_t4(uint8_t(cs.x), uint8_t(cs.y), uint8_t(cs.z), uint8_t(1)));
}

[shader("compute")]
[numthreads(8, 8, 8)]
void gvdbOpExpandC(OPERATOR_ARGUMENTS)
{
    GVDB_VOXPACKED
    LOAD_SHARED_MEMORY(uint8_t, sharedC, gvdb, channel, localIdx, atlasIdx)
    if (outside) return;

    const uint8_t match = uint8_t(p1);
    uint8_t c = 0;
    c = (sharedC[localIdx.x - 1][localIdx.y][localIdx.z] == match) ? uint8_t(1) : c;
    c = (sharedC[localIdx.x + 1][localIdx.y][localIdx.z] == match) ? uint8_t(1) : c;
    c = (sharedC[localIdx.x][localIdx.y - 1][localIdx.z] == match) ? uint8_t(1) : c;
    c = (sharedC[localIdx.x][localIdx.y + 1][localIdx.z] == match) ? uint8_t(1) : c;
    c = (sharedC[localIdx.x][localIdx.y][localIdx.z - 1] == match) ? uint8_t(1) : c;
    c = (sharedC[localIdx.x][localIdx.y][localIdx.z + 1] == match) ? uint8_t(1) : c;

    uint8_t v = sharedC[localIdx.x][localIdx.y][localIdx.z];
    if (v == 0 && c == 1) {
        c = uint8_t(p2);
        atlasWrite<uint8_t>(gvdb, channel, atlasIdx, c);
    }
}

[shader("compute")]
[numthreads(8, 8, 8)]
void gvdbOpNoise(OPERATOR_ARGUMENTS)
{
    GVDB_VOXPACKED
    LOAD_SHARED_MEMORY(float, sharedF, gvdb, channel, localIdx, atlasIdx)
    if (outside) return;

    //-- noise
    float v = sharedF[localIdx.x][localIdx.y][localIdx.z];
    if (v > 0.01) v += opRandom(float3(atlasIdx)) * p1;

    atlasWrite<float>(gvdb, channel, atlasIdx, v);
}
