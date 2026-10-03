// IGVDBPointOps: point clouds -> topology / voxels. Port of the point and GPU
// topology members of VolumeGVDB (gvdb_volume_gvdb.cpp: RebuildTopology,
// AccumulateTopology, ActivateBricksGPU, ActivateExtraBricksGPU,
// ActivateIncreBricksGPU, FindUniqueBrick, RadixSortByByte, GetBoundingBox,
// GetMinMaxVel, InsertPointsSubcell(_FP16), GatherDensity, GatherLevelSet(_FP16),
// InsertPoints, ScatterDensity, ConvertAndTransform, ScalePntPos, PrefixSum).
//
// Everything goes through the public IGVDBVolume interface; the kernels are
// the ones of kernels/GVDBParticles.cu and GVDB.cuh (gvdbLinkBricks,
// gvdbDelink*, gvdbSplitPos), loaded through IGVDBVolume::getKernel().
//
// Deviations from the reference are marked NOTE(port).
#include <gvdb/GVDB.h>
#include <gvdb/GPDeviceCUDAUtils.h>
#include <donut/core/log.h>
#include <nvrhi/core/autoptr.h>
#include <vector>
#include <cstring>
#include <algorithm>

namespace gvdb {

using namespace donut;

namespace {

#define GVDB_PO_V_GP(expr)                                                        \
    do {                                                                          \
        auto rc_ = (expr);                                                        \
        if (NVRHI_FAILED(rc_)) {                                                  \
            donut::log::error("GVDBPointOps: gp call failed (%d): %s", (int)rc_, #expr); \
            NVRHI_ASSERT(0);                                                      \
        }                                                                         \
    } while (0)

// The AUX_* buffers of the reference VolumeGVDB that the point operations use.
enum AuxId {
    AUX_PNODE,              // brick index of each point (insertPoints)
    AUX_PNDX,               // index of the point inside its brick
    AUX_GRIDCNT,            // points per brick
    AUX_GRIDOFF,            // prefix sum of AUX_GRIDCNT
    AUX_PNTSORT,            // points sorted by brick (float3)
    AUX_ARRAY1,             // prefix sum scratch
    AUX_SCAN1,
    AUX_ARRAY2,
    AUX_SCAN2,
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

constexpr int THREADS = 512;

// Reference grid size: int(n / threads) + 1.
inline int blocksFor(uint64_t n, int threads = THREADS) { return int(n / threads) + 1; }

// A null device pointer as a kernel argument. KernelArg::Buffer(nullptr) must
// not be used: the CUDA backend passes no parameter storage for it.
inline gp::KernelArg nullPtrArg() { return gp::KernelArg::Scalar(uint64_t(0)); }

class GVDBPointOps : public nvrhi::ObjectImpl<IGVDBPointOps> {
 public:
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(GVDBPointOps)
    NVRHI_IMPLEMENTS_INTERFACE(IGVDBPointOps)
    NVRHI_END_INTERFACE_TABLE()

    explicit GVDBPointOps(IGVDBVolume *volume)
        : m_volume(volume), m_device(volume->getDevice()), m_queue(volume->getQueue()) {}

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
    struct Aux {
        nvrhi::AutoPtr<gp::IBuffer> buf;
        uint64_t count = 0;
        uint32_t stride = 0;
        bool staging = false;
    };

    // ---- buffers ----
    gp::IBuffer *prepareAux(AuxId id, uint64_t count, uint32_t stride, bool zero, bool staging = false);
    void releaseAux(AuxId id);
    gp::IBuffer *aux(AuxId id) { return m_aux[id].buf.Get(); }
    gp::KernelArg auxArg(AuxId id) { return m_aux[id].buf ? gp::KernelArg::Buffer(m_aux[id].buf.Get()) : nullPtrArg(); }
    void readback(gp::IBuffer *src, uint64_t srcOffset, void *dst, uint64_t bytes);
    template <typename T>
    T readbackValue(gp::IBuffer *src, uint64_t index) {
        T v{};
        readback(src, index * sizeof(T), &v, sizeof(T));
        return v;
    }
    void upload(gp::IBuffer *dst, const void *data, uint64_t bytes) {
        GVDB_PO_V_GP(m_queue->writeBuffer(dst, data, bytes, 0));
    }

    // ---- launches ----
    void launch(const char *kernel, gp::dim3 grid, gp::dim3 block, const std::vector<gp::KernelArg> &args);
    void launch1D(const char *kernel, uint64_t count, int threads, const std::vector<gp::KernelArg> &args) {
        launch(kernel, {blocksFor(count, threads), 1, 1}, {threads, 1, 1}, args);
    }
    static void pushPointArgs(std::vector<gp::KernelArg> &args, const PointData &p);
    gp::KernelArg vdbInfoArg() { return gp::KernelArg::Buffer(m_volume->getVDBInfoGPU()); }

    // ---- algorithms ----
    void prefixSum(gp::IBuffer *in, gp::IBuffer *out, uint32_t numElem);
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
    nvrhi::AutoPtr<gp::IDevice> m_device;
    nvrhi::AutoPtr<gp::IDeviceQueue> m_queue;

    PointData m_pos, m_vel, m_clr;
    Aux m_aux[MAX_AUX];
    nvrhi::AutoPtr<gp::IBuffer> m_readback;   // staging buffer for host readbacks

    bool m_rebuildTopo = true;
    int m_currDepth = -1;
    dm::float3 m_posMin = dm::float3::zero(), m_posMax = dm::float3::zero(), m_posRange = dm::float3{1.f};
    dm::float3 m_velMin = dm::float3::zero(), m_velMax = dm::float3::zero(), m_velRange = dm::float3{1.f};
    int m_numSubcells = 0;      // sub-cells of the last insertPointsSubcell*() (used by the gathers)
};

// ============================================================================
// Buffers and launches
// ============================================================================

gp::IBuffer *GVDBPointOps::prepareAux(AuxId id, uint64_t count, uint32_t stride, bool zero, bool staging) {
    Aux &a = m_aux[id];
    uint64_t bytes = std::max<uint64_t>(count * stride, 4);
    bytes = (bytes + 3) & ~uint64_t(3);   // clearBufferUint needs a multiple of 4
    if (!a.buf || a.buf->getDesc()->byteSize < bytes || a.stride != stride || a.staging != staging) {
        a.buf = nullptr;
        gp::BufferDesc desc;
        desc.byteSize = bytes;
        desc.isStaging = staging;
        GVDB_PO_V_GP(m_device->createBuffer(desc, &a.buf));
        a.stride = stride;
        a.staging = staging;
    }
    a.count = count;
    if (zero) GVDB_PO_V_GP(m_queue->clearBufferUint(a.buf, 0));
    return a.buf;
}

void GVDBPointOps::releaseAux(AuxId id) {
    m_aux[id].buf = nullptr;
    m_aux[id].count = 0;
    m_aux[id].stride = 0;
}

void GVDBPointOps::readback(gp::IBuffer *src, uint64_t srcOffset, void *dst, uint64_t bytes) {
    if (bytes == 0) return;
    if (!m_readback || m_readback->getDesc()->byteSize < bytes) {
        m_readback = nullptr;
        gp::BufferDesc desc;
        desc.byteSize = std::max<uint64_t>(bytes, 4096);
        desc.isStaging = true;
        GVDB_PO_V_GP(m_device->createBuffer(desc, &m_readback));
    }
    GVDB_PO_V_GP(m_queue->copyBufferRegion(m_readback, 0, src, srcOffset, bytes));
    m_device->commitQueue(m_queue);
    GVDB_PO_V_GP(m_device->waitForQueue(m_queue));
    void *mapped = nullptr;
    GVDB_PO_V_GP(m_device->mapBuffer(m_readback, &mapped));
    memcpy(dst, mapped, bytes);
    m_device->unmapBuffer(m_readback);
}

void GVDBPointOps::launch(const char *kernel, gp::dim3 grid, gp::dim3 block,
                          const std::vector<gp::KernelArg> &args) {
    gp::IKernel *k = m_volume->getKernel(kernel);
    if (!k) {
        log::error("GVDBPointOps: kernel %s not found", kernel);
        return;
    }
    GVDB_PO_V_GP(m_queue->launch(k, grid, block, args.data(), args.size()));
}

// char* ppos, int pos_off, int pos_stride
void GVDBPointOps::pushPointArgs(std::vector<gp::KernelArg> &args, const PointData &p) {
    if (p.buffer) {
        args.push_back(gp::KernelArg::Buffer(p.buffer, 0));
        args.push_back(gp::KernelArg::Scalar(int(p.offset)));
        args.push_back(gp::KernelArg::Scalar(int(p.stride)));
    } else {
        args.push_back(nullPtrArg());
        args.push_back(gp::KernelArg::Scalar(int(0)));
        args.push_back(gp::KernelArg::Scalar(int(0)));
    }
}

// Exclusive prefix sum of `numElem` uints (reference PrefixSum: prefixSum /
// prefixFixup kernels, GVDB_SCAN_BLOCKSIZE threads, two elements per thread).
void GVDBPointOps::prefixSum(gp::IBuffer *in, gp::IBuffer *out, uint32_t numElem) {
    if (numElem == 0) return;
    constexpr int naux = GVDB_SCAN_BLOCKSIZE << 1;   // elements per block
    const int grid1 = dm::div_ceil(int(numElem), naux);
    const int grid2 = dm::div_ceil(grid1, naux);
    NVRHI_ASSERT(grid2 <= naux && "prefixSum: too many elements");
    const gp::dim3 block{GVDB_SCAN_BLOCKSIZE, 1, 1};
    const int zon = 1;

    gp::IBuffer *array1 = prepareAux(AUX_ARRAY1, grid1, sizeof(uint32_t), false);
    gp::IBuffer *scan1 = prepareAux(AUX_SCAN1, grid1, sizeof(uint32_t), false);
    gp::IBuffer *array2 = prepareAux(AUX_ARRAY2, grid2, sizeof(uint32_t), false);
    gp::IBuffer *scan2 = prepareAux(AUX_SCAN2, grid2, sizeof(uint32_t), false);

    using KA = gp::KernelArg;
    launch("prefixSum", {grid1, 1, 1}, block,
           {KA::Buffer(in), KA::Buffer(out), KA::Buffer(array1), KA::Scalar(int(numElem)), KA::Scalar(zon)});
    if (grid1 > 1) {
        launch("prefixSum", {grid2, 1, 1}, block,
               {KA::Buffer(array1), KA::Buffer(scan1), KA::Buffer(array2), KA::Scalar(grid1), KA::Scalar(zon)});
        if (grid2 > 1) {
            launch("prefixSum", {1, 1, 1}, block,
                   {KA::Buffer(array2), KA::Buffer(scan2), nullPtrArg(), KA::Scalar(grid2), KA::Scalar(zon)});
            launch("prefixFixup", {grid2, 1, 1}, block, {KA::Buffer(scan1), KA::Buffer(scan2), KA::Scalar(grid1)});
        }
        launch("prefixFixup", {grid1, 1, 1}, block, {KA::Buffer(out), KA::Buffer(scan1), KA::Scalar(int(numElem))});
    }
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
    gp::IBuffer *x = prepareAux(AUX_WORLD_POS_X, numPnts, sizeof(float), false);
    gp::IBuffer *y = prepareAux(AUX_WORLD_POS_Y, numPnts, sizeof(float), false);
    gp::IBuffer *z = prepareAux(AUX_WORLD_POS_Z, numPnts, sizeof(float), false);

    using KA = gp::KernelArg;
    std::vector<KA> args{KA::Scalar(int(numPnts)), KA::Scalar(trans)};
    pushPointArgs(args, m_pos);
    args.insert(args.end(), {KA::Buffer(x), KA::Buffer(y), KA::Buffer(z)});
    launch1D("gvdbSplitPos", numPnts, 256, args);

    float mn[3], mx[3];
    GVDB_PO_V_GP(gp::minMaxFloat(m_queue, x, 0, numPnts, &mn[0], &mx[0]));
    GVDB_PO_V_GP(gp::minMaxFloat(m_queue, y, 0, numPnts, &mn[1], &mx[1]));
    GVDB_PO_V_GP(gp::minMaxFloat(m_queue, z, 0, numPnts, &mn[2], &mx[2]));

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
    gp::IBuffer *x = prepareAux(AUX_WORLD_POS_X, numPnts, sizeof(float), false);
    gp::IBuffer *y = prepareAux(AUX_WORLD_POS_Y, numPnts, sizeof(float), false);
    gp::IBuffer *z = prepareAux(AUX_WORLD_POS_Z, numPnts, sizeof(float), false);

    using KA = gp::KernelArg;
    std::vector<KA> args{KA::Scalar(int(numPnts)), KA::Scalar(dm::float3::zero())};
    pushPointArgs(args, m_vel);
    args.insert(args.end(), {KA::Buffer(x), KA::Buffer(y), KA::Buffer(z)});
    launch1D("gvdbSplitPos", numPnts, 256, args);

    float mn[3], mx[3];
    GVDB_PO_V_GP(gp::minMaxFloat(m_queue, x, 0, numPnts, &mn[0], &mx[0]));
    GVDB_PO_V_GP(gp::minMaxFloat(m_queue, y, 0, numPnts, &mn[1], &mx[1]));
    GVDB_PO_V_GP(gp::minMaxFloat(m_queue, z, 0, numPnts, &mn[2], &mx[2]));
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
    gp::IBuffer *buf = prepareAux(AUX_RANGE_RES, rootLev, sizeof(int), false);
    upload(buf, rangeRes.data(), rootLev * sizeof(int));
}

// Reference RadixSortByByte: copies the (lev, x, y, z) ushort keys of
// AUX_BRICK_LEVXYZ into AUX_SORTED_LEVXYZ and sorts them as 64-bit keys.
void GVDBPointOps::radixSortByByte(uint32_t numKeys) {
    gp::IBuffer *sorted = prepareAux(AUX_SORTED_LEVXYZ, uint64_t(numKeys) * 4, sizeof(uint16_t), false);
    if (numKeys == 0) return;
    GVDB_PO_V_GP(m_queue->copyBufferRegion(sorted, 0, aux(AUX_BRICK_LEVXYZ), 0, uint64_t(numKeys) * 4 * sizeof(uint16_t)));
    GVDB_PO_V_GP(gp::sortKeys64(m_queue, sorted, 0, numKeys));
}

// Reference FindUniqueBrick: marks the first key of each run, counts them per
// level (AUX_LEVEL_CNT) and compacts them into AUX_UNIQUE_LEVXYZ.
void GVDBPointOps::findUniqueBrick(uint32_t numKeys, int levDepth, int &numUnique) {
    numUnique = 0;
    gp::IBuffer *marker = prepareAux(AUX_MARKER, numKeys, sizeof(int), true);
    gp::IBuffer *presum = prepareAux(AUX_MARKER_PRESUM, numKeys, sizeof(int), true);
    gp::IBuffer *uniqueCnt = prepareAux(AUX_UNIQUE_CNT, 1, sizeof(int), true);
    gp::IBuffer *levCnt = prepareAux(AUX_LEVEL_CNT, levDepth, sizeof(int), true);
    if (numKeys == 0) return;

    using KA = gp::KernelArg;
    gp::IBuffer *sorted = aux(AUX_SORTED_LEVXYZ);
    launch1D("gvdbFindUnique", numKeys, THREADS,
             {KA::Scalar(int(numKeys)), KA::Buffer(sorted), KA::Buffer(marker), KA::Buffer(uniqueCnt), KA::Buffer(levCnt)});
    numUnique = readbackValue<int>(uniqueCnt, 0);

    prefixSum(marker, presum, numKeys);

    gp::IBuffer *unique = prepareAux(AUX_UNIQUE_LEVXYZ, numUnique, sizeof(uint64_t), false);
    launch1D("gvdbCompactUnique", numKeys, THREADS,
             {KA::Scalar(int(numKeys)), KA::Buffer(sorted), KA::Buffer(marker), KA::Buffer(presum), KA::Buffer(unique)});
}

// Reads AUX_LEVEL_CNT / AUX_UNIQUE_LEVXYZ back and allocates one node per
// unique (lev, x, y, z) key, with an empty child list above level 0.
void GVDBPointOps::allocateUniqueNodes(int rootLev, int numUnique) {
    if (numUnique == 0) return;
    std::vector<int> levCnt(rootLev);
    readback(aux(AUX_LEVEL_CNT), 0, levCnt.data(), rootLev * sizeof(int));
    std::vector<uint16_t> keys(size_t(numUnique) * 4);
    readback(aux(AUX_UNIQUE_LEVXYZ), 0, keys.data(), keys.size() * sizeof(uint16_t));

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
    using KA = gp::KernelArg;
    for (int lev = rootLev - 1; lev >= 0; --lev) {
        launch1D("gvdbLinkBricks", m_volume->getNumNodes(uint8_t(lev)), THREADS, {vdbInfoArg(), KA::Scalar(lev)});
    }
}

// Reference ActivateBricksGPU: full build. One key per point and level,
// sorted, made unique, one node per key, root at rootLev, linked on the GPU.
void GVDBPointOps::activateBricksGPU(uint32_t numPnts, float radius, dm::float3 orig, int rootLev, dm::int3 rootPos) {
    (void)radius;
    prepareRangeRes(rootLev);
    const uint64_t numKeys = uint64_t(numPnts) * rootLev;
    gp::IBuffer *keys = prepareAux(AUX_BRICK_LEVXYZ, numKeys * 4, sizeof(uint16_t), false);

    using KA = gp::KernelArg;
    std::vector<KA> args{KA::Scalar(int(numPnts)), KA::Scalar(rootLev), auxArg(AUX_RANGE_RES)};
    pushPointArgs(args, m_pos);
    args.insert(args.end(), {KA::Scalar(orig), KA::Buffer(keys)});
    launch1D("gvdbCalcBrickId", numPnts, THREADS, args);

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
    gp::IBuffer *keys = prepareAux(AUX_BRICK_LEVXYZ, uint64_t(numPnts) * rootLev * 4, sizeof(uint16_t), true);
    gp::IBuffer *extraCnt = prepareAux(AUX_EXTRA_BRICK_CNT, 1, sizeof(int), true);
    m_volume->prepareVDBPartially();

    using KA = gp::KernelArg;
    std::vector<KA> args{vdbInfoArg(), KA::Scalar(radius), KA::Scalar(int(numPnts)), KA::Scalar(rootLev),
                         auxArg(AUX_RANGE_RES)};
    pushPointArgs(args, m_pos);
    args.insert(args.end(), {KA::Scalar(orig), KA::Buffer(keys), KA::Buffer(extraCnt)});
    launch1D("gvdbCalcExtraBrickId", numPnts, THREADS, args);

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
    gp::IBuffer *markerBuf = prepareAux(AUX_NODE_MARKER, totalLeaf, sizeof(int), false);
    upload(markerBuf, marker.data(), totalLeaf * sizeof(int));

    prepareRangeRes(rootLev);
    gp::IBuffer *keys = prepareAux(AUX_BRICK_LEVXYZ, uint64_t(numPnts) * rootLev * 4, sizeof(uint16_t), true);
    gp::IBuffer *extraCnt = prepareAux(AUX_EXTRA_BRICK_CNT, 1, sizeof(int), true);
    m_volume->prepareVDBPartially();

    using KA = gp::KernelArg;
    std::vector<KA> args{vdbInfoArg(), KA::Scalar(radius), KA::Scalar(int(numPnts)), KA::Scalar(rootLev),
                         auxArg(AUX_RANGE_RES)};
    pushPointArgs(args, m_pos);
    args.insert(args.end(), {KA::Scalar(orig), KA::Buffer(keys), KA::Buffer(extraCnt), KA::Buffer(markerBuf)});
    launch1D("gvdbCalcIncreExtraBrickId", numPnts, THREADS, args);

    const int numExtra = readbackValue<int>(extraCnt, 0);
    if (numExtra <= 0) return;   // reference: no new node, nothing else changes
    if (uint64_t(numExtra) > uint64_t(numPnts) * rootLev)
        log::warning("GVDBPointOps: incremental brick keys (%d) exceed the key buffer (%llu)", numExtra,
                     (unsigned long long)(uint64_t(numPnts) * rootLev));

    int numUnique = 0;
    radixSortByByte(uint32_t(numExtra));
    findUniqueBrick(uint32_t(numExtra), rootLev, numUnique);

    // apply the markers to the existing bricks
    readback(markerBuf, 0, marker.data(), totalLeaf * sizeof(int));
    for (uint64_t i = 0; i < totalLeaf; ++i) m_volume->getNode(0, i)->flags = marker[i] ? 1 : 0;

    allocateUniqueNodes(rootLev, numUnique);
    linkBricks(rootLev);

    // delink, bottom up
    launch1D("gvdbDelinkLeafBricks", m_volume->getNumNodes(0), THREADS, {vdbInfoArg()});
    for (int lev = 1; lev < rootLev; ++lev)
        launch1D("gvdbDelinkBricks", m_volume->getNumNodes(uint8_t(lev)), THREADS, {vdbInfoArg(), KA::Scalar(lev)});

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
    gp::IBuffer *flag = prepareAux(AUX_SUBCELL_FLAG, numSCellMapping, sizeof(int), false);
    gp::IBuffer *mapping = prepareAux(AUX_SUBCELL_MAPPING, numSCellMapping, sizeof(int), false);
    using KA = gp::KernelArg;
    launch1D("gvdbSetFlagSubcell", numSCellMapping, THREADS, {vdbInfoArg(), KA::Scalar(numSCellMapping), KA::Buffer(flag)});
    prefixSum(flag, mapping, numSCellMapping);

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
    gp::IBuffer *cnt = prepareAux(AUX_SUBCELL_CNT, numSCell, sizeof(int), true);
    std::vector<KA> args{vdbInfoArg(), KA::Scalar(subcellSize), KA::Scalar(scPerBrick), KA::Scalar(int(numPnts))};
    pushPointArgs(args, m_pos);
    args.insert(args.end(), {KA::Buffer(cnt), KA::Scalar(range), KA::Scalar(trans), KA::Scalar(scDim),
                             KA::Scalar(radius), KA::Scalar(numSCell), KA::Buffer(mapping)});
    launch1D("gvdbCountSubcell", numPnts, THREADS, args);

    gp::IBuffer *prefix = prepareAux(AUX_SUBCELL_PREFIXSUM, numSCell, sizeof(int), true);
    prefixSum(cnt, prefix, numSCell);

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
    gp::IBuffer *pos = prepareAux(AUX_SUBCELL_POS, numSCell, sizeof(dm::int3), false);
    gp::IBuffer *nid = prepareAux(AUX_SUBCELL_NID, numSCell, sizeof(int), false);
    using KA = gp::KernelArg;
    launch1D("gvdbCalcSubcellPos", numSCellMapping, THREADS,
             {vdbInfoArg(), KA::Buffer(nid), KA::Buffer(pos), KA::Scalar(numSCellMapping), KA::Scalar(scDim),
              KA::Scalar(scPerBrick), KA::Scalar(subcellSize), auxArg(AUX_SUBCELL_MAPPING)});
    m_numSubcells = numSCell;
}

void GVDBPointOps::insertPointsSubcell(int subcellSize, uint32_t numPnts, float radius, dm::float3 trans,
                                       int &scPntsLength) {
    int scPerBrick, numSCell, scDim, numSCellMapping;
    dm::int3 range;
    if (!prepareSubcells(subcellSize, numPnts, radius, trans, scPntsLength, scPerBrick, numSCell, scDim, range,
                         numSCellMapping))
        return;

    gp::IBuffer *scPos = prepareAux(AUX_SUBCELL_PNT_POS, scPntsLength, sizeof(dm::float3), false);
    gp::IBuffer *scVel = m_vel.buffer ? prepareAux(AUX_SUBCELL_PNT_VEL, scPntsLength, sizeof(dm::float3), false) : nullptr;
    gp::IBuffer *scClr = m_clr.buffer ? prepareAux(AUX_SUBCELL_PNT_CLR, scPntsLength, sizeof(uint32_t), false) : nullptr;

    using KA = gp::KernelArg;
    std::vector<KA> args{vdbInfoArg(), KA::Scalar(subcellSize), KA::Scalar(scPerBrick), KA::Scalar(int(numPnts)),
                         auxArg(AUX_SUBCELL_CNT), auxArg(AUX_SUBCELL_PREFIXSUM), auxArg(AUX_SUBCELL_MAPPING),
                         KA::Scalar(range), KA::Scalar(trans), KA::Scalar(scDim), KA::Scalar(radius)};
    pushPointArgs(args, m_pos);
    args.push_back(KA::Buffer(scPos));
    pushPointArgs(args, m_vel);
    args.push_back(scVel ? KA::Buffer(scVel) : nullPtrArg());
    pushPointArgs(args, m_clr);
    args.push_back(scClr ? KA::Buffer(scClr) : nullPtrArg());
    launch1D("gvdbInsertSubcell", numPnts, THREADS, args);

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

    gp::IBuffer *scPos = prepareAux(AUX_SUBCELL_PNT_POS, scPntsLength, 3 * sizeof(uint16_t), false);
    gp::IBuffer *scVel = m_vel.buffer ? prepareAux(AUX_SUBCELL_PNT_VEL, scPntsLength, 3 * sizeof(uint16_t), false) : nullptr;
    gp::IBuffer *scClr = m_clr.buffer ? prepareAux(AUX_SUBCELL_PNT_CLR, scPntsLength, sizeof(uint32_t), false) : nullptr;

    using KA = gp::KernelArg;
    std::vector<KA> args{vdbInfoArg(), KA::Scalar(subcellSize), KA::Scalar(scPerBrick), KA::Scalar(int(numPnts)),
                         auxArg(AUX_SUBCELL_CNT), auxArg(AUX_SUBCELL_PREFIXSUM), auxArg(AUX_SUBCELL_MAPPING),
                         KA::Scalar(m_posMin), KA::Scalar(m_posRange), KA::Scalar(m_velMin), KA::Scalar(m_velRange),
                         KA::Scalar(range), KA::Scalar(trans), KA::Scalar(scDim), KA::Scalar(radius)};
    pushPointArgs(args, m_pos);
    args.push_back(KA::Buffer(scPos));
    pushPointArgs(args, m_vel);
    args.push_back(scVel ? KA::Buffer(scVel) : nullPtrArg());
    pushPointArgs(args, m_clr);
    args.push_back(scClr ? KA::Buffer(scClr) : nullPtrArg());
    launch1D("gvdbInsertSubcell_fp16", numPnts, THREADS, args);

    finishSubcells(subcellSize, scPerBrick, scDim, numSCell, numSCellMapping);
}

void GVDBPointOps::gatherDensity(int subcellSize, uint32_t numPnts, float radius, dm::float3 trans,
                                 int scPntsLength, int chanDensity, int chanClr, bool accumulate) {
    (void)trans;
    if (scPntsLength == 0 || m_numSubcells == 0) return;
    m_volume->prepareVDB();
    using KA = gp::KernelArg;
    launch("gvdbGatherDensity", {m_numSubcells, 1, 1}, {subcellSize, subcellSize, subcellSize},
           {vdbInfoArg(), KA::Scalar(int(numPnts)), KA::Scalar(m_numSubcells), KA::Scalar(radius),
            auxArg(AUX_SUBCELL_NID), auxArg(AUX_SUBCELL_CNT), auxArg(AUX_SUBCELL_PREFIXSUM), auxArg(AUX_SUBCELL_POS),
            auxArg(AUX_SUBCELL_PNT_POS), auxArg(AUX_SUBCELL_PNT_VEL), auxArg(AUX_SUBCELL_PNT_CLR),
            KA::Scalar(chanDensity), KA::Scalar(chanClr), KA::Scalar(accumulate)});
}

void GVDBPointOps::gatherLevelSet(int subcellSize, uint32_t numPnts, float radius, dm::float3 trans,
                                  int scPntsLength, int chanLevelSet, int chanClr, bool accumulate) {
    (void)trans;
    (void)scPntsLength;   // the reference gathers even without sub-cell points (writes the far value)
    if (m_numSubcells == 0) return;
    m_volume->prepareVDB();
    using KA = gp::KernelArg;
    launch("gvdbGatherLevelSet", {m_numSubcells, 1, 1}, {subcellSize, subcellSize, subcellSize},
           {vdbInfoArg(), KA::Scalar(int(numPnts)), KA::Scalar(m_numSubcells), KA::Scalar(radius),
            auxArg(AUX_SUBCELL_NID), auxArg(AUX_SUBCELL_CNT), auxArg(AUX_SUBCELL_PREFIXSUM), auxArg(AUX_SUBCELL_POS),
            auxArg(AUX_SUBCELL_PNT_POS), auxArg(AUX_SUBCELL_PNT_VEL), auxArg(AUX_SUBCELL_PNT_CLR),
            KA::Scalar(chanLevelSet), KA::Scalar(chanClr), KA::Scalar(accumulate)});
}

void GVDBPointOps::gatherLevelSetFP16(int subcellSize, uint32_t numPnts, float radius, dm::float3 trans,
                                      int scPntsLength, int chanLevelSet, int chanClr) {
    (void)trans;
    (void)scPntsLength;
    if (m_numSubcells == 0) return;
    m_volume->prepareVDB();
    using KA = gp::KernelArg;
    launch("gvdbGatherLevelSet_fp16", {m_numSubcells, 1, 1}, {subcellSize, subcellSize, subcellSize},
           {vdbInfoArg(), KA::Scalar(int(numPnts)), KA::Scalar(m_numSubcells), KA::Scalar(radius),
            KA::Scalar(m_posMin), KA::Scalar(m_posRange), KA::Scalar(m_velMin), KA::Scalar(m_velRange),
            auxArg(AUX_SUBCELL_NID), auxArg(AUX_SUBCELL_CNT), auxArg(AUX_SUBCELL_PREFIXSUM), auxArg(AUX_SUBCELL_POS),
            auxArg(AUX_SUBCELL_PNT_POS), auxArg(AUX_SUBCELL_PNT_VEL), auxArg(AUX_SUBCELL_PNT_CLR),
            KA::Scalar(chanLevelSet), KA::Scalar(chanClr)});
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
    gp::IBuffer *pnode = prepareAux(AUX_PNODE, numPnts, sizeof(int), false);
    gp::IBuffer *pndx = prepareAux(AUX_PNDX, numPnts, sizeof(int), false);
    gp::IBuffer *gridCnt = prepareAux(AUX_GRIDCNT, bricks, sizeof(int), true);

    using KA = gp::KernelArg;
    std::vector<KA> args{vdbInfoArg(), KA::Scalar(int(numPnts))};
    pushPointArgs(args, m_pos);
    args.insert(args.end(), {KA::Buffer(pnode), KA::Buffer(pndx), KA::Buffer(gridCnt), KA::Scalar(trans)});
    launch1D("gvdbInsertPoints", numPnts, THREADS, args);

    if (prefix) {
        gp::IBuffer *gridOff = prepareAux(AUX_GRIDOFF, bricks, sizeof(int), false);
        prefixSum(gridCnt, gridOff, bricks);

        gp::IBuffer *sorted = prepareAux(AUX_PNTSORT, numPnts, sizeof(dm::float3), false);
        std::vector<KA> sortArgs{KA::Scalar(int(numPnts))};
        pushPointArgs(sortArgs, m_pos);
        sortArgs.insert(sortArgs.end(), {KA::Buffer(pnode), KA::Buffer(pndx), KA::Scalar(bricks), KA::Buffer(gridCnt),
                                         KA::Buffer(gridOff), KA::Buffer(sorted), KA::Scalar(trans)});
        launch1D("gvdbSortPoints", numPnts, THREADS, sortArgs);
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
    gp::IBuffer *colorBuf = nullptr;
    if (useColorBuf) {
        const dm::uint3 brickRes = m_volume->getBrickDim();
        // NOTE(port): sized with the node count (the kernel indexes by node index).
        numVoxels = brickRes.x * brickRes.y * brickRes.z * uint32_t(m_volume->getNumNodes(0));
        colorBuf = prepareAux(AUX_COLAVG, uint64_t(numVoxels) * 4, sizeof(uint32_t), true);
    }

    using KA = gp::KernelArg;
    std::vector<KA> args{vdbInfoArg(), KA::Scalar(int(numPnts)), KA::Scalar(radius), KA::Scalar(amp)};
    pushPointArgs(args, m_pos);
    pushPointArgs(args, m_clr);
    args.insert(args.end(), {auxArg(AUX_PNODE), KA::Scalar(trans), KA::Scalar(expand),
                             colorBuf ? KA::Buffer(colorBuf) : nullPtrArg()});
    launch1D("gvdbScatterPointDensity", numPnts, 256, args);

    if (useColorBuf) {
        launch1D("gvdbScatterPointAvgCol", numVoxels, 256,
                 {vdbInfoArg(), KA::Scalar(int(numVoxels)), KA::Buffer(colorBuf)});
    }
}

void GVDBPointOps::convertAndTransform(const PointData &src, int srcBits, const PointData &dst, int dstBits,
                                       uint32_t numPnts, dm::float3 wMin, dm::float3 wDelta, dm::float3 trans,
                                       dm::float3 scale) {
    if (!src.buffer || !dst.buffer || numPnts == 0) return;
    using KA = gp::KernelArg;
    // The kernel assumes tightly packed components (6 bytes for ushort, 12 for float).
    launch1D("gvdbConvAndTransform", numPnts, THREADS,
             {KA::Scalar(int(numPnts)), KA::Buffer(src.buffer, src.offset), KA::Scalar(char(srcBits)),
              KA::Buffer(dst.buffer, dst.offset), KA::Scalar(char(dstBits)), KA::Scalar(wMin), KA::Scalar(wDelta),
              KA::Scalar(trans), KA::Scalar(scale)});
}

void GVDBPointOps::scalePointPositions(uint32_t numPnts, float scale) {
    if (!m_pos.buffer || numPnts == 0) return;
    using KA = gp::KernelArg;
    std::vector<KA> args{KA::Scalar(int(numPnts))};
    pushPointArgs(args, m_pos);
    args.push_back(KA::Scalar(scale));
    launch1D("gvdbScalePntPos", numPnts, THREADS, args);
}

}  // namespace

nvrhi::FRESULT createGVDBPointOps(IGVDBVolume *volume, IGVDBPointOps **ops) {
    if (!volume || !ops) return nvrhi::FE_INVALID_ARGS;
    *ops = MAKE_RC_OBJ(GVDBPointOps, volume);
    return nvrhi::FS_OK;
}

}  // namespace gvdb
