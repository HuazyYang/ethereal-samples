//-----------------------------------------------------------------------------
// NVIDIA(R) GVDB VOXELS
// Copyright 2017 NVIDIA Corporation
// SPDX-License-Identifier: Apache-2.0
//
// Version 1.0: Rama Hoetzlein, 5/1/2017
//-----------------------------------------------------------------------------
// GVDB Points and topology (port of cuda_gvdb_particles.cuh and the kernels of cuda_gvdb_node.cuh)
// - topology       - gvdbLinkBricks, gvdbDelinkLeafBricks, gvdbDelinkBricks, gvdbSplitPos
// - brick keys     - gvdbCalcBrickId, gvdbCalcExtraBrickId, gvdbCalcIncreExtraBrickId, gvdbFindUnique,
//                    gvdbCompactUnique
// - points         - gvdbInsertPoints, gvdbSortPoints, gvdbScatterPointDensity, gvdbScatterPointAvgCol,
//                    gvdbConvAndTransform, gvdbScalePntPos
// - sub-cells      - gvdbSetFlagSubcell, gvdbCountSubcell, gvdbInsertSubcell(_fp16), gvdbCalcSubcellPos,
//                    gvdbGatherDensity, gvdbGatherLevelSet(_fp16)
// - voxelization   - gvdbInsertTriangles, gvdbSortTriangles, gvdbVoxelize
//
// Points come as strided arrays: (uint8_t* p, int offset, int stride) addresses point i at p + i * stride + offset.
// The 1D kernels run with 512 threads per group unless their [numthreads] says otherwise; the gathers run one 4x4x4
// group per sub-cell (sub-cells of 4 voxels).
//-----------------------------------------------------------------------------
#include "GVDB.slangh"

static const float NOHIT = 1.0e10f;

// float -> uint8_t conversions saturate on the GPU, as the reference's uchar casts do (averaged alpha exceeds 1).
#define CLR2CHAR(c)     uint8_t4(uint8_t((c).x * 255.0f), uint8_t((c).y * 255.0f), uint8_t((c).z * 255.0f), uint8_t((c).w * 255.0f))
#define CHAR2CLR(c)     (float4(c) / 255.0f)
// The colour of a uint (bytes x, y, z, w) as float4, and back.
float4 int2clr(uint c) { return float4(float(c & 0xFF), float((c >> 8) & 0xFF), float((c >> 16) & 0xFF), float((c >> 24) & 0xFF)) / 255.0f; }
uint clr2int(float4 c)
{
    // CLR2INT of the reference adds the bytes (an out-of-range channel carries into the next one).
    return uint(c.x * 255.0f) + (uint(c.y * 255.0f) << 8) + (uint(c.z * 255.0f) << 16) + (uint(c.w * 255.0f) << 24);
}

// Channel type codes of gvdbVoxelize's `otype` (the VBX codes, see gvdb::atlasFormatToVBXType).
#define T_UCHAR 0
#define T_FLOAT 3
#define T_INT   6

float3 pointAt(uint8_t* p, int offset, int stride, uint i)
{
    return *(float3*)(p + (int64_t(i) * stride + offset));
}

void fminmax3(float a, float b, float c, out float mn, out float mx)
{
    mn = mx = a;
    if (b < mn) mn = b;
    if (b > mx) mx = b;
    if (c < mn) mn = c;
    if (c > mx) mx = c;
}

// mul4x of the reference: row vector * 4x4 matrix of 16 floats.
float3 mul4x(float3 a, float* mtx)
{
    return float3(a.x * mtx[0] + a.y * mtx[4] + a.z * mtx[8] + mtx[12],
                  a.x * mtx[1] + a.y * mtx[5] + a.z * mtx[9] + mtx[13],
                  a.x * mtx[2] + a.y * mtx[6] + a.z * mtx[10] + mtx[14]);
}

//------- Topology (cuda_gvdb_node.cuh)

[shader("compute")]
[numthreads(512, 1, 1)]
void gvdbDelinkLeafBricks(uniform VDBInfo* gvdb, uint3 tid : SV_DispatchThreadID)
{
    int i = int(tid.x);
    if (i >= gvdb->nodecnt[0]) return;

    VDBNode* node = getNode(gvdb, 0, i);

    if (node->mFlags != 0) return;                // in use brick
    if (node->mParent == ID_UNDEF64) return;      // deactivated brick

    uint64_t pnodeId = getParent(gvdb, 1, node->mPos);
    VDBNode* pnode = getNode(gvdb, 1, int(pnodeId));

    int res = gvdb->res[1];
    int3 range = gvdb->noderange[1];

    int3 posInNode = node->mPos - pnode->mPos;
    posInNode *= res;
    posInNode.x /= range.x;
    posInNode.y /= range.y;
    posInNode.z /= range.z;
    int bitMaskPos = (posInNode.z * res + posInNode.y) * res + posInNode.x;

    node->mParent = ID_UNDEF64;                   // no parent = deactivated
    uint64_t listid = pnode->mChildList;
    int cndx = int(listid >> 16);
    uint64_t* clist = (uint64_t*)(gvdb->childlist[1] + uint64_t(cndx * gvdb->childwid[1]));
    clist[bitMaskPos] = ID_UNDEF64;
}

[shader("compute")]
[numthreads(512, 1, 1)]
void gvdbDelinkBricks(uniform VDBInfo* gvdb, uniform int lev, uint3 tid : SV_DispatchThreadID)
{
    int i = int(tid.x);
    if (i >= gvdb->nodecnt[lev]) return;

    VDBNode* node = getNode(gvdb, lev, i);

    if (node->mParent == ID_UNDEF64) { node->mFlags = 0; return; }   // deactivated brick

    for (int ci = 0; ci < gvdb->res[lev] * gvdb->res[lev] * gvdb->res[lev]; ci++)
    {
        uint64_t* clist = (uint64_t*)(gvdb->childlist[lev] + uint64_t(i * gvdb->childwid[lev]));
        if (clist[ci] != ID_UNDEF64) { node->mFlags = 1; return; }
    }

    node->mFlags = 0;

    uint64_t pnodeId = getParent(gvdb, lev + 1, node->mPos);

    VDBNode* pnode = getNode(gvdb, lev + 1, int(pnodeId));

    int res = gvdb->res[lev + 1];
    int3 range = gvdb->noderange[lev + 1];

    int3 posInNode = node->mPos - pnode->mPos;
    posInNode *= res;
    posInNode.x /= range.x;
    posInNode.y /= range.y;
    posInNode.z /= range.z;
    int bitMaskPos = (posInNode.z * res + posInNode.y) * res + posInNode.x;

    node->mParent = ID_UNDEF64;                   // no parent = deactivated
    uint64_t listid = pnode->mChildList;
    int cndx = int(listid >> 16);
    uint64_t* clist = (uint64_t*)(gvdb->childlist[lev + 1] + uint64_t(cndx * gvdb->childwid[lev + 1]));
    clist[bitMaskPos] = ID_UNDEF64;
}

[shader("compute")]
[numthreads(512, 1, 1)]
void gvdbLinkBricks(uniform VDBInfo* gvdb, uniform int lev, uint3 tid : SV_DispatchThreadID)
{
    int i = int(tid.x);
    if (i >= gvdb->nodecnt[lev]) return;

    VDBNode* node = getNode(gvdb, lev, i);

    if (node->mFlags == 0) return;
    uint64_t pnodeId = getParent(gvdb, lev + 1, node->mPos);
    if (pnodeId == ID_UNDEFL) return;

    VDBNode* pnode = getNode(gvdb, lev + 1, int(pnodeId));

    int res = gvdb->res[lev + 1];
    int3 range = gvdb->noderange[lev + 1];

    int3 posInNode = node->mPos - pnode->mPos;
    posInNode *= res;
    posInNode.x /= range.x;
    posInNode.y /= range.y;
    posInNode.z /= range.z;
    int bitMaskPos = (posInNode.z * res + posInNode.y) * res + posInNode.x;

    if (posInNode.x > res || posInNode.x < 0 || posInNode.y > res || posInNode.y < 0 || posInNode.z > res || posInNode.z < 0) return;

    // set mParent in node
    node->mParent = ((pnodeId << 16) | uint64_t((lev + 1) << 8));   // set parent of child

    uint64_t listid = pnode->mChildList;
    uint64_t cndx = listid >> 16;
    if (cndx >= uint64_t(gvdb->nodecnt[lev + 1])) return;
    uint64_t* clist = (uint64_t*)(gvdb->childlist[lev + 1] + cndx * uint64_t(gvdb->childwid[lev + 1]));

    clist[bitMaskPos] = ((uint64_t(i) << 16) | (uint64_t(lev) << 8));
}

[shader("compute")]
[numthreads(256, 1, 1)]
void gvdbSplitPos(uniform int num_pnts, uniform float3 trans, uniform uint8_t* ppos, uniform int pos_off,
                  uniform int pos_stride, uniform float* xout, uniform float* yout, uniform float* zout,
                  uint3 tid : SV_DispatchThreadID)
{
    int i = int(tid.x);
    if (i >= num_pnts) return;

    float3 wpos = pointAt(ppos, pos_off, pos_stride, uint(i)) + trans;

    xout[i] = wpos.x;
    yout[i] = wpos.y;
    zout[i] = wpos.z;
}

//------- Points

// NOTE(port): the reference calls __syncthreads() here after threads may have returned; there is no shared memory
// to synchronize, so the barrier is left out.
[shader("compute")]
[numthreads(512, 1, 1)]
void gvdbInsertPoints(uniform VDBInfo* gvdb, uniform int num_pnts, uniform uint8_t* ppos, uniform int pos_off,
                      uniform int pos_stride, uniform int* pnode, uniform int* poff, uniform int* gcnt,
                      uniform float3 ptrans, uint3 tid : SV_DispatchThreadID)
{
    uint i = tid.x;
    if (i >= uint(num_pnts)) return;

    float3 wpos = pointAt(ppos, pos_off, pos_stride, i);   // NOTE: +ptrans is below. Allows check for wpos.z==NOHIT

    if (wpos.z == NOHIT) { pnode[i] = int(ID_UNDEFL); return; }   // If position invalid, return.
    float3 offs, vmin;                                            // Get GVDB node at the particle point
    uint64_t nid;
    VDBNode* node = getNodeAtPoint(gvdb, wpos + ptrans, offs, vmin, nid);
    if (node == nullptr) { pnode[i] = int(ID_UNDEFL); return; }  // If no brick at location, return.

    pnode[i] = int(nid);                                          // Place point in brick
    int offset;
    InterlockedAdd(gcnt[int(nid)], 1, offset);                    // Increment brick pcount, and retrieve this point index at the same time
    poff[i] = offset;
}

[shader("compute")]
[numthreads(512, 1, 1)]
void gvdbSortPoints(uniform int num_pnts, uniform uint8_t* ppos, uniform int pos_off, uniform int pos_stride,
                    uniform int* pnode, uniform int* poff, uniform int num_nodes, uniform int* gcnt, uniform int* goff,
                    uniform float3* pout, uniform float3 ptrans, uint3 tid : SV_DispatchThreadID)
{
    uint i = tid.x;
    if (i >= uint(num_pnts)) return;

    uint64_t nid = uint64_t(int64_t(pnode[i]));
    if (nid > uint64_t(num_nodes)) return;

    int ndx = goff[int(nid)] + poff[i];   // sorted index = brick offset (for particle's nid) + particle offset in brick

    float3 wpos = pointAt(ppos, pos_off, pos_stride, i) + ptrans;
    pout[ndx] = wpos;
}

int3 getCoveringNode(float3 pos, int range)
{
    int3 nodepos;

    nodepos.x = int(ceil(pos.x / float(range)) * float(range));
    nodepos.y = int(ceil(pos.y / float(range)) * float(range));
    nodepos.z = int(ceil(pos.z / float(range)) * float(range));
    if (pos.x < float(nodepos.x)) nodepos.x -= range;
    if (pos.y < float(nodepos.y)) nodepos.y -= range;
    if (pos.z < float(nodepos.z)) nodepos.z -= range;

    return nodepos;
}

[shader("compute")]
[numthreads(512, 1, 1)]
void gvdbSetFlagSubcell(uniform VDBInfo* gvdb, uniform int num_sc, uniform int* sc_flag, uint3 tid : SV_DispatchThreadID)
{
    int i = int(tid.x);
    if (i >= num_sc) return;

    VDBNode* node = getNode(gvdb, 0, i);

    sc_flag[i] = (node->mParent == ID_UNDEF64) ? 0 : 1;
}

[shader("compute")]
[numthreads(512, 1, 1)]
void gvdbConvAndTransform(uniform int num_pnts, uniform uint8_t* psrc, uniform int8_t psrcbits, uniform uint8_t* pdest,
                          uniform int8_t pdestbits, uniform float3 wmin, uniform float3 wdelta, uniform float3 trans,
                          uniform float3 scal, uint3 tid : SV_DispatchThreadID)
{
    uint i = tid.x;
    if (i >= uint(num_pnts)) return;

    float3 pnt = float3(0, 0, 0);

    // unpack input format
    switch (psrcbits) {
    case 2: {
        uint16_t* pbuf = (uint16_t*)(psrc + int64_t(i) * 6);
        pnt = float3(float(pbuf[0]), float(pbuf[1]), float(pbuf[2]));
        } break;
    case 4: {
        float* pbuf = (float*)(psrc + int64_t(i) * 12);
        pnt = float3(pbuf[0], pbuf[1], pbuf[2]);
        } break;
    default:
        break;
    }
    // scale and transform
    pnt = (wmin + pnt * wdelta) * scal + trans;

    *(float3*)(pdest + int64_t(i) * 12) = pnt;
}

[shader("compute")]
[numthreads(512, 1, 1)]
void gvdbScalePntPos(uniform int num_pnts, uniform uint8_t* ppos, uniform int pos_off, uniform int pos_stride,
                     uniform float scale, uint3 tid : SV_DispatchThreadID)
{
    uint i = tid.x;
    if (i >= uint(num_pnts)) return;

    float3* p = (float3*)(ppos + (int64_t(i) * pos_stride + pos_off));
    *p = *p * scale;
}

//------- Sub-cells

[shader("compute")]
[numthreads(512, 1, 1)]
void gvdbInsertSubcell_fp16(uniform VDBInfo* gvdb, uniform int subcell_size, uniform int sc_per_brick, uniform int num_pnts,
                            uniform int* sc_cnt, uniform int* sc_offset, uniform int* sc_mapping,
                            uniform float3 pos_min, uniform float3 pos_range, uniform float3 vel_min, uniform float3 vel_range,
                            uniform int3 sc_range, uniform float3 ptrans, uniform int res, uniform float radius,
                            uniform uint8_t* ppos, uniform int pos_off, uniform int pos_stride, uniform uint16_t3* sc_pnt_pos,
                            uniform uint8_t* pvel, uniform int vel_off, uniform int vel_stride, uniform uint16_t3* sc_pnt_vel,
                            uniform uint8_t* pclr, uniform int clr_off, uniform int clr_stride, uniform uint* sc_pnt_clr,
                            uint3 tid : SV_DispatchThreadID)
{
    uint i = tid.x;
    if (i >= uint(num_pnts)) return;

    float3 wpos = pointAt(ppos, pos_off, pos_stride, i) + ptrans;

    if (wpos.z == NOHIT) return;                              // If position invalid, return.
    if (wpos.x < 0 || wpos.y < 0 || wpos.z < 0) return;       // robust test

    float3 wvel = (pvel == nullptr) ? float3(0, 0, 0) : pointAt(pvel, vel_off, vel_stride, i);
    uint wclr = (pclr == nullptr) ? 0 : *(uint*)(pclr + (int64_t(i) * clr_stride + clr_off));

    int scminx = (int(wpos.x - radius) / subcell_size) * subcell_size;
    int scminy = (int(wpos.y - radius) / subcell_size) * subcell_size;
    int scminz = (int(wpos.z - radius) / subcell_size) * subcell_size;
    int scmaxx = (int(wpos.x + 1 + radius) / subcell_size) * subcell_size;
    int scmaxy = (int(wpos.y + 1 + radius) / subcell_size) * subcell_size;
    int scmaxz = (int(wpos.z + 1 + radius) / subcell_size) * subcell_size;

    int3 scPos, posInNode;
    for (scPos.x = scminx; scPos.x <= scmaxx; scPos.x += subcell_size)
    {
        for (scPos.y = scminy; scPos.y <= scmaxy; scPos.y += subcell_size)
        {
            for (scPos.z = scminz; scPos.z <= scmaxz; scPos.z += subcell_size)
            {
                int pnodeId = getPosLeafParent(gvdb, scPos);
                if (pnodeId == int(ID_UNDEFL)) continue;
                VDBNode* pnode = getNode(gvdb, 0, pnodeId);

                posInNode = scPos - pnode->mPos;
                posInNode.x /= subcell_size;
                posInNode.y /= subcell_size;
                posInNode.z /= subcell_size;
                int localOffs = (posInNode.z * res + posInNode.y) * res + posInNode.x;

                int sc_idx = sc_mapping[pnodeId] * sc_per_brick + localOffs;
                int offset;
                InterlockedAdd(sc_cnt[sc_idx], 1, offset);
                sc_pnt_pos[sc_offset[sc_idx] + offset] = uint16_t3(
                    uint16_t((wpos.x - pos_min.x) / pos_range.x * 65535),
                    uint16_t((wpos.y - pos_min.y) / pos_range.y * 65535),
                    uint16_t((wpos.z - pos_min.z) / pos_range.z * 65535));
                if (pvel != nullptr) sc_pnt_vel[sc_offset[sc_idx] + offset] = uint16_t3(
                    uint16_t((wvel.x - vel_min.x) / vel_range.x * 65535),
                    uint16_t((wvel.y - vel_min.y) / vel_range.y * 65535),
                    uint16_t((wvel.z - vel_min.z) / vel_range.z * 65535));
                if (pclr != nullptr) sc_pnt_clr[sc_offset[sc_idx] + offset] = wclr;
            }
        }
    }
}

[shader("compute")]
[numthreads(512, 1, 1)]
void gvdbInsertSubcell(uniform VDBInfo* gvdb, uniform int subcell_size, uniform int sc_per_brick, uniform int num_pnts,
                       uniform int* sc_cnt, uniform int* sc_offset, uniform int* sc_mapping,
                       uniform int3 sc_range, uniform float3 ptrans, uniform int res, uniform float radius,
                       uniform uint8_t* ppos, uniform int pos_off, uniform int pos_stride, uniform float3* sc_pnt_pos,
                       uniform uint8_t* pvel, uniform int vel_off, uniform int vel_stride, uniform float3* sc_pnt_vel,
                       uniform uint8_t* pclr, uniform int clr_off, uniform int clr_stride, uniform uint* sc_pnt_clr,
                       uint3 tid : SV_DispatchThreadID)
{
    uint i = tid.x;
    if (i >= uint(num_pnts)) return;

    float3 wpos = pointAt(ppos, pos_off, pos_stride, i);
    if (wpos.z == NOHIT) return;                              // If position invalid, return.
    if (wpos.x < 0 || wpos.y < 0 || wpos.z < 0) return;       // robust test
    wpos += ptrans;                                           // add ptrans here to allow check for NOHIT

    float3 wvel = (pvel == nullptr) ? float3(0, 0, 0) : pointAt(pvel, vel_off, vel_stride, i);
    uint wclr = (pclr == nullptr) ? 0 : *(uint*)(pclr + (int64_t(i) * clr_stride + clr_off));

    int scminx = (int(wpos.x - radius) / subcell_size) * subcell_size;
    int scminy = (int(wpos.y - radius) / subcell_size) * subcell_size;
    int scminz = (int(wpos.z - radius) / subcell_size) * subcell_size;
    int scmaxx = (int(wpos.x + 1 + radius) / subcell_size) * subcell_size;
    int scmaxy = (int(wpos.y + 1 + radius) / subcell_size) * subcell_size;
    int scmaxz = (int(wpos.z + 1 + radius) / subcell_size) * subcell_size;

    int3 scPos, posInNode;
    for (scPos.x = scminx; scPos.x <= scmaxx; scPos.x += subcell_size)
    {
        for (scPos.y = scminy; scPos.y <= scmaxy; scPos.y += subcell_size)
        {
            for (scPos.z = scminz; scPos.z <= scmaxz; scPos.z += subcell_size)
            {
                int pnodeId = getPosLeafParent(gvdb, scPos);
                if (pnodeId == int(ID_UNDEFL)) continue;
                VDBNode* pnode = getNode(gvdb, 0, pnodeId);

                posInNode = scPos - pnode->mPos;
                posInNode.x /= subcell_size;
                posInNode.y /= subcell_size;
                posInNode.z /= subcell_size;
                int localOffs = (posInNode.z * res + posInNode.y) * res + posInNode.x;

                int sc_idx = sc_mapping[pnodeId] * sc_per_brick + localOffs;
                int offset;
                InterlockedAdd(sc_cnt[sc_idx], 1, offset);
                sc_pnt_pos[sc_offset[sc_idx] + offset] = wpos;
                if (pvel != nullptr) sc_pnt_vel[sc_offset[sc_idx] + offset] = wvel;
                if (pclr != nullptr) sc_pnt_clr[sc_offset[sc_idx] + offset] = wclr;
            }
        }
    }
}

[shader("compute")]
[numthreads(512, 1, 1)]
void gvdbCountSubcell(uniform VDBInfo* gvdb, uniform int subcell_size, uniform int sc_per_brick, uniform int num_pnts,
                      uniform uint8_t* ppos, uniform int pos_off, uniform int pos_stride, uniform int* sc_cnt,
                      uniform int3 sc_range, uniform float3 ptrans, uniform int res, uniform float radius,
                      uniform int num_sc, uniform int* sc_mapping, uint3 tid : SV_DispatchThreadID)
{
    uint i = tid.x;
    if (i >= uint(num_pnts)) return;

    float3 wpos = pointAt(ppos, pos_off, pos_stride, i);
    if (wpos.z == NOHIT) return;                              // If position invalid, return.
    wpos += ptrans;                                           // add here to allow check above for NOHIT
    if (wpos.x < 0 || wpos.y < 0 || wpos.z < 0) return;       // robust test

    int scminx = (int(wpos.x - radius) / subcell_size) * subcell_size;
    int scminy = (int(wpos.y - radius) / subcell_size) * subcell_size;
    int scminz = (int(wpos.z - radius) / subcell_size) * subcell_size;
    int scmaxx = (int(wpos.x + 1 + radius) / subcell_size) * subcell_size;
    int scmaxy = (int(wpos.y + 1 + radius) / subcell_size) * subcell_size;
    int scmaxz = (int(wpos.z + 1 + radius) / subcell_size) * subcell_size;

    int3 scPos, posInNode;
    for (scPos.x = scminx; scPos.x <= scmaxx; scPos.x += subcell_size)
    {
        for (scPos.y = scminy; scPos.y <= scmaxy; scPos.y += subcell_size)
        {
            for (scPos.z = scminz; scPos.z <= scmaxz; scPos.z += subcell_size)
            {
                int pnodeId = getPosLeafParent(gvdb, scPos);
                if (pnodeId == int(ID_UNDEFL)) continue;
                VDBNode* pnode = getNode(gvdb, 0, pnodeId);
                posInNode = scPos - pnode->mPos;
                posInNode.x /= subcell_size;
                posInNode.y /= subcell_size;
                posInNode.z /= subcell_size;
                int localOffs = (posInNode.z * res + posInNode.y) * res + posInNode.x;

                InterlockedAdd(sc_cnt[sc_mapping[pnodeId] * sc_per_brick + localOffs], 1);
            }
        }
    }
}

//------- Brick keys: one (lev, x, y, z) ushort key per node, sorted and made unique as 64-bit keys

[shader("compute")]
[numthreads(512, 1, 1)]
void gvdbFindUnique(uniform int num_pnts, uniform int64_t* pin, uniform int* marker, uniform int* uniqueCnt,
                    uniform int* levCnt, uint3 tid : SV_DispatchThreadID)
{
    uint i = tid.x;
    if (i >= uint(num_pnts)) return;

    if ((i == 0) || (pin[i] != pin[i - 1]))
    {
        marker[i] = 1;
        InterlockedAdd(uniqueCnt[0], 1);

        int lev = int((pin[i] >> 48) & 0xFF);
        if (lev == 255) return;

        InterlockedAdd(levCnt[lev], 1);
    }
}

[shader("compute")]
[numthreads(512, 1, 1)]
void gvdbCalcBrickId(uniform int num_pnts, uniform int lev_depth, uniform int* range_res,
                     uniform uint8_t* ppos, uniform int pos_off, uniform int pos_stride,
                     uniform float3 orig, uniform uint16_t* pout, uint3 tid : SV_DispatchThreadID)
{
    uint i = tid.x;
    if (i >= uint(num_pnts)) return;

    float3 wpos = pointAt(ppos, pos_off, pos_stride, i) + orig;

    if (wpos.x < 0 || wpos.y < 0 || wpos.z < 0)
    {
        for (int lev = 0; lev < lev_depth; lev++)
        {
            pout[i * uint(lev_depth) * 4 + uint(lev) * 4 + 3] = 255;
            pout[i * uint(lev_depth) * 4 + uint(lev) * 4 + 2] = 0;
            pout[i * uint(lev_depth) * 4 + uint(lev) * 4 + 1] = 0;
            pout[i * uint(lev_depth) * 4 + uint(lev) * 4 + 0] = 0;
        }
        return;
    }

    for (int lev = 0; lev < lev_depth; lev++)
    {
        int3 brickPos = getCoveringNode(wpos, range_res[lev]);

        pout[i * uint(lev_depth) * 4 + uint(lev) * 4 + 3] = uint16_t(lev);
        pout[i * uint(lev_depth) * 4 + uint(lev) * 4 + 2] = uint16_t(brickPos.x / range_res[lev]);
        pout[i * uint(lev_depth) * 4 + uint(lev) * 4 + 1] = uint16_t(brickPos.y / range_res[lev]);
        pout[i * uint(lev_depth) * 4 + uint(lev) * 4 + 0] = uint16_t(brickPos.z / range_res[lev]);
    }
}

[shader("compute")]
[numthreads(512, 1, 1)]
void gvdbCalcIncreExtraBrickId(uniform VDBInfo* gvdb, uniform float radius, uniform int num_pnts, uniform int lev_depth,
                               uniform int* range_res, uniform uint8_t* ppos, uniform int pos_off, uniform int pos_stride,
                               uniform float3 orig, uniform uint16_t* pout, uniform int* exBrick_cnt,
                               uniform int* node_markers, uint3 tid : SV_DispatchThreadID)
{
    uint i = tid.x;
    if (i >= uint(num_pnts)) return;

    float3 wpos = pointAt(ppos, pos_off, pos_stride, i) + orig;

    if (wpos.x < 0 || wpos.y < 0 || wpos.z < 0) return;

    // int - float in the reference: the bounds are computed in float and truncated
    int3 bmin = int3(float3(int3(wpos)) - radius);
    int3 bmax = int3(float3(int3(wpos)) + radius);

    int3 ndPos;
    for (int lev = 0; lev < lev_depth; lev++)
    {
        int rres = range_res[lev];
        for (ndPos.x = bmin.x / rres * rres; ndPos.x <= bmax.x / rres * rres; ndPos.x += rres)
        {
            for (ndPos.y = bmin.y / rres * rres; ndPos.y <= bmax.y / rres * rres; ndPos.y += rres)
            {
                for (ndPos.z = bmin.z / rres * rres; ndPos.z <= bmax.z / rres * rres; ndPos.z += rres)
                {
                    int pnodeId = int(getPosParent(gvdb, ndPos, lev));

                    if (pnodeId == int(ID_UNDEFL))
                    {
                        int idx;
                        InterlockedAdd(exBrick_cnt[0], 1, idx);
                        pout[idx * 4 + 0] = uint16_t(ndPos.z / rres);
                        pout[idx * 4 + 1] = uint16_t(ndPos.y / rres);
                        pout[idx * 4 + 2] = uint16_t(ndPos.x / rres);
                        pout[idx * 4 + 3] = uint16_t(lev);
                    } else {
                        if (lev == 0) InterlockedOr(node_markers[pnodeId], 1);
                    }
                }
            }
        }
    }
}

[shader("compute")]
[numthreads(512, 1, 1)]
void gvdbCalcExtraBrickId(uniform VDBInfo* gvdb, uniform float radius, uniform int num_pnts, uniform int lev_depth,
                          uniform int* range_res, uniform uint8_t* ppos, uniform int pos_off, uniform int pos_stride,
                          uniform float3 orig, uniform uint16_t* pout, uniform int* exBrick_cnt,
                          uint3 tid : SV_DispatchThreadID)
{
    uint i = tid.x;
    if (i >= uint(num_pnts)) return;

    float3 wpos = pointAt(ppos, pos_off, pos_stride, i) + orig;

    if (wpos.x < 0 || wpos.y < 0 || wpos.z < 0) return;

    // int - float in the reference: the bounds are computed in float and truncated
    int3 bmin = int3(float3(int3(wpos)) - radius);
    int3 bmax = int3(float3(int3(wpos)) + 2 * radius);

    int3 ndPos;
    for (int lev = 0; lev < lev_depth; lev++)
    {
        int rres = range_res[lev];
        for (ndPos.x = bmin.x / rres * rres; ndPos.x <= bmax.x / rres * rres; ndPos.x += rres)
        {
            for (ndPos.y = bmin.y / rres * rres; ndPos.y <= bmax.y / rres * rres; ndPos.y += rres)
            {
                for (ndPos.z = bmin.z / rres * rres; ndPos.z <= bmax.z / rres * rres; ndPos.z += rres)
                {
                    int pnodeId = int(getPosParent(gvdb, ndPos, lev));
                    if (pnodeId == int(ID_UNDEFL))
                    {
                        int idx;
                        InterlockedAdd(exBrick_cnt[0], 1, idx);
                        pout[idx * 4 + 0] = uint16_t(ndPos.z / rres);
                        pout[idx * 4 + 1] = uint16_t(ndPos.y / rres);
                        pout[idx * 4 + 2] = uint16_t(ndPos.x / rres);
                        pout[idx * 4 + 3] = uint16_t(lev);
                    }
                }
            }
        }
    }
}

[shader("compute")]
[numthreads(512, 1, 1)]
void gvdbCompactUnique(uniform int num_pnts, uniform int64_t* pin, uniform int* marker, uniform int* offset,
                       uniform int64_t* pout, uint3 tid : SV_DispatchThreadID)
{
    uint i = tid.x;
    if (i >= uint(num_pnts)) return;

    if (marker[i] > 0)
    {
        pout[offset[i]] = pin[i];
    }
}

//------- Scatter

float distFunc(float3 a, float bx, float by, float bz, float r)
{
    bx -= a.x; by -= a.y; bz -= a.z;
    float c = (bx * bx + by * by + bz * bz) / (r * r);
    return 1.0 + c * (-3 + c * (3 - c));
}

[shader("compute")]
[numthreads(256, 1, 1)]
void gvdbScatterPointDensity(uniform VDBInfo* gvdb, uniform int num_pnts, uniform float radius, uniform float amp,
                             uniform uint8_t* ppos, uniform int pos_off, uniform int pos_stride,
                             uniform uint8_t* pclr, uniform int clr_off, uniform int clr_stride,
                             uniform int* pnode, uniform float3 ptrans, uniform bool expand, uniform uint* colorBuf,
                             uint3 tid : SV_DispatchThreadID)
{
    uint i = tid.x;
    if (i >= uint(num_pnts)) return;
    if (pnode[i] == int(ID_UNDEFL)) return;   // make sure point is inside a brick

    // Get particle position in brick
    float3 wpos = pointAt(ppos, pos_off, pos_stride, i) + ptrans;
    float3 vmin;
    float w;
    VDBNode* node = getNode(gvdb, 0, pnode[i], vmin);   // Get node
    float3 p = wpos - vmin;
    float3 pi = float3(int3(p));

    // range of pi.x,pi.y,pi.z = [0, gvdb->res0-1]
    if (pi.x < 0 || pi.y < 0 || pi.z < 0 || pi.x >= gvdb->res[0] || pi.y >= gvdb->res[0] || pi.z >= gvdb->res[0]) return;
    uint3 q = uint3(pi) + uint3(node->mValue);

    w = atlasRead<float>(gvdb, 0, q) + distFunc(p, pi.x, pi.y, pi.z, radius);
    atlasWrite<float>(gvdb, 0, q, w);

    if (expand) {
        w = atlasRead<float>(gvdb, 0, uint3(q.x - 1, q.y, q.z)) + distFunc(p, pi.x - 1, pi.y, pi.z, radius);
        atlasWrite<float>(gvdb, 0, uint3(q.x - 1, q.y, q.z), w);
        w = atlasRead<float>(gvdb, 0, uint3(q.x + 1, q.y, q.z)) + distFunc(p, pi.x + 1, pi.y, pi.z, radius);
        atlasWrite<float>(gvdb, 0, uint3(q.x + 1, q.y, q.z), w);
        w = atlasRead<float>(gvdb, 0, uint3(q.x, q.y - 1, q.z)) + distFunc(p, pi.x, pi.y - 1, pi.z, radius);
        atlasWrite<float>(gvdb, 0, uint3(q.x, q.y - 1, q.z), w);
        w = atlasRead<float>(gvdb, 0, uint3(q.x, q.y + 1, q.z)) + distFunc(p, pi.x, pi.y + 1, pi.z, radius);
        atlasWrite<float>(gvdb, 0, uint3(q.x, q.y + 1, q.z), w);
        w = atlasRead<float>(gvdb, 0, uint3(q.x, q.y, q.z - 1)) + distFunc(p, pi.x, pi.y, pi.z - 1, radius);
        atlasWrite<float>(gvdb, 0, uint3(q.x, q.y, q.z - 1), w);
        w = atlasRead<float>(gvdb, 0, uint3(q.x, q.y, q.z + 1)) + distFunc(p, pi.x, pi.y, pi.z + 1, radius);
        atlasWrite<float>(gvdb, 0, uint3(q.x, q.y, q.z + 1), w);
    }

    if (pclr != nullptr) {
        uint8_t4 wclr = *(uint8_t4*)(pclr + (int64_t(i) * clr_stride + clr_off));

        if (colorBuf != nullptr) {
            // Increment index
            uint brickres = uint(gvdb->res[0]);
            uint vid = (brickres * brickres * brickres * uint(pnode[i])) + (brickres * brickres * uint(pi.z)) +
                       (brickres * uint(pi.y)) + uint(pi.x);
            uint colorIdx = vid * 4;

            // Store in color in the colorbuf
            InterlockedAdd(colorBuf[colorIdx + 0], 1u);
            InterlockedAdd(colorBuf[colorIdx + 1], uint(wclr.x));
            InterlockedAdd(colorBuf[colorIdx + 2], uint(wclr.y));
            InterlockedAdd(colorBuf[colorIdx + 3], uint(wclr.z));
        }
        else {
            atlasWrite<uint8_t4>(gvdb, 3, q, wclr);
        }
    }
}

[shader("compute")]
[numthreads(256, 1, 1)]
void gvdbScatterPointAvgCol(uniform VDBInfo* gvdb, uniform int num_voxels, uniform uint* colorBuf,
                            uint3 tid : SV_DispatchThreadID)
{
    uint vid = tid.x;
    if (vid >= uint(num_voxels)) return;

    uint colorIdx = vid * 4;
    uint count = colorBuf[colorIdx + 0];
    if (count > 0)
    {
        // Average color dividing by count
        uint colx = colorBuf[colorIdx + 1] / count;
        uint coly = colorBuf[colorIdx + 2] / count;
        uint colz = colorBuf[colorIdx + 3] / count;
        uint8_t4 pclr = uint8_t4(uint8_t(colx), uint8_t(coly), uint8_t(colz), uint8_t(255));

        // Get node
        uint brickres = uint(gvdb->res[0]);
        uint nid = vid / (brickres * brickres * brickres);
        float3 vmin;
        VDBNode* node = getNode(gvdb, 0, int(nid), vmin);

        // Get local 3d indices
        uint3 pi;
        pi.x = vid % (brickres);
        pi.y = vid % (brickres * brickres) / (brickres);
        pi.z = vid % (brickres * brickres * brickres) / (brickres * brickres);

        // Get global atlas index
        uint3 q = pi + uint3(node->mValue);

        atlasWrite<uint8_t4>(gvdb, 1, q, pclr);
    }
}

//------- Sub-cell positions and gathers

[shader("compute")]
[numthreads(512, 1, 1)]
void gvdbCalcSubcellPos(uniform VDBInfo* gvdb, uniform int* sc_nid, uniform int3* sc_pos, uniform int numBricks,
                        uniform int sc_dim, uniform int sc_per_brick, uniform int subcell_size, uniform int* sc_mapping,
                        uint3 tid : SV_DispatchThreadID)
{
    uint i = tid.x;
    if (i >= uint(numBricks)) return;

    VDBNode* pnode = getNode(gvdb, 0, int(i));
    if (pnode->mParent == ID_UNDEF64) return;
    int3 pos = pnode->mPos;

    for (int sc = 0; sc < sc_per_brick; sc++)
    {
        const int index = sc_mapping[i] * sc_per_brick + sc;
        sc_pos[index] = int3(pos.x + sc % sc_dim * subcell_size,
                             pos.y + (sc / sc_dim) % sc_dim * subcell_size,
                             pos.z + (sc / sc_dim / sc_dim) % sc_dim * subcell_size);
        sc_nid[index] = int(i);
    }
}

[shader("compute")]
[numthreads(4, 4, 4)]
void gvdbGatherDensity(uniform VDBInfo* gvdb, uniform int num_pnts, uniform int num_sc, uniform float radius,
                       uniform int* sc_nid, uniform int* sc_cnt, uniform int* sc_off, uniform int3* sc_pos,
                       uniform float3* sc_pnt_pos, uniform float3* sc_pnt_vel, uniform uint8_t4* sc_pnt_clr,
                       uniform int chanDensity, uniform int chanClr, uniform bool bAccumulate,
                       uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    int sc_id = int(group.x);   // current subcell ID
    if (sc_id >= num_sc) return;

    int3 wpos = sc_pos[sc_id] + int3(thread);

    VDBNode* node = getNode(gvdb, 0, sc_nid[sc_id]);
    float3 vmin = float3(node->mPos);
    int3 vox = node->mValue + int3(float3(wpos) - vmin);

    float3 jpos;
    float4 clr = float4(0, 0, 0, 1);
    float val = 0, c = 0.0f;

    if (bAccumulate) {
        val = atlasRead<float>(gvdb, uint(chanDensity), uint3(vox));
        if (sc_pnt_clr != nullptr) {
            clr = CHAR2CLR(atlasRead<uint8_t4>(gvdb, uint(chanClr), uint3(vox)));
        }
    }
    for (int j = 0; j < sc_cnt[sc_id]; j++) {
        jpos = sc_pnt_pos[sc_off[sc_id] + j] - float3(wpos);
        c = sqrt(jpos.x * jpos.x + jpos.y * jpos.y + jpos.z * jpos.z);
        val = max(val, radius - c);
        if (sc_pnt_clr != nullptr) clr += CHAR2CLR(sc_pnt_clr[sc_off[sc_id] + j]);
    }
    atlasWrite<float>(gvdb, uint(chanDensity), uint3(vox), val);

    if (sc_pnt_clr != nullptr) {
        clr /= float(sc_cnt[sc_id] + (bAccumulate ? 1 : 0));
        atlasWrite<uint8_t4>(gvdb, uint(chanClr), uint3(vox), CLR2CHAR(clr));
    }
}

[shader("compute")]
[numthreads(4, 4, 4)]
void gvdbGatherLevelSet(uniform VDBInfo* gvdb, uniform int num_pnts, uniform int num_sc, uniform float radius,
                        uniform int* sc_nid, uniform int* sc_cnt, uniform int* sc_off, uniform int3* sc_pos,
                        uniform float3* sc_pnt_pos, uniform float3* sc_pnt_vel, uniform uint8_t4* sc_pnt_clr,
                        uniform int chanLevelset, uniform int chanClr, uniform bool bAccumulate,
                        uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    int sc_id = int(group.x);   // current subcell ID
    if (sc_id >= num_sc) return;

    int3 wpos = sc_pos[sc_id] + int3(thread);

    VDBNode* node = getNode(gvdb, 0, sc_nid[sc_id]);
    float3 vmin = float3(node->mPos);
    int3 vox = node->mValue + int3(float3(wpos) - vmin);

    float3 jpos;
    float4 clr = float4(0, 0, 0, 1);
    float dist = 3.0f, c = 0.0f;

    if (bAccumulate) {
        dist = atlasRead<float>(gvdb, uint(chanLevelset), uint3(vox));
        if (sc_pnt_clr != nullptr) {
            clr = CHAR2CLR(atlasRead<uint8_t4>(gvdb, uint(chanClr), uint3(vox)));
        }
    }
    for (int j = 0; j < sc_cnt[sc_id]; j++) {
        jpos = sc_pnt_pos[sc_off[sc_id] + j] - float3(wpos);
        c = sqrt(jpos.x * jpos.x + jpos.y * jpos.y + jpos.z * jpos.z);
        dist = min(dist, c - radius);
        if (sc_pnt_clr != nullptr) clr += CHAR2CLR(sc_pnt_clr[sc_off[sc_id] + j]);
    }
    atlasWrite<float>(gvdb, uint(chanLevelset), uint3(vox), dist);

    if (sc_pnt_clr != nullptr) {
        clr /= float(sc_cnt[sc_id] + (bAccumulate ? 1 : 0));
        atlasWrite<uint8_t4>(gvdb, uint(chanClr), uint3(vox), CLR2CHAR(clr));
    }
}

[shader("compute")]
[numthreads(4, 4, 4)]
void gvdbGatherLevelSet_fp16(uniform VDBInfo* gvdb, uniform int num_pnts, uniform int num_sc, uniform float radius,
                             uniform float3 pos_min, uniform float3 pos_range, uniform float3 vel_min, uniform float3 vel_range,
                             uniform int* sc_nid, uniform int* sc_cnt, uniform int* sc_off, uniform int3* sc_pos,
                             uniform uint16_t3* sc_pnt_pos, uniform uint16_t3* sc_pnt_vel, uniform uint* sc_pnt_clr,
                             uniform int chanLevelset, uniform int chanClr,
                             uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    int sc_id = int(group.x);   // current subcell ID
    if (sc_id >= num_sc) return;

    int3 wpos = sc_pos[sc_id] + int3(thread);

    VDBNode* node = getNode(gvdb, 0, sc_nid[sc_id]);
    float3 vmin = float3(node->mPos);
    float3 vdel = gvdb->vdel[node->mLev];
    int3 vox = node->mValue + int3((float3(wpos) - vmin) / vdel);

    float3 jpos, tmppos;
    float4 clr = float4(0, 0, 0, 0);
    float c = 0.0f;
    float dist = 3.0f;

    for (int j = 0; j < sc_cnt[sc_id]; j++) {
        const uint16_t3 packed = sc_pnt_pos[sc_off[sc_id] + j];
        tmppos = float3(packed) / 65535.0f * pos_range + pos_min;

        jpos = tmppos - float3(wpos);
        c = sqrt(jpos.x * jpos.x + jpos.y * jpos.y + jpos.z * jpos.z);
        dist = min(dist, c - radius);
        if (sc_pnt_clr != nullptr) clr += int2clr(sc_pnt_clr[sc_off[sc_id] + j]) / c;
    }

    atlasWrite<float>(gvdb, uint(chanLevelset), uint3(vox), dist);

    // The reference writes the packed colour (a uint) with the stride of a float.
    if (sc_pnt_clr != nullptr)
        atlasWrite<uint>(gvdb, uint(chanClr), uint3(vox), clr2int(clr));
}

//------- Voxelization (triangles binned by y, then a solid voxelization per node)

[shader("compute")]
[numthreads(512, 1, 1)]
void gvdbInsertTriangles(uniform float bdiv, uniform int bmax, uniform int* bcnt, uniform int vcnt, uniform int ecnt,
                         uniform float3* vbuf, uniform int* ebuf, uniform float* xform, uint3 tid : SV_DispatchThreadID)
{
    uint n = tid.x;
    if (n >= uint(ecnt)) return;

    // get transformed triangle
    float3 v0, v1, v2;
    int3 f = int3(ebuf[n * 3], ebuf[n * 3 + 1], ebuf[n * 3 + 2]);
    v0 = vbuf[f.x]; v0 = mul4x(v0, xform);
    v1 = vbuf[f.y]; v1 = mul4x(v1, xform);
    v2 = vbuf[f.z]; v2 = mul4x(v2, xform);

    // compute bounds on y-axis
    float p0, p1;
    fminmax3(v0.y, v1.y, v2.y, p0, p1);
    p0 = float(int(p0 / bdiv)); p1 = float(int(p1 / bdiv));   // y-min and y-max bins

    // If either of these conditions are true, the triangle must be outside of the volume.
    if (p0 >= bmax) return;
    if (p1 < 0) return;
    // Clamp bounds to lie within allocated bins.
    if (p1 >= bmax) p1 = float(bmax - 1);
    if (p0 < 0) p0 = 0;

    // scan bins covered by triangle
    for (int y = int(p0); y <= int(p1); y++) {
        InterlockedAdd(bcnt[y], 1);   // histogram bin counts
    }
}

// Sort triangles: given a list of bins and known offsets (prefixes), and a list of vertices and faces, performs a
// deep copy of the transformed triangles into bins, where some may be duplicated.
//   bdiv, bmax - bins division and maximum number
//   bcnt       - number of triangles in each bin
//   boff       - starting offset of each bin in the triangle buffer
//   tbuf       - triangle buffer: the bins and their triangles (more than ecnt due to overlaps)
//   vcnt, vbuf - vertex buffer and number of vertices
//   ecnt, ebuf - element buffer and number of faces
[shader("compute")]
[numthreads(512, 1, 1)]
void gvdbSortTriangles(uniform float bdiv, uniform int bmax, uniform int* bcnt, uniform int* boff, uniform int tricnt,
                       uniform float3* tbuf, uniform int vcnt, uniform int ecnt, uniform float3* vbuf, uniform int* ebuf,
                       uniform float* xform, uint3 tid : SV_DispatchThreadID)
{
    uint n = tid.x;
    if (n >= uint(ecnt)) return;

    // get transformed triangle
    float3 v0, v1, v2;
    int3 f = int3(ebuf[n * 3], ebuf[n * 3 + 1], ebuf[n * 3 + 2]);
    v0 = vbuf[f.x]; v0 = mul4x(v0, xform);
    v1 = vbuf[f.y]; v1 = mul4x(v1, xform);
    v2 = vbuf[f.z]; v2 = mul4x(v2, xform);

    // compute bounds on y-axis
    float p0, p1;
    fminmax3(v0.y, v1.y, v2.y, p0, p1);
    p0 = float(int(p0 / bdiv)); p1 = float(int(p1 / bdiv));   // y-min and y-max bins

    // If either of these conditions are true, the triangle must be outside of the volume.
    if (p0 >= bmax) return;
    if (p1 < 0) return;
    // Clamp bounds to lie within allocated bins.
    if (p1 >= bmax) p1 = float(bmax - 1);
    if (p0 < 0) p0 = 0;

    // scan bins covered by triangle
    for (int y = int(p0); y <= int(p1); y++) {
        int bndx;
        InterlockedAdd(bcnt[y], 1, bndx);   // get bin index (and histogram bin counts)
        bndx += boff[y];                     // get offset into triangle buffer (tbuf)
        tbuf[bndx * 3] = v0;                 // deep copy transformed vertices of face
        tbuf[bndx * 3 + 1] = v1;
        tbuf[bndx * 3 + 2] = v2;
    }
}

// Round-toward-zero arithmetic of the watertight ray-triangle test (CUDA intrinsics: the test depends on it).
float fmulRz(float a, float b)
{
    __target_switch
    {
    case cuda:
        __intrinsic_asm "__fmul_rz($0, $1)";
    }
}

float faddRz(float a, float b)
{
    __target_switch
    {
    case cuda:
        __intrinsic_asm "__fadd_rz($0, $1)";
    }
}

void writeVoxel(uint8_t* obuf, uint8_t otype, uint index, float value)
{
    switch (otype) {
    case T_UCHAR: obuf[index] = uint8_t(value); break;
    case T_FLOAT: ((float*)obuf)[index] = value; break;
    case T_INT:   ((int*)obuf)[index] = int(value); break;
    default: break;
    }
}

[shader("compute")]
[numthreads(8, 8, 8)]
void gvdbVoxelize(uniform float3 vmin, uniform float3 vmax, uniform int3 res, uniform uint8_t* obuf,
                  uniform uint8_t otype, uniform float val_surf, uniform float val_inside, uniform float bdiv,
                  uniform int bmax, uniform int* bcnt, uniform int* boff, uniform float3* tbuf,
                  uint3 t : SV_DispatchThreadID)
{
    if (t.x >= uint(res.x) || t.y >= uint(res.y) || t.z >= uint(res.z)) return;
    const uint index = (t.z * uint(res.y) + t.y) * uint(res.x) + t.x;

    // solid voxelization
    float3 tdel = (vmax - vmin) / float3(res);                       // width of voxel
    float3 center = vmin + float3(t.x + .5f, t.y + .5f, t.z + .5f) * tdel;   // center of voxel
    float3 v0, v1, v2;
    float3 e0, e1, e2;
    float3 norm, p;
    float rad;
    int n, cnt = 0;
    int b = int(center.y / bdiv);
    if (b >= bmax) b = bmax - 1;

    for (n = boff[b]; n < boff[b] + bcnt[b]; n++) {
        // This snapping to grid emulates a fixed-point rasterizer, and avoids the case where our ray-triangle
        // intersection ray passes exactly through a triangle vertex.
        // Get vertex; scale voxel to [-0.5,0.5]^3; snap to grid
        v0 = tbuf[n * 3 + 0]; v0 = (v0 - center) / tdel; v0.y += exp2(-6.0f) * float(v0.y == floor(v0.y));
        v1 = tbuf[n * 3 + 1]; v1 = (v1 - center) / tdel; v1.y += exp2(-6.0f) * float(v1.y == floor(v1.y));
        v2 = tbuf[n * 3 + 2]; v2 = (v2 - center) / tdel; v2.y += exp2(-6.0f) * float(v2.y == floor(v2.y));

        //--- bounding box test
        fminmax3(v0.y, v1.y, v2.y, p.x, p.y);
        if (p.x > 0.5f || p.y < -0.5f) continue;
        fminmax3(v0.z, v1.z, v2.z, p.x, p.y);
        if (p.x > 0.5f || p.y < -0.5f) continue;
        fminmax3(v0.x, v1.x, v2.x, p.x, p.y);
        if (p.y < -0.5f) continue;              // x- half space, keep x+ half space

        //--- ray-triangle intersection
        //--- intersect all triangles with ray O + Dt = (0,0,0) + (1,0,0)t, t > 0
        // Based on the watertight intersection test from Woop, Benthin, and Wald: http://jcgt.org/published/0002/01/05/
        // Used space: norm (norm.x = U, norm.y = V, norm.z = W), rad (= T)

        // Prevent the compiler from fusing these into MAD operations
        norm.x = faddRz(fmulRz(v2.y, v1.z), -fmulRz(v2.z, v1.y));   // U = DOT(CROSS(v2, v1), D)  norm.x <=> U
        norm.y = faddRz(fmulRz(v0.y, v2.z), -fmulRz(v0.z, v2.y));   // V = DOT(CROSS(v0, v2), D)  norm.y <=> V
        norm.z = faddRz(fmulRz(v1.y, v0.z), -fmulRz(v1.z, v0.y));   // W = DOT(CROSS(v1, v0), D)  norm.z <=> W

        // Now, test to see whether our point is inside or on the edges of the triangle.
        if ((norm.x >= 0.0f && norm.y >= 0.0f && norm.z >= 0.0f) || (norm.x <= 0.0f && norm.y <= 0.0f && norm.z <= 0.0f)) {

            rad = fmulRz(norm.x, v0.x) + fmulRz(norm.y, v1.x) + fmulRz(norm.z, v2.x);   // rad <=> T
            p.y = norm.x + norm.y + norm.z;                                              // p.y <=> det

            // Check hit distance (T/det) - ignore coplanar triangles and triangles behind where the ray starts:
            if (rad * p.y > 0.0f) {
                if ((norm.x != 0.0f) && (norm.y != 0.0f) && (norm.z != 0.0f))
                {
                    cnt += 1;   // Inside the triangle
                }
                else {
                    // D3D10-style top-left rule for oriented triangles. Uses the fact that the ray never passes
                    // through a mesh vertex.
                    if (norm.x == 0.0f) {
                        // Edge 1, 2
                        e0.x = v2.z - v1.z;
                        e0.y = v2.y - v1.y;
                    }
                    else if (norm.y == 0.0f) {
                        // Edge 2, 0
                        e0.x = v0.z - v2.z;
                        e0.y = v0.y - v2.y;
                    }
                    else { // norm.z == 0.0f
                        // Edge 0, 1
                        e0.x = v1.z - v0.z;
                        e0.y = v1.y - v0.y;
                    }

                    // This will be positive if the edge vi.zy, vj.zy is in lexicographic order, negative if in
                    // reverse lexicographic order, and 0 if vi.zy == vj.zy.
                    e0.x = 2.0f * (float(e0.x > 0.0f) - float(0.0f > e0.x)) + (float(e0.y > 0.0f) - float(0.0f > e0.y));
                    if (p.y * e0.x > 0.0f) {
                        cnt += 1;
                    }
                }
            }
        }

        // For conservative triangle tests, only consider triangles with AABBs containing this voxel:
        if (p.x > 0.5f) continue;   // x+ half space

        // set e0 and e1 to their normal values
        e0 = v1 - v0; e1 = v2 - v0;

        //--- fast box-plane test
        e2 = -e1; e1 = v2 - v1;
        norm = cross(e0, e1);
        p.x = 0; p.y = 0;
        if (norm.x > 0.0f) { p.x += norm.x * (-0.5f - v0.x); p.y += norm.x * (0.5f - v0.x); }
        else               { p.x += norm.x * (0.5f - v0.x);  p.y += norm.x * (-0.5f - v0.x); }
        if (norm.y > 0.0f) { p.x += norm.y * (-0.5f - v0.y); p.y += norm.y * (0.5f - v0.y); }
        else               { p.x += norm.y * (0.5f - v0.y);  p.y += norm.y * (-0.5f - v0.y); }
        if (norm.z > 0.0f) { p.x += norm.z * (-0.5f - v0.z); p.y += norm.z * (0.5f - v0.z); }
        else               { p.x += norm.z * (0.5f - v0.z);  p.y += norm.z * (-0.5f - v0.z); }
        if (p.x > 0.0f) continue;   // do not overlap
        if (p.y < 0.0f) continue;

        //--- schwarz-seidel tests
        rad = (norm.z >= 0) ? 1 : -1;
        p = float3(-e0.y * rad, e0.x * rad, 0);
        if (-(p.x + p.y) * 0.5f - (p.x * v0.x + p.y * v0.y) + max(0.0f, p.x) + max(0.0f, p.y) < 0) continue;   // no overlap
        p = float3(-e1.y * rad, e1.x * rad, 0);
        if (-(p.x + p.y) * 0.5f - (p.x * v1.x + p.y * v1.y) + max(0.0f, p.x) + max(0.0f, p.y) < 0) continue;
        p = float3(-e2.y * rad, e2.x * rad, 0);
        if (-(p.x + p.y) * 0.5f - (p.x * v2.x + p.y * v2.y) + max(0.0f, p.x) + max(0.0f, p.y) < 0) continue;

        rad = (norm.y >= 0) ? -1 : 1;
        p = float3(-e0.z * rad, 0, e0.x * rad);
        if (-(p.x + p.z) * 0.5f - (p.x * v0.x + p.z * v0.z) + max(0.0f, p.x) + max(0.0f, p.z) < 0) continue;   // no overlap
        p = float3(-e1.z * rad, 0, e1.x * rad);
        if (-(p.x + p.z) * 0.5f - (p.x * v1.x + p.z * v1.z) + max(0.0f, p.x) + max(0.0f, p.z) < 0) continue;
        p = float3(-e2.z * rad, 0, e2.x * rad);
        if (-(p.x + p.z) * 0.5f - (p.x * v2.x + p.z * v2.z) + max(0.0f, p.x) + max(0.0f, p.z) < 0) continue;

        rad = (norm.x >= 0) ? 1 : -1;
        p = float3(0, -e0.z * rad, e0.y * rad);
        if (-(p.y + p.z) * 0.5f - (p.y * v0.y + p.z * v0.z) + max(0.0f, p.y) + max(0.0f, p.z) < 0) continue;   // no overlap
        p = float3(0, -e1.z * rad, e1.y * rad);
        if (-(p.y + p.z) * 0.5f - (p.y * v1.y + p.z * v1.z) + max(0.0f, p.y) + max(0.0f, p.z) < 0) continue;
        p = float3(0, -e2.z * rad, e2.y * rad);
        if (-(p.y + p.z) * 0.5f - (p.y * v2.y + p.z * v2.z) + max(0.0f, p.y) + max(0.0f, p.z) < 0) continue;

        writeVoxel(obuf, otype, index, val_surf);
        break;
    }

    if (n == boff[b] + bcnt[b]) {
        // solid voxelization
        if ((cnt % 2) == 1) {
            writeVoxel(obuf, otype, index, val_inside);
        }
    }
}
