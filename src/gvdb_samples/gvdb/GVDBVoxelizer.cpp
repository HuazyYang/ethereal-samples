// IGVDBVoxelizer: triangle mesh -> topology and voxels. Port of
// VolumeGVDB::SolidVoxelize / InsertTriangles / VoxelizeNode
// (gvdb_volume_gvdb.cpp) through the public IGVDBVolume interface, using the
// gvdbInsertTriangles / gvdbSortTriangles / gvdbVoxelize / prefixSum kernels
// of kernels/GVDBParticles.cu.
//
// Algorithm (hierarchical rasterization): clear the volume, activate every
// top-level node covering the transformed model bounds, then for each level
// from the top down bin the transformed triangles by y (the node height of the
// level), voxelize each node of the level at its child resolution and either
// activate the children that received a value (levels > 0) or copy the voxels
// into the brick's atlas slot (level 0). The topology is finished and the atlas
// allocated once level 1 has been processed, before the bricks are written.
//
// The apron is NOT updated here: call IGVDBVoxelOps::updateApron() afterwards
// (the reference did it inside SolidVoxelize).
#include <gvdb/GVDB.h>
#include <donut/core/log.h>
#include <nvrhi/core/autoptr.h>
#include <vector>
#include <cstring>
#include <algorithm>

namespace gvdb {

using namespace donut;

namespace {

#define GVDB_VX_V_GP(expr)                                                        \
    do {                                                                          \
        auto rc_ = (expr);                                                        \
        if (NVRHI_FAILED(rc_)) {                                                  \
            donut::log::error("GVDBVoxelizer: gp call failed (%d): %s", (int)rc_, #expr); \
            NVRHI_ASSERT(0);                                                      \
        }                                                                         \
    } while (0)

enum AuxId {
    AUX_BIN_COUNT,   // triangles per y bin (reference AUX_GRIDCNT)
    AUX_BIN_OFFSET,  // prefix sum of the bin counts (reference AUX_GRIDOFF)
    AUX_TRI_BUF,     // triangles deep-copied into their bins (3 float3 each)
    AUX_VOXELIZE,    // voxel values of one node (e.icnt elements)
    AUX_ARRAY1,      // prefix sum scratch
    AUX_SCAN1,
    AUX_ARRAY2,
    AUX_SCAN2,
    MAX_AUX
};

// A null device pointer as a kernel argument (KernelArg::Buffer(nullptr) passes no storage).
inline gp::KernelArg nullPtrArg() { return gp::KernelArg::Scalar(uint64_t(0)); }

class GVDBVoxelizer : public nvrhi::ObjectImpl<IGVDBVoxelizer> {
 public:
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(GVDBVoxelizer)
    NVRHI_IMPLEMENTS_INTERFACE(IGVDBVoxelizer)
    NVRHI_END_INTERFACE_TABLE()

    explicit GVDBVoxelizer(IGVDBVolume *volume)
        : m_volume(volume), m_device(volume->getDevice()), m_queue(volume->getQueue()) {}

    IGVDBVolume *getVolume() const override { return m_volume; }

    nvrhi::FRESULT solidVoxelize(int channel, gp::IBuffer *vertices, uint32_t numVertices, gp::IBuffer *indices,
                                 uint32_t numIndices, const dm::box3 &modelBounds, const dm::affine3 &matModelToIndex,
                                 float valSurface, float valInside) override;

 private:
    struct Aux {
        nvrhi::AutoPtr<gp::IBuffer> buf;
        uint32_t stride = 0;
    };

    gp::IBuffer *prepareAux(AuxId id, uint64_t count, uint32_t stride, bool zero);
    gp::IBuffer *aux(AuxId id) { return m_aux[id].buf.Get(); }
    void readback(gp::IBuffer *src, uint64_t srcOffset, void *dst, uint64_t bytes);
    void launch(const char *kernel, gp::dim3 grid, gp::dim3 block, const std::vector<gp::KernelArg> &args);
    void prefixSum(gp::IBuffer *in, gp::IBuffer *out, uint32_t numElem);

    // Bins the transformed triangles by y with bin height ybdiv. Returns
    // (bins, binned triangles, source triangles).
    dm::int3 insertTriangles(float ybdiv);
    // Voxelizes one node at its child resolution; returns the number of
    // activated children (0 for bricks, whose voxels go to the atlas).
    uint32_t voxelizeNode(Node *node, int channel, float bdiv, float valSurface, float valInside,
                          float valVoidThreshold);

    nvrhi::AutoPtr<IGVDBVolume> m_volume;
    nvrhi::AutoPtr<gp::IDevice> m_device;
    nvrhi::AutoPtr<gp::IDeviceQueue> m_queue;
    Aux m_aux[MAX_AUX];
    nvrhi::AutoPtr<gp::IBuffer> m_readback;

    // per solidVoxelize() call
    gp::IBuffer *m_vertices = nullptr;
    gp::IBuffer *m_indices = nullptr;
    int m_numVertices = 0;
    int m_numTriangles = 0;
    int m_numBins = 0;
    dm::float4x4 m_matModelToIndex = dm::float4x4::identity();   // the `cxform` constant (row vectors)
    dm::box3 m_indexBounds;                                      // transformed, padded model bounds
};

gp::IBuffer *GVDBVoxelizer::prepareAux(AuxId id, uint64_t count, uint32_t stride, bool zero) {
    Aux &a = m_aux[id];
    uint64_t bytes = std::max<uint64_t>(count * stride, 4);
    bytes = (bytes + 3) & ~uint64_t(3);
    if (!a.buf || a.buf->getDesc()->byteSize < bytes || a.stride != stride) {
        a.buf = nullptr;
        gp::BufferDesc desc;
        desc.byteSize = bytes;
        GVDB_VX_V_GP(m_device->createBuffer(desc, &a.buf));
        a.stride = stride;
    }
    if (zero) GVDB_VX_V_GP(m_queue->clearBufferUint(a.buf, 0));
    return a.buf;
}

void GVDBVoxelizer::readback(gp::IBuffer *src, uint64_t srcOffset, void *dst, uint64_t bytes) {
    if (bytes == 0) return;
    if (!m_readback || m_readback->getDesc()->byteSize < bytes) {
        m_readback = nullptr;
        gp::BufferDesc desc;
        desc.byteSize = std::max<uint64_t>(bytes, 4096);
        desc.isStaging = true;
        GVDB_VX_V_GP(m_device->createBuffer(desc, &m_readback));
    }
    GVDB_VX_V_GP(m_queue->copyBufferRegion(m_readback, 0, src, srcOffset, bytes));
    m_device->commitQueue(m_queue);
    GVDB_VX_V_GP(m_device->waitForQueue(m_queue));
    void *mapped = nullptr;
    GVDB_VX_V_GP(m_device->mapBuffer(m_readback, &mapped));
    memcpy(dst, mapped, bytes);
    m_device->unmapBuffer(m_readback);
}

void GVDBVoxelizer::launch(const char *kernel, gp::dim3 grid, gp::dim3 block,
                           const std::vector<gp::KernelArg> &args) {
    gp::IKernel *k = m_volume->getKernel(kernel);
    if (!k) {
        log::error("GVDBVoxelizer: kernel %s not found", kernel);
        return;
    }
    GVDB_VX_V_GP(m_queue->launch(k, grid, block, args.data(), args.size()));
}

// Exclusive prefix sum of `numElem` uints (reference PrefixSum).
void GVDBVoxelizer::prefixSum(gp::IBuffer *in, gp::IBuffer *out, uint32_t numElem) {
    if (numElem == 0) return;
    constexpr int naux = GVDB_SCAN_BLOCKSIZE << 1;
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

nvrhi::FRESULT GVDBVoxelizer::solidVoxelize(int channel, gp::IBuffer *vertices, uint32_t numVertices,
                                            gp::IBuffer *indices, uint32_t numIndices, const dm::box3 &modelBounds,
                                            const dm::affine3 &matModelToIndex, float valSurface, float valInside) {
    if (!vertices || !indices || numVertices == 0 || numIndices < 3) return nvrhi::FE_INVALID_ARGS;
    if (channel < 0 || channel >= m_volume->getNumChannels()) return nvrhi::FE_INVALID_ARGS;
    if (m_volume->getNumLevels() == 0) return nvrhi::FE_INVALID_ARGS;

    m_vertices = vertices;
    m_indices = indices;
    m_numVertices = int(numVertices);
    m_numTriangles = int(numIndices / 3);
    m_matModelToIndex = dm::affineToHomogeneous(matModelToIndex);

    // Reference Model::ComputeBounds(xform, 0.1): bounds of the transformed
    // model, grown by 10% of the extent on each side.
    dm::box3 b = modelBounds * matModelToIndex;
    dm::float3 margin = b.diagonal() * 0.1f;
    m_indexBounds = dm::box3(b.lower() - margin, b.upper() + margin);

    // VDB hierarchical rasterization: new root, all top-level nodes in the bounds.
    m_volume->clear();
    const int N = m_volume->getNumLevels();
    Extents e = m_volume->computeExtents(uint8_t(N), m_indexBounds);   // children = level N-1 nodes
    m_volume->activateRegion(e);

    for (int lev = N - 1; lev >= 0; --lev) {
        const uint64_t nodeCnt = m_volume->getNumNodes(uint8_t(lev));
        const float ybinDiv = m_volume->getCover(int8_t(lev)).y;   // bins align with the nodes of this level
        insertTriangles(ybinDiv);

        uint32_t cnt = 0;
        for (uint64_t n = 0; n < nodeCnt; ++n) {
            Node *node = m_volume->getNode(uint8_t(lev), n);
            cnt += voxelizeNode(node, channel, ybinDiv, valSurface, valInside, 0.f);
        }
        if (lev == 1) {   // finish and allocate the atlas before the bricks are written
            m_volume->finishTopology(true);
            m_volume->updateAtlas();
        }
        log::info("Voxelized.. lev: %d, nodes: %llu, new: %u", lev, (unsigned long long)nodeCnt, cnt);
    }

    m_device->commitQueue(m_queue);
    GVDB_VX_V_GP(m_device->waitForQueue(m_queue));
    m_vertices = nullptr;
    m_indices = nullptr;
    return nvrhi::FS_OK;
}

dm::int3 GVDBVoxelizer::insertTriangles(float ybdiv) {
    m_numBins = int(m_indexBounds.upper().y / ybdiv) + 1;
    const int ybins = m_numBins;
    gp::IBuffer *binCount = prepareAux(AUX_BIN_COUNT, ybins, sizeof(uint32_t), true);
    gp::IBuffer *binOffset = prepareAux(AUX_BIN_OFFSET, ybins, sizeof(uint32_t), false);

    using KA = gp::KernelArg;
    const gp::dim3 block{512, 1, 1};
    const gp::dim3 grid{dm::div_ceil(m_numTriangles, block.x), 1, 1};

    // histogram of triangles per bin
    GVDB_VX_V_GP(m_queue->setConstantBuffer(m_volume->getKernel("gvdbInsertTriangles"), "cxform",
                                            &m_matModelToIndex, sizeof(m_matModelToIndex)));
    launch("gvdbInsertTriangles", grid, block,
           {KA::Scalar(ybdiv), KA::Scalar(ybins), KA::Buffer(binCount), KA::Scalar(m_numVertices),
            KA::Scalar(m_numTriangles), KA::Buffer(m_vertices), KA::Buffer(m_indices)});

    // bin offsets, total binned triangles
    prefixSum(binCount, binOffset, ybins);
    std::vector<uint32_t> counts(ybins);
    readback(binCount, 0, counts.data(), ybins * sizeof(uint32_t));
    int triCnt = 0;
    for (int n = 0; n < ybins; ++n) triCnt += int(counts[n]);

    // deep copy of the transformed triangles into their bins
    GVDB_VX_V_GP(m_queue->clearBufferUint(binCount, 0));
    gp::IBuffer *triBuf = prepareAux(AUX_TRI_BUF, std::max(triCnt, 1), 3 * sizeof(dm::float3), false);
    launch("gvdbSortTriangles", grid, block,
           {KA::Scalar(ybdiv), KA::Scalar(ybins), KA::Buffer(binCount), KA::Buffer(binOffset), KA::Scalar(triCnt),
            KA::Buffer(triBuf), KA::Scalar(m_numVertices), KA::Scalar(m_numTriangles), KA::Buffer(m_vertices),
            KA::Buffer(m_indices)});

    return dm::int3{ybins, triCnt, m_numTriangles};
}

uint32_t GVDBVoxelizer::voxelizeNode(Node *node, int channel, float bdiv, float valSurface, float valInside,
                                     float valVoidThreshold) {
    Extents e = m_volume->computeExtents(node);
    if (e.icnt <= 0) return 0;

    // gvdbVoxelize writes the channel's element type (VBX type codes)
    const AtlasFormat format = m_volume->getChannelDesc(channel).format;
    const uint8_t otype = uint8_t(atlasFormatToVBXType(format));
    const uint32_t elemSize = atlasFormatBytes(format);
    if (format != ATLAS_FORMAT_R8_UINT && format != ATLAS_FORMAT_R32_FLOAT) {
        log::error("GVDBVoxelizer: channel %d must be R8_UINT or R32_FLOAT", channel);
        return 0;
    }

    gp::IBuffer *voxels = prepareAux(AUX_VOXELIZE, uint64_t(e.icnt), elemSize, true);

    using KA = gp::KernelArg;
    const gp::dim3 block{8, 8, 8};
    const gp::dim3 grid{dm::div_ceil(e.ires.x, block.x), dm::div_ceil(e.ires.y, block.y),
                        dm::div_ceil(e.ires.z, block.z)};
    launch("gvdbVoxelize", grid, block,
           {KA::Scalar(e.vmin), KA::Scalar(e.vmax), KA::Scalar(e.ires), KA::Buffer(voxels), KA::Scalar(otype),
            KA::Scalar(valSurface), KA::Scalar(valInside), KA::Scalar(bdiv), KA::Scalar(m_numBins),
            KA::Buffer(aux(AUX_BIN_COUNT)), KA::Buffer(aux(AUX_BIN_OFFSET)), KA::Buffer(aux(AUX_TRI_BUF))});

    if (node->level == 0) {
        // brick: the voxels go straight into the atlas (same queue, so the
        // reuse of AUX_VOXELIZE by the next node is ordered after the copy)
        m_volume->copyAtlasBlock(channel, node->value, voxels, dm::uint3(e.ires));
        return 0;
    }

    // interior node: activate the children that received a value
    std::vector<uint8_t> raw(size_t(e.icnt) * elemSize);
    readback(voxels, 0, raw.data(), raw.size());
    std::vector<uint8_t> masks(size_t(e.icnt));
    if (format == ATLAS_FORMAT_R32_FLOAT) {
        const float *v = reinterpret_cast<const float *>(raw.data());
        for (int i = 0; i < e.icnt; ++i) masks[i] = v[i] > valVoidThreshold;
    } else {
        for (int i = 0; i < e.icnt; ++i) masks[i] = float(raw[i]) > valVoidThreshold;
    }
    return uint32_t(m_volume->activateRegionFromMasks(e, masks.data()));
}

}  // namespace

nvrhi::FRESULT createGVDBVoxelizer(IGVDBVolume *volume, IGVDBVoxelizer **voxelizer) {
    if (!volume || !voxelizer) return nvrhi::FE_INVALID_ARGS;
    *voxelizer = MAKE_RC_OBJ(GVDBVoxelizer, volume);
    return nvrhi::FS_OK;
}

}  // namespace gvdb
