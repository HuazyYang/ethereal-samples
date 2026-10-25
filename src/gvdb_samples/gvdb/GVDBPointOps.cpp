// IGVDBPointOps: point clouds -> topology / voxels. Port of the point and GPU
// topology members of VolumeGVDB (gvdb_volume_gvdb.cpp: RebuildTopology,
// AccumulateTopology, ActivateBricksGPU, ActivateExtraBricksGPU,
// ActivateIncreBricksGPU, FindUniqueBrick, RadixSortByByte, GetBoundingBox,
// GetMinMaxVel, InsertPointsSubcell(_FP16), GatherDensity, GatherLevelSet(_FP16),
// InsertPoints, ScatterDensity, ConvertAndTransform, ScalePntPos, PrefixSum).
//
// Everything goes through the public IGVDBVolume interface; the kernels are
// the ones of shaders/GVDBParticles.slang (points, sub-cells and the topology
// kernels gvdbLinkBricks, gvdbDelink*, gvdbSplitPos), loaded through
// IGVDBVolume::getKernel(), and the primitives of GVDBPrimitives.h (prefix
// sum, the 64-bit key sort and the min/max that the reference took from Thrust).
//
// Deviations from the reference are marked NOTE(port).
#include <gvdb/GVDB.h>
#include "GVDBPrimitives.h"
#include <donut/core/log.h>
#include <nvrhi/core/autoptr.h>
#include <vector>
#include <cstring>
#include <algorithm>

namespace gvdb {

using namespace donut;

namespace {

// The AUX_* buffers of the reference VolumeGVDB that the point operations use.
enum AuxId {
    AUX_PNODE,              // brick index of each point (insertPoints)
    AUX_PNDX,               // index of the point inside its brick
    AUX_GRIDCNT,            // points per brick
    AUX_GRIDOFF,            // prefix sum of AUX_GRIDCNT
    AUX_PNTSORT,            // points sorted by brick (float3)
    AUX_COLAVG,             // scatterDensity colour accumulation (4 uints per voxel)
    AUX_SUBCELL_FLAG,       // per brick: 1 if the brick has a parent
    AUX_SUBCELL_MAPPING,    // prefix sum of AUX_SUBCELL_FLAG (brick -> compact brick index)
    AUX_SUBCELL_CNT,        // points per sub-cell
    AUX_SUBCELL_PREFIXSUM,  // prefix sum of AUX_SUBCELL_CNT
    AUX_SUBCELL_PNT_POS,    // points binned by sub-cell (float3 / ushort3)
    AUX_SUBCELL_PNT_VEL,
    AUX_SUBCELL_PNT_CLR,
    AUX_SUBCELL_POS,        // index-space origin of each sub-cell (int3)
    AUX_SUBCELL_NID,        // brick index of each sub-cell
    AUX_WORLD_POS_X,        // split coordinates for the min/max reductions
    AUX_WORLD_POS_Y,
    AUX_WORLD_POS_Z,
    AUX_RANGE_RES,          // voxels per node side per level (int[rootLev])
    AUX_BRICK_LEVXYZ,       // (lev, x, y, z) ushort keys of the covering nodes
    AUX_SORTED_LEVXYZ,      // same, sorted as 64-bit keys
    AUX_MARKER,             // 1 at the first key of each run
    AUX_MARKER_PRESUM,      // prefix sum of AUX_MARKER
    AUX_UNIQUE_CNT,         // number of unique keys
    AUX_LEVEL_CNT,          // unique keys per level
    AUX_UNIQUE_LEVXYZ,      // compacted unique keys
    AUX_EXTRA_BRICK_CNT,    // number of keys written by the extra / incremental kernels
    AUX_NODE_MARKER,        // per brick: active flag (incremental path)
    MAX_AUX
};

// The sub-cell size the gather kernels are compiled for ([numthreads(4, 4, 4)]).
constexpr int SUBCELL_SIZE = 4;

class GVDBPointOps : public nvrhi::ObjectImpl<IGVDBPointOps> {
 public:
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(GVDBPointOps)
    NVRHI_IMPLEMENTS_INTERFACE(IGVDBPointOps)
    NVRHI_END_INTERFACE_TABLE()

    explicit GVDBPointOps(IGVDBVolume *volume)
        : m_volume(volume), m_context(volume->getContext()), m_primitives(volume->getContext()) {}

    IGVDBVolume *getVolume() const override { return m_volume; }

    void setPoints(const PointData &pos, const PointData &vel, const PointData &clr) override;
    dm::box3 computePointBounds(uint32_t numPnts, dm::float3 trans) override;

    void rebuildTopology(uint32_t numPnts, float radius, dm::float3 origin) override;
    void accumulateTopology(uint32_t numPnts, float radius, dm::float3 origin, int minDepth) override;
    void requestFullRebuild(bool rebuild) override { m_rebuildTopo = rebuild; }

    void insertPointsSubcell(int subcellSize, uint32_t numPnts, float radius, dm::float3 trans,
                             int &scPntsLength) override;
    void insertPointsSubcellFP16(int subcellSize, uint32_t numPnts, float radius, dm::float3 trans,
                                 int &scPntsLength) override;
    void gatherDensity(int subcellSize, uint32_t numPnts, float radius, dm::float3 trans, int scPntsLength,
                       int chanDensity, int chanClr, bool accumulate) override;
    void gatherLevelSet(int subcellSize, uint32_t numPnts, float radius, dm::float3 trans, int scPntsLength,
                        int chanLevelSet, int chanClr, bool accumulate) override;
    void gatherLevelSetFP16(int subcellSize, uint32_t numPnts, float radius, dm::float3 trans, int scPntsLength,
                            int chanLevelSet, int chanClr) override;
    void insertPoints(uint32_t numPnts, dm::float3 trans, bool prefix) override;
    void scatterDensity(uint32_t numPnts, float radius, float amp, dm::float3 trans, bool expand,
                        bool avgColor) override;
    void convertAndTransform(const PointData &src, int srcBits, const PointData &dst, int dstBits,
                             uint32_t numPnts, dm::float3 wMin, dm::float3 wDelta, dm::float3 trans,
                             dm::float3 scale) override;
    void scalePointPositions(uint32_t numPnts, float scale) override;

 private:
    // ---- buffers ----
    nvrhi::IBuffer *prepareAux(AuxId id, uint64_t count, uint32_t stride, bool zero);
    void releaseAux(AuxId id);
    nvrhi::IBuffer *aux(AuxId id) { return m_aux[id].Get(); }
    uint64_t auxAddress(AuxId id) { return deviceAddress(m_aux[id]); }
    template <typename T>
    T readbackValue(nvrhi::IBuffer *src, uint64_t index) {
        T v{};
        m_context->readBuffer(src, index * sizeof(T), &v, sizeof(T));
        return v;
    }
    void upload(nvrhi::IBuffer *dst, const void *data, uint64_t bytes) {
        m_context->getCommandList()->writeBuffer(dst, data, bytes, 0);
    }

    // ---- launches ----
    // Records a launch of one of the volume's kernels; the arguments as the kernel declares them.
    template <typename... Args>
    void launch(const char *kernel, dm::uint3 groups, const Args &...args) {
        launchKernel(m_context->getCommandList(), m_volume->getKernel(kernel), groups, args...);
    }
    // One thread per element.
    template <typename... Args>
    void launch1D(const char *kernel, uint64_t count, const Args &...args) {
        IComputeKernel *k = m_volume->getKernel(kernel);
        launchKernel(m_context->getCommandList(), k, groupsFor(k, count), args...);
    }
    uint64_t vdbInfo() { return deviceAddress(m_volume->getVDBInfoGPU()); }
    // The (char* p, int offset, int stride) arguments of a point array.
    static uint64_t ptr(const PointData &p) { return deviceAddress(p.buffer); }
    static int off(const PointData &p) { return p.buffer ? int(p.offset) : 0; }
    static int stride(const PointData &p) { return p.buffer ? int(p.stride) : 0; }
    static bool checkSubcellSize(int subcellSize, const char *what);

    // ---- algorithms ----
    void computeVelBounds(uint32_t numPnts);
    dm::int3 coveringNodePos(int lev, dm::int3 pos) const;
    int determineDepth(dm::int3 &rootPos) const;
    void prepareRangeRes(int rootLev);
    void radixSortByByte(uint32_t numKeys);
    void findUniqueBrick(uint32_t numKeys, int levDepth, int &numUnique);
    void allocateUniqueNodes(int rootLev, int numUnique);
    void linkBricks(int rootLev);
    void activateBricksGPU(uint32_t numPnts, float radius, dm::float3 orig, int rootLev, dm::int3 rootPos);
    void activateExtraBricksGPU(uint32_t numPnts, float radius, dm::float3 orig, int rootLev);
    void activateIncreBricksGPU(uint32_t numPnts, float radius, dm::float3 orig, int rootLev, bool accum);
    bool prepareSubcells(int subcellSize, uint32_t numPnts, float radius, dm::float3 trans, int &scPntsLength,
                         int &scPerBrick, int &numSCell, int &scDim, dm::int3 &range, int &numSCellMapping);
    void finishSubcells(int subcellSize, int scPerBrick, int scDim, int numSCell, int numSCellMapping);

    nvrhi::AutoPtr<IGVDBVolume> m_volume;
    nvrhi::AutoPtr<IComputeContext> m_context;
    GVDBPrimitives m_primitives;

    PointData m_pos, m_vel, m_clr;
    nvrhi::BufferHandle m_aux[MAX_AUX];

    bool m_rebuildTopo = true;
    int m_currDepth = -1;
    dm::float3 m_posMin = dm::float3::zero(), m_posMax = dm::float3::zero(), m_posRange = dm::float3{1.f};
    dm::float3 m_velMin = dm::float3::zero(), m_velMax = dm::float3::zero(), m_velRange = dm::float3{1.f};
    int m_numSubcells = 0;      // sub-cells of the last insertPointsSubcell*() (used by the gathers)
};

// ============================================================================
// Buffers
// ============================================================================

// NOTE(port): a buffer is reused while it is large enough (the reference reallocated on every stride change).
nvrhi::IBuffer *GVDBPointOps::prepareAux(AuxId id, uint64_t count, uint32_t stride, bool zero) {
    uint64_t bytes = std::max<uint64_t>(count * stride, 4);
    bytes = (bytes + 3) & ~uint64_t(3);   // clearBufferUInt needs a multiple of 4
    m_aux[id] = GVDBPrimitives::ensureBuffer(m_context, m_aux[id], bytes, "GVDB point ops");
    nvrhi::IBuffer *buffer = m_aux[id];
    if (zero && buffer) m_context->getCommandList()->clearBufferUInt(buffer, 0);
    return buffer;
}

void GVDBPointOps::releaseAux(AuxId id) {
    m_context->releaseAfterUse(m_aux[id].Get());
    m_aux[id] = nullptr;
}

bool GVDBPointOps::checkSubcellSize(int subcellSize, const char *what) {
    if (subcellSize == SUBCELL_SIZE) return true;
    log::error("GVDBPointOps::%s: sub-cells of %d voxels are not supported (the gather kernels run %d^3 threads)", what,
               subcellSize, SUBCELL_SIZE);
    return false;
}

// ============================================================================
// Points and bounds
// ============================================================================

void GVDBPointOps::setPoints(const PointData &pos, const PointData &vel, const PointData &clr) {
    m_pos = pos;
    m_vel = vel;
    m_clr = clr;
    if (!vel.buffer) releaseAux(AUX_SUBCELL_PNT_VEL);
    if (!clr.buffer) releaseAux(AUX_SUBCELL_PNT_CLR);
}

// Reference GetBoundingBox: split the translated positions per axis, reduce
// min/max, pad by -1 / +2 voxels and clamp to >= 0. Also keeps the result in
// m_posMin / m_posRange for the FP16 paths.
dm::box3 GVDBPointOps::computePointBounds(uint32_t numPnts, dm::float3 trans) {
    if (!m_pos.buffer || numPnts == 0) {
        log::warning("GVDBPointOps::computePointBounds: no points");
        return dm::box3(dm::float3::zero(), dm::float3::zero());
    }
    nvrhi::IBuffer *x = prepareAux(AUX_WORLD_POS_X, numPnts, sizeof(float), false);
    nvrhi::IBuffer *y = prepareAux(AUX_WORLD_POS_Y, numPnts, sizeof(float), false);
    nvrhi::IBuffer *z = prepareAux(AUX_WORLD_POS_Z, numPnts, sizeof(float), false);

    launch1D("gvdbSplitPos", numPnts, int(numPnts), trans, ptr(m_pos), off(m_pos), stride(m_pos), deviceAddress(x),
             deviceAddress(y), deviceAddress(z));

    float mn[3], mx[3];
    m_primitives.minMax(x, numPnts, mn[0], mx[0]);
    m_primitives.minMax(y, numPnts, mn[1], mx[1]);
    m_primitives.minMax(z, numPnts, mn[2], mx[2]);

    m_posMin = dm::max(dm::float3{mn[0], mn[1], mn[2]} - 1.f, dm::float3::zero());
    m_posMax = dm::max(dm::float3{mx[0], mx[1], mx[2]} + 2.f, dm::float3::zero());
    m_posRange = m_posMax - m_posMin;
    return dm::box3(m_posMin, m_posMax);
}

// Reference GetMinMaxVel (no padding, no clamping).
void GVDBPointOps::computeVelBounds(uint32_t numPnts) {
    if (!m_vel.buffer || numPnts == 0) {
        m_velMin = m_velMax = dm::float3::zero();
        m_velRange = dm::float3{1.f};
        return;
    }
    nvrhi::IBuffer *x = prepareAux(AUX_WORLD_POS_X, numPnts, sizeof(float), false);
    nvrhi::IBuffer *y = prepareAux(AUX_WORLD_POS_Y, numPnts, sizeof(float), false);
    nvrhi::IBuffer *z = prepareAux(AUX_WORLD_POS_Z, numPnts, sizeof(float), false);

    launch1D("gvdbSplitPos", numPnts, int(numPnts), dm::float3::zero(), ptr(m_vel), off(m_vel), stride(m_vel),
             deviceAddress(x), deviceAddress(y), deviceAddress(z));

    float mn[3], mx[3];
    m_primitives.minMax(x, numPnts, mn[0], mx[0]);
    m_primitives.minMax(y, numPnts, mn[1], mx[1]);
    m_primitives.minMax(z, numPnts, mn[2], mx[2]);
    m_velMin = dm::float3{mn[0], mn[1], mn[2]};
    m_velMax = dm::float3{mx[0], mx[1], mx[2]};
    m_velRange = m_velMax - m_velMin;
    // NOTE(port): a zero range would divide by zero in gvdbInsertSubcell_fp16.
    if (m_velRange.x == 0.f) m_velRange.x = 1.f;
    if (m_velRange.y == 0.f) m_velRange.y = 1.f;
    if (m_velRange.z == 0.f) m_velRange.z = 1.f;
}

// ============================================================================
// GPU topology
// ============================================================================

// Reference GetCoveringNode: origin of the level-`lev` node covering pos.
dm::int3 GVDBPointOps::coveringNodePos(int lev, dm::int3 pos) const {
    int range = int(m_volume->getRange(int8_t(lev)));
    dm::int3 nodepos = (pos / range) * range;
    if (pos.x < nodepos.x) nodepos.x -= range;
    if (pos.y < nodepos.y) nodepos.y -= range;
    if (pos.z < nodepos.z) nodepos.z -= range;
    return nodepos;
}

// Reference DetermineDepth: the lowest level whose node covers the whole point
// bounds. Returns -1 when no level of the configuration does.
int GVDBPointOps::determineDepth(dm::int3 &rootPos) const {
    const int numLevels = m_volume->getNumLevels();
    const dm::int3 pmin{m_posMin}, pmax{m_posMax};
    for (int lev = 0; lev < numLevels; ++lev) {
        dm::int3 a = coveringNodePos(lev, pmin);
        dm::int3 b = coveringNodePos(lev, pmax);
        if (dm::all(a == b)) {
            rootPos = a;
            return lev;
        }
    }
    return -1;
}

void GVDBPointOps::prepareRangeRes(int rootLev) {
    std::vector<int> rangeRes(rootLev);
    for (int lev = 0; lev < rootLev; ++lev) rangeRes[lev] = int(m_volume->getRange(int8_t(lev)));
    nvrhi::IBuffer *buf = prepareAux(AUX_RANGE_RES, rootLev, sizeof(int), false);
    upload(buf, rangeRes.data(), rootLev * sizeof(int));
}

// Reference RadixSortByByte: copies the (lev, x, y, z) ushort keys of
// AUX_BRICK_LEVXYZ into AUX_SORTED_LEVXYZ and sorts them as 64-bit keys.
void GVDBPointOps::radixSortByByte(uint32_t numKeys) {
    nvrhi::IBuffer *sorted = prepareAux(AUX_SORTED_LEVXYZ, uint64_t(numKeys) * 4, sizeof(uint16_t), false);
    if (numKeys == 0) return;
    m_context->getCommandList()->copyBuffer(sorted, 0, aux(AUX_BRICK_LEVXYZ), 0, uint64_t(numKeys) * 4 * sizeof(uint16_t));
    m_primitives.sortKeys64(sorted, numKeys);
}

// Reference FindUniqueBrick: marks the first key of each run, counts them per
// level (AUX_LEVEL_CNT) and compacts them into AUX_UNIQUE_LEVXYZ.
void GVDBPointOps::findUniqueBrick(uint32_t numKeys, int levDepth, int &numUnique) {
    numUnique = 0;
    nvrhi::IBuffer *marker = prepareAux(AUX_MARKER, numKeys, sizeof(int), true);
    nvrhi::IBuffer *presum = prepareAux(AUX_MARKER_PRESUM, numKeys, sizeof(int), true);
    nvrhi::IBuffer *uniqueCnt = prepareAux(AUX_UNIQUE_CNT, 1, sizeof(int), true);
    nvrhi::IBuffer *levCnt = prepareAux(AUX_LEVEL_CNT, levDepth, sizeof(int), true);
    if (numKeys == 0) return;

    const uint64_t sorted = auxAddress(AUX_SORTED_LEVXYZ);
    launch1D("gvdbFindUnique", numKeys, int(numKeys), sorted, deviceAddress(marker), deviceAddress(uniqueCnt),
             deviceAddress(levCnt));
    numUnique = readbackValue<int>(uniqueCnt, 0);

    m_primitives.prefixSum(marker, presum, numKeys);

    nvrhi::IBuffer *unique = prepareAux(AUX_UNIQUE_LEVXYZ, numUnique, sizeof(uint64_t), false);
    launch1D("gvdbCompactUnique", numKeys, int(numKeys), sorted, deviceAddress(marker), deviceAddress(presum),
             deviceAddress(unique));
}

// Reads AUX_LEVEL_CNT / AUX_UNIQUE_LEVXYZ back and allocates one node per
// unique (lev, x, y, z) key, with an empty child list above level 0.
void GVDBPointOps::allocateUniqueNodes(int rootLev, int numUnique) {
    if (numUnique == 0) return;
    std::vector<int> levCnt(rootLev);
    m_context->readBuffer(aux(AUX_LEVEL_CNT), 0, levCnt.data(), rootLev * sizeof(int));
    std::vector<uint16_t> keys(size_t(numUnique) * 4);
    m_context->readBuffer(aux(AUX_UNIQUE_LEVXYZ), 0, keys.data(), keys.size() * sizeof(uint16_t));

    int prefix = 0;
    for (int lev = 0; lev < rootLev; ++lev) {
        const int range = int(m_volume->getRange(int8_t(lev)));
        const int end = std::min(prefix + levCnt[lev], numUnique);
        for (int n = prefix; n < end; ++n) {
            // key layout (little endian): [0] = z, [1] = y, [2] = x, [3] = lev
            dm::int3 pos{int(keys[n * 4 + 2]) * range, int(keys[n * 4 + 1]) * range, int(keys[n * 4 + 0]) * range};
            NodeId id = m_volume->allocateNode(uint8_t(lev), pos, true);
            if (lev > 0) {
                NodeId cl = m_volume->allocateChildList(uint8_t(lev));
                m_volume->getNode(id)->childList = cl;
            }
        }
        prefix += levCnt[lev];
    }
}

// Uploads the pools, then links every active node to its parent, top down
// (gvdbLinkBricks sets node->parent and the parent's child list entry).
void GVDBPointOps::linkBricks(int rootLev) {
    m_volume->commitPools();
    m_volume->invalidateVDBInfo();   // pools (and their GPU buffers) changed
    m_volume->prepareVDBPartially();
    for (int lev = rootLev - 1; lev >= 0; --lev) {
        launch1D("gvdbLinkBricks", m_volume->getNumNodes(uint8_t(lev)), vdbInfo(), lev);
    }
}

// Reference ActivateBricksGPU: full build. One key per point and level,
// sorted, made unique, one node per key, root at rootLev, linked on the GPU.
void GVDBPointOps::activateBricksGPU(uint32_t numPnts, float radius, dm::float3 orig, int rootLev, dm::int3 rootPos) {
    (void)radius;
    prepareRangeRes(rootLev);
    const uint64_t numKeys = uint64_t(numPnts) * rootLev;
    nvrhi::IBuffer *keys = prepareAux(AUX_BRICK_LEVXYZ, numKeys * 4, sizeof(uint16_t), false);

    launch1D("gvdbCalcBrickId", numPnts, int(numPnts), rootLev, auxAddress(AUX_RANGE_RES), ptr(m_pos), off(m_pos),
             stride(m_pos), orig, deviceAddress(keys));

    int numUnique = 0;
    radixSortByByte(uint32_t(numKeys));
    findUniqueBrick(uint32_t(numKeys), rootLev, numUnique);

    // root
    NodeId root = m_volume->allocateNode(uint8_t(rootLev), rootPos, true);
    NodeId rootChildren = m_volume->allocateChildList(uint8_t(rootLev));
    m_volume->getNode(root)->childList = rootChildren;
    m_volume->setRoot(root);

    allocateUniqueNodes(rootLev, numUnique);
    linkBricks(rootLev);
    m_volume->fetchPools();
}

// Reference ActivateExtraBricksGPU: for radius >= 1, adds the nodes covering
// [p - radius, p + 2 radius] that the point pass did not create.
void GVDBPointOps::activateExtraBricksGPU(uint32_t numPnts, float radius, dm::float3 orig, int rootLev) {
    prepareRangeRes(rootLev);
    // NOTE(port): same capacity as the reference (numPnts * rootLev keys); the
    // kernel does not bound its output.
    nvrhi::IBuffer *keys = prepareAux(AUX_BRICK_LEVXYZ, uint64_t(numPnts) * rootLev * 4, sizeof(uint16_t), true);
    nvrhi::IBuffer *extraCnt = prepareAux(AUX_EXTRA_BRICK_CNT, 1, sizeof(int), true);
    m_volume->prepareVDBPartially();

    launch1D("gvdbCalcExtraBrickId", numPnts, vdbInfo(), radius, int(numPnts), rootLev, auxAddress(AUX_RANGE_RES),
             ptr(m_pos), off(m_pos), stride(m_pos), orig, deviceAddress(keys), deviceAddress(extraCnt));

    const int numExtra = readbackValue<int>(extraCnt, 0);
    if (numExtra <= 0) return;
    if (uint64_t(numExtra) > uint64_t(numPnts) * rootLev)
        log::warning("GVDBPointOps: extra brick keys (%d) exceed the key buffer (%llu)", numExtra,
                     (unsigned long long)(uint64_t(numPnts) * rootLev));

    int numUnique = 0;
    radixSortByByte(uint32_t(numExtra));
    findUniqueBrick(uint32_t(numExtra), rootLev, numUnique);
    allocateUniqueNodes(rootLev, numUnique);
    linkBricks(rootLev);
    m_volume->fetchPools();
}

// Reference ActivateIncreBricksGPU: keeps the hierarchy, marks the bricks that
// still receive points (or all existing ones when accum), adds the missing
// nodes, links them and delinks the unused ones.
void GVDBPointOps::activateIncreBricksGPU(uint32_t numPnts, float radius, dm::float3 orig, int rootLev, bool accum) {
    const uint64_t totalLeaf = m_volume->getNumNodes(0);
    if (totalLeaf == 0) return;

    // leaf markers: 0 = deactivate unless a point lands in it
    std::vector<int> marker(totalLeaf, 0);
    if (accum)
        for (uint64_t i = 0; i < totalLeaf; ++i) marker[i] = m_volume->getNode(0, i)->flags ? 1 : 0;
    nvrhi::IBuffer *markerBuf = prepareAux(AUX_NODE_MARKER, totalLeaf, sizeof(int), false);
    upload(markerBuf, marker.data(), totalLeaf * sizeof(int));

    prepareRangeRes(rootLev);
    nvrhi::IBuffer *keys = prepareAux(AUX_BRICK_LEVXYZ, uint64_t(numPnts) * rootLev * 4, sizeof(uint16_t), true);
    nvrhi::IBuffer *extraCnt = prepareAux(AUX_EXTRA_BRICK_CNT, 1, sizeof(int), true);
    m_volume->prepareVDBPartially();

    launch1D("gvdbCalcIncreExtraBrickId", numPnts, vdbInfo(), radius, int(numPnts), rootLev, auxAddress(AUX_RANGE_RES),
             ptr(m_pos), off(m_pos), stride(m_pos), orig, deviceAddress(keys), deviceAddress(extraCnt),
             deviceAddress(markerBuf));

    const int numExtra = readbackValue<int>(extraCnt, 0);
    if (numExtra <= 0) return;   // reference: no new node, nothing else changes
    if (uint64_t(numExtra) > uint64_t(numPnts) * rootLev)
        log::warning("GVDBPointOps: incremental brick keys (%d) exceed the key buffer (%llu)", numExtra,
                     (unsigned long long)(uint64_t(numPnts) * rootLev));

    int numUnique = 0;
    radixSortByByte(uint32_t(numExtra));
    findUniqueBrick(uint32_t(numExtra), rootLev, numUnique);

    // apply the markers to the existing bricks
    m_context->readBuffer(markerBuf, 0, marker.data(), totalLeaf * sizeof(int));
    for (uint64_t i = 0; i < totalLeaf; ++i) m_volume->getNode(0, i)->flags = marker[i] ? 1 : 0;

    allocateUniqueNodes(rootLev, numUnique);
    linkBricks(rootLev);

    // delink, bottom up
    launch1D("gvdbDelinkLeafBricks", m_volume->getNumNodes(0), vdbInfo());
    for (int lev = 1; lev < rootLev; ++lev)
        launch1D("gvdbDelinkBricks", m_volume->getNumNodes(uint8_t(lev)), vdbInfo(), lev);

    m_volume->fetchPools();
    // NOTE(port): the reference updates the pool's used count here; in this port
    // IGVDBVolume::finishTopology() recounts the active bricks.
}

void GVDBPointOps::rebuildTopology(uint32_t numPnts, float radius, dm::float3 origin) {
    if (!m_pos.buffer || numPnts == 0) {
        log::warning("GVDBPointOps::rebuildTopology: no points");
        return;
    }
    computePointBounds(numPnts, origin);

    dm::int3 rootPos;
    int depth = determineDepth(rootPos);
    if (depth == 0) {
        log::error("GVDBPointOps: point extents smaller than 1 brick. Not yet supported.");
        return;
    }
    if (depth < 0 || depth > 5) {
        log::error("GVDBPointOps: point extents exceed 5 levels.");
        return;
    }
    if (m_rebuildTopo) {
        m_volume->clear();
    } else if (depth != m_currDepth) {
        log::warning("GVDBPointOps: depth change in rebuild (%d -> %d).", m_currDepth, depth);
        m_volume->clear();
        m_rebuildTopo = true;
    }
    if (m_rebuildTopo) {
        // NOTE(port): the reference only records mCurrDepth on a depth change, so
        // its second rebuild always reported a depth change; the depth of every
        // full build is recorded here.
        m_currDepth = depth;
        activateBricksGPU(numPnts, radius, origin, depth, rootPos);
        if (radius >= 1.f) activateExtraBricksGPU(numPnts, radius, origin, depth);
        m_rebuildTopo = false;

        releaseAux(AUX_BRICK_LEVXYZ);
        releaseAux(AUX_MARKER);
        releaseAux(AUX_SORTED_LEVXYZ);
        releaseAux(AUX_MARKER_PRESUM);
    } else {
        activateIncreBricksGPU(numPnts, radius, origin, depth, false);
    }
}

void GVDBPointOps::accumulateTopology(uint32_t numPnts, float radius, dm::float3 origin, int minDepth) {
    if (!m_pos.buffer || numPnts == 0) {
        log::warning("GVDBPointOps::accumulateTopology: no points");
        return;
    }
    computePointBounds(numPnts, origin);

    dm::int3 rootPos;
    int depth = determineDepth(rootPos);
    if (depth < minDepth) depth = minDepth;
    if (depth == 0) {
        log::error("GVDBPointOps: point extents smaller than 1 brick. Not yet supported.");
        return;
    }
    if (depth > 5 || depth >= int(m_volume->getNumLevels())) {
        log::error("GVDBPointOps: point extents exceed 5 levels.");
        return;
    }
    if (m_rebuildTopo) {
        rebuildTopology(numPnts, radius, origin);
        return;
    }
    activateIncreBricksGPU(numPnts, radius, origin, depth, true);
}

// ============================================================================
// Sub-cell insertion and gathers
// ============================================================================

// Common part of InsertPointsSubcell / InsertPointsSubcell_FP16 up to the
// point counts per sub-cell. Returns false when there is nothing to insert.
bool GVDBPointOps::prepareSubcells(int subcellSize, uint32_t numPnts, float radius, dm::float3 trans,
                                   int &scPntsLength, int &scPerBrick, int &numSCell, int &scDim, dm::int3 &range,
                                   int &numSCellMapping) {
    scPntsLength = 0;
    m_numSubcells = 0;
    if (!m_pos.buffer) return false;
    if (!checkSubcellSize(subcellSize, "insertPointsSubcell")) return false;
    if (m_volume->getNumNodes(0) == 0) {
        log::warning("GVDBPointOps::insertPointsSubcell: no bricks exist yet.");
        return false;
    }
    m_volume->prepareVDB();

    const int res0 = int(m_volume->getResDim(0));
    scDim = res0 / subcellSize;
    scPerBrick = scDim * scDim * scDim;
    range = dm::int3{int(m_volume->getRange(0)) / scDim};
    numSCellMapping = int(m_volume->getNumNodes(0));

    // brick -> compact index of the bricks that have a parent
    nvrhi::IBuffer *flag = prepareAux(AUX_SUBCELL_FLAG, numSCellMapping, sizeof(int), false);
    nvrhi::IBuffer *mapping = prepareAux(AUX_SUBCELL_MAPPING, numSCellMapping, sizeof(int), false);
    launch1D("gvdbSetFlagSubcell", numSCellMapping, vdbInfo(), numSCellMapping, deviceAddress(flag));
    m_primitives.prefixSum(flag, mapping, numSCellMapping);

    // NOTE(port): the reference sizes the sub-cell arrays with the pool's used
    // count; the number of flagged bricks is used here so the mapping and the
    // arrays always agree.
    const int numBrick = readbackValue<int>(mapping, numSCellMapping - 1) + readbackValue<int>(flag, numSCellMapping - 1);
    if (numBrick == 0) {
        log::warning("GVDBPointOps::insertPointsSubcell: no active bricks.");
        return false;
    }
    numSCell = scPerBrick * numBrick;

    // points per sub-cell
    nvrhi::IBuffer *cnt = prepareAux(AUX_SUBCELL_CNT, numSCell, sizeof(int), true);
    launch1D("gvdbCountSubcell", numPnts, vdbInfo(), subcellSize, scPerBrick, int(numPnts), ptr(m_pos), off(m_pos),
             stride(m_pos), deviceAddress(cnt), range, trans, scDim, radius, numSCell, deviceAddress(mapping));

    nvrhi::IBuffer *prefix = prepareAux(AUX_SUBCELL_PREFIXSUM, numSCell, sizeof(int), true);
    m_primitives.prefixSum(cnt, prefix, numSCell);

    const int offsetLast = readbackValue<int>(prefix, numSCell - 1);
    const int cntLast = readbackValue<int>(cnt, numSCell - 1);
    scPntsLength = offsetLast + cntLast;
    if (scPntsLength == 0) return false;

    // the insert kernels count again
    prepareAux(AUX_SUBCELL_CNT, numSCell, sizeof(int), true);
    return true;
}

// Sub-cell origins and brick indices for the gathers (gvdbCalcSubcellPos).
void GVDBPointOps::finishSubcells(int subcellSize, int scPerBrick, int scDim, int numSCell, int numSCellMapping) {
    nvrhi::IBuffer *pos = prepareAux(AUX_SUBCELL_POS, numSCell, sizeof(dm::int3), false);
    nvrhi::IBuffer *nid = prepareAux(AUX_SUBCELL_NID, numSCell, sizeof(int), false);
    launch1D("gvdbCalcSubcellPos", numSCellMapping, vdbInfo(), deviceAddress(nid), deviceAddress(pos), numSCellMapping,
             scDim, scPerBrick, subcellSize, auxAddress(AUX_SUBCELL_MAPPING));
    m_numSubcells = numSCell;
}

void GVDBPointOps::insertPointsSubcell(int subcellSize, uint32_t numPnts, float radius, dm::float3 trans,
                                       int &scPntsLength) {
    int scPerBrick, numSCell, scDim, numSCellMapping;
    dm::int3 range;
    if (!prepareSubcells(subcellSize, numPnts, radius, trans, scPntsLength, scPerBrick, numSCell, scDim, range,
                         numSCellMapping))
        return;

    nvrhi::IBuffer *scPos = prepareAux(AUX_SUBCELL_PNT_POS, scPntsLength, sizeof(dm::float3), false);
    nvrhi::IBuffer *scVel = m_vel.buffer ? prepareAux(AUX_SUBCELL_PNT_VEL, scPntsLength, sizeof(dm::float3), false) : nullptr;
    nvrhi::IBuffer *scClr = m_clr.buffer ? prepareAux(AUX_SUBCELL_PNT_CLR, scPntsLength, sizeof(uint32_t), false) : nullptr;

    launch1D("gvdbInsertSubcell", numPnts, vdbInfo(), subcellSize, scPerBrick, int(numPnts), auxAddress(AUX_SUBCELL_CNT),
             auxAddress(AUX_SUBCELL_PREFIXSUM), auxAddress(AUX_SUBCELL_MAPPING), range, trans, scDim, radius,
             ptr(m_pos), off(m_pos), stride(m_pos), deviceAddress(scPos),
             ptr(m_vel), off(m_vel), stride(m_vel), deviceAddress(scVel),
             ptr(m_clr), off(m_clr), stride(m_clr), deviceAddress(scClr));

    finishSubcells(subcellSize, scPerBrick, scDim, numSCell, numSCellMapping);
}

void GVDBPointOps::insertPointsSubcellFP16(int subcellSize, uint32_t numPnts, float radius, dm::float3 trans,
                                           int &scPntsLength) {
    int scPerBrick, numSCell, scDim, numSCellMapping;
    dm::int3 range;
    if (!prepareSubcells(subcellSize, numPnts, radius, trans, scPntsLength, scPerBrick, numSCell, scDim, range,
                         numSCellMapping))
        return;

    // NOTE(port): the position bounds come from the last computePointBounds /
    // rebuildTopology; the velocity bounds (reference GetMinMaxVel, which no
    // sample calls) are computed here when velocities are set.
    computeVelBounds(numPnts);

    nvrhi::IBuffer *scPos = prepareAux(AUX_SUBCELL_PNT_POS, scPntsLength, 3 * sizeof(uint16_t), false);
    nvrhi::IBuffer *scVel = m_vel.buffer ? prepareAux(AUX_SUBCELL_PNT_VEL, scPntsLength, 3 * sizeof(uint16_t), false) : nullptr;
    nvrhi::IBuffer *scClr = m_clr.buffer ? prepareAux(AUX_SUBCELL_PNT_CLR, scPntsLength, sizeof(uint32_t), false) : nullptr;

    launch1D("gvdbInsertSubcell_fp16", numPnts, vdbInfo(), subcellSize, scPerBrick, int(numPnts),
             auxAddress(AUX_SUBCELL_CNT), auxAddress(AUX_SUBCELL_PREFIXSUM), auxAddress(AUX_SUBCELL_MAPPING), m_posMin,
             m_posRange, m_velMin, m_velRange, range, trans, scDim, radius,
             ptr(m_pos), off(m_pos), stride(m_pos), deviceAddress(scPos),
             ptr(m_vel), off(m_vel), stride(m_vel), deviceAddress(scVel),
             ptr(m_clr), off(m_clr), stride(m_clr), deviceAddress(scClr));

    finishSubcells(subcellSize, scPerBrick, scDim, numSCell, numSCellMapping);
}

void GVDBPointOps::gatherDensity(int subcellSize, uint32_t numPnts, float radius, dm::float3 trans,
                                 int scPntsLength, int chanDensity, int chanClr, bool accumulate) {
    (void)trans;
    if (scPntsLength == 0 || m_numSubcells == 0) return;
    if (!checkSubcellSize(subcellSize, "gatherDensity")) return;
    m_volume->prepareVDB();
    launch("gvdbGatherDensity", dm::uint3(uint32_t(m_numSubcells), 1u, 1u), vdbInfo(), int(numPnts), m_numSubcells,
           radius, auxAddress(AUX_SUBCELL_NID), auxAddress(AUX_SUBCELL_CNT), auxAddress(AUX_SUBCELL_PREFIXSUM),
           auxAddress(AUX_SUBCELL_POS), auxAddress(AUX_SUBCELL_PNT_POS), auxAddress(AUX_SUBCELL_PNT_VEL),
           auxAddress(AUX_SUBCELL_PNT_CLR), chanDensity, chanClr, accumulate);
}

void GVDBPointOps::gatherLevelSet(int subcellSize, uint32_t numPnts, float radius, dm::float3 trans,
                                  int scPntsLength, int chanLevelSet, int chanClr, bool accumulate) {
    (void)trans;
    (void)scPntsLength;   // the reference gathers even without sub-cell points (writes the far value)
    if (m_numSubcells == 0) return;
    if (!checkSubcellSize(subcellSize, "gatherLevelSet")) return;
    m_volume->prepareVDB();
    launch("gvdbGatherLevelSet", dm::uint3(uint32_t(m_numSubcells), 1u, 1u), vdbInfo(), int(numPnts), m_numSubcells,
           radius, auxAddress(AUX_SUBCELL_NID), auxAddress(AUX_SUBCELL_CNT), auxAddress(AUX_SUBCELL_PREFIXSUM),
           auxAddress(AUX_SUBCELL_POS), auxAddress(AUX_SUBCELL_PNT_POS), auxAddress(AUX_SUBCELL_PNT_VEL),
           auxAddress(AUX_SUBCELL_PNT_CLR), chanLevelSet, chanClr, accumulate);
}

void GVDBPointOps::gatherLevelSetFP16(int subcellSize, uint32_t numPnts, float radius, dm::float3 trans,
                                      int scPntsLength, int chanLevelSet, int chanClr) {
    (void)trans;
    (void)scPntsLength;
    if (m_numSubcells == 0) return;
    if (!checkSubcellSize(subcellSize, "gatherLevelSetFP16")) return;
    m_volume->prepareVDB();
    launch("gvdbGatherLevelSet_fp16", dm::uint3(uint32_t(m_numSubcells), 1u, 1u), vdbInfo(), int(numPnts),
           m_numSubcells, radius, m_posMin, m_posRange, m_velMin, m_velRange, auxAddress(AUX_SUBCELL_NID),
           auxAddress(AUX_SUBCELL_CNT), auxAddress(AUX_SUBCELL_PREFIXSUM), auxAddress(AUX_SUBCELL_POS),
           auxAddress(AUX_SUBCELL_PNT_POS), auxAddress(AUX_SUBCELL_PNT_VEL), auxAddress(AUX_SUBCELL_PNT_CLR),
           chanLevelSet, chanClr);
}

// ============================================================================
// Brick insertion, scatter, conversion
// ============================================================================

void GVDBPointOps::insertPoints(uint32_t numPnts, dm::float3 trans, bool prefix) {
    if (!m_pos.buffer || numPnts == 0) return;
    m_volume->prepareVDB();

    // NOTE(port): the reference sizes the per-brick counters with the atlas
    // brick count; the kernels index them with the level-0 node index.
    const int bricks = int(m_volume->getNumNodes(0));
    if (bricks == 0) return;
    nvrhi::IBuffer *pnode = prepareAux(AUX_PNODE, numPnts, sizeof(int), false);
    nvrhi::IBuffer *pndx = prepareAux(AUX_PNDX, numPnts, sizeof(int), false);
    nvrhi::IBuffer *gridCnt = prepareAux(AUX_GRIDCNT, bricks, sizeof(int), true);

    launch1D("gvdbInsertPoints", numPnts, vdbInfo(), int(numPnts), ptr(m_pos), off(m_pos), stride(m_pos),
             deviceAddress(pnode), deviceAddress(pndx), deviceAddress(gridCnt), trans);

    if (prefix) {
        nvrhi::IBuffer *gridOff = prepareAux(AUX_GRIDOFF, bricks, sizeof(int), false);
        m_primitives.prefixSum(gridCnt, gridOff, bricks);

        nvrhi::IBuffer *sorted = prepareAux(AUX_PNTSORT, numPnts, sizeof(dm::float3), false);
        launch1D("gvdbSortPoints", numPnts, int(numPnts), ptr(m_pos), off(m_pos), stride(m_pos), deviceAddress(pnode),
                 deviceAddress(pndx), bricks, deviceAddress(gridCnt), deviceAddress(gridOff), deviceAddress(sorted), trans);
    }
}

void GVDBPointOps::scatterDensity(uint32_t numPnts, float radius, float amp, dm::float3 trans, bool expand,
                                  bool avgColor) {
    if (!m_pos.buffer || numPnts == 0) return;
    if (!aux(AUX_PNODE)) {
        log::warning("GVDBPointOps::scatterDensity: insertPoints() must be called first.");
        return;
    }
    m_volume->prepareVDB();

    const bool useColorBuf = m_clr.buffer && avgColor;
    uint32_t numVoxels = 0;
    nvrhi::IBuffer *colorBuf = nullptr;
    if (useColorBuf) {
        const dm::uint3 brickRes = m_volume->getBrickDim();
        // NOTE(port): sized with the node count (the kernel indexes by node index).
        numVoxels = brickRes.x * brickRes.y * brickRes.z * uint32_t(m_volume->getNumNodes(0));
        colorBuf = prepareAux(AUX_COLAVG, uint64_t(numVoxels) * 4, sizeof(uint32_t), true);
    }

    launch1D("gvdbScatterPointDensity", numPnts, vdbInfo(), int(numPnts), radius, amp, ptr(m_pos), off(m_pos),
             stride(m_pos), ptr(m_clr), off(m_clr), stride(m_clr), auxAddress(AUX_PNODE), trans, expand,
             deviceAddress(colorBuf));

    if (useColorBuf) {
        launch1D("gvdbScatterPointAvgCol", numVoxels, vdbInfo(), int(numVoxels), deviceAddress(colorBuf));
    }
}

void GVDBPointOps::convertAndTransform(const PointData &src, int srcBits, const PointData &dst, int dstBits,
                                       uint32_t numPnts, dm::float3 wMin, dm::float3 wDelta, dm::float3 trans,
                                       dm::float3 scale) {
    if (!src.buffer || !dst.buffer || numPnts == 0) return;
    // The kernel assumes tightly packed components (6 bytes for ushort, 12 for float).
    launch1D("gvdbConvAndTransform", numPnts, int(numPnts), deviceAddress(src.buffer, src.offset), int8_t(srcBits),
             deviceAddress(dst.buffer, dst.offset), int8_t(dstBits), wMin, wDelta, trans, scale);
}

void GVDBPointOps::scalePointPositions(uint32_t numPnts, float scale) {
    if (!m_pos.buffer || numPnts == 0) return;
    launch1D("gvdbScalePntPos", numPnts, int(numPnts), ptr(m_pos), off(m_pos), stride(m_pos), scale);
}

}  // namespace

nvrhi::FRESULT createGVDBPointOps(IGVDBVolume *volume, IGVDBPointOps **ops) {
    if (!volume || !ops) return nvrhi::FE_INVALID_ARGS;
    *ops = MAKE_RC_OBJ(GVDBPointOps, volume);
    return nvrhi::FS_OK;
}

}  // namespace gvdb
