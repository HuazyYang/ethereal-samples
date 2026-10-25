// IGVDBVoxelizer: triangle mesh -> topology and voxels. Port of
// VolumeGVDB::SolidVoxelize / InsertTriangles / VoxelizeNode
// (gvdb_volume_gvdb.cpp) through the public IGVDBVolume interface, using the
// gvdbInsertTriangles / gvdbSortTriangles / gvdbVoxelize kernels of
// shaders/GVDBParticles.slang and the prefix sum of GVDBPrimitives.h.
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
#include "GVDBPrimitives.h"
#include <donut/core/log.h>
#include <nvrhi/core/autoptr.h>
#include <vector>
#include <cstring>
#include <algorithm>

namespace gvdb {

using namespace donut;

namespace {

enum AuxId {
    AUX_BIN_COUNT,   // triangles per y bin (reference AUX_GRIDCNT)
    AUX_BIN_OFFSET,  // prefix sum of the bin counts (reference AUX_GRIDOFF)
    AUX_TRI_BUF,     // triangles deep-copied into their bins (3 float3 each)
    AUX_VOXELIZE,    // voxel values of one node (e.icnt elements)
    AUX_MATRIX,      // the model -> index transform (the reference's `cxform` constant)
    MAX_AUX
};

class GVDBVoxelizer : public nvrhi::ObjectImpl<IGVDBVoxelizer> {
 public:
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(GVDBVoxelizer)
    NVRHI_IMPLEMENTS_INTERFACE(IGVDBVoxelizer)
    NVRHI_END_INTERFACE_TABLE()

    explicit GVDBVoxelizer(IGVDBVolume *volume)
        : m_volume(volume), m_context(volume->getContext()), m_primitives(volume->getContext()) {}

    IGVDBVolume *getVolume() const override { return m_volume; }

    nvrhi::FRESULT solidVoxelize(int channel, nvrhi::IBuffer *vertices, uint32_t numVertices, nvrhi::IBuffer *indices,
                                 uint32_t numIndices, const dm::box3 &modelBounds, const dm::affine3 &matModelToIndex,
                                 float valSurface, float valInside) override;

 private:
    nvrhi::IBuffer *prepareAux(AuxId id, uint64_t count, uint32_t stride, bool zero);
    nvrhi::IBuffer *aux(AuxId id) { return m_aux[id].Get(); }

    // Bins the transformed triangles by y with bin height ybdiv. Returns
    // (bins, binned triangles, source triangles).
    dm::int3 insertTriangles(float ybdiv);
    // Voxelizes one node at its child resolution; returns the number of
    // activated children (0 for bricks, whose voxels go to the atlas).
    uint32_t voxelizeNode(Node *node, int channel, float bdiv, float valSurface, float valInside,
                          float valVoidThreshold);

    nvrhi::AutoPtr<IGVDBVolume> m_volume;
    nvrhi::AutoPtr<IComputeContext> m_context;
    GVDBPrimitives m_primitives;
    nvrhi::BufferHandle m_aux[MAX_AUX];

    // per solidVoxelize() call
    nvrhi::IBuffer *m_vertices = nullptr;
    nvrhi::IBuffer *m_indices = nullptr;
    int m_numVertices = 0;
    int m_numTriangles = 0;
    int m_numBins = 0;
    dm::float4x4 m_matModelToIndex = dm::float4x4::identity();   // the `cxform` constant (row vectors)
    dm::box3 m_indexBounds;                                      // transformed, padded model bounds
};

nvrhi::IBuffer *GVDBVoxelizer::prepareAux(AuxId id, uint64_t count, uint32_t stride, bool zero) {
    uint64_t bytes = std::max<uint64_t>(count * stride, 4);
    bytes = (bytes + 3) & ~uint64_t(3);
    m_aux[id] = GVDBPrimitives::ensureBuffer(m_context, m_aux[id], bytes, "GVDB voxelizer");
    nvrhi::IBuffer *buffer = m_aux[id];
    if (zero && buffer) m_context->getCommandList()->clearBufferUInt(buffer, 0);
    return buffer;
}

nvrhi::FRESULT GVDBVoxelizer::solidVoxelize(int channel, nvrhi::IBuffer *vertices, uint32_t numVertices,
                                            nvrhi::IBuffer *indices, uint32_t numIndices, const dm::box3 &modelBounds,
                                            const dm::affine3 &matModelToIndex, float valSurface, float valInside) {
    if (!vertices || !indices || numVertices == 0 || numIndices < 3) return nvrhi::FE_INVALID_ARGS;
    if (channel < 0 || channel >= m_volume->getNumChannels()) return nvrhi::FE_INVALID_ARGS;
    if (m_volume->getNumLevels() == 0) return nvrhi::FE_INVALID_ARGS;

    m_vertices = vertices;
    m_indices = indices;
    m_numVertices = int(numVertices);
    m_numTriangles = int(numIndices / 3);
    m_matModelToIndex = dm::affineToHomogeneous(matModelToIndex);
    nvrhi::IBuffer *matrix = prepareAux(AUX_MATRIX, 16, sizeof(float), false);
    m_context->getCommandList()->writeBuffer(matrix, &m_matModelToIndex, sizeof(m_matModelToIndex), 0);

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

    m_context->finish();
    m_vertices = nullptr;
    m_indices = nullptr;
    return nvrhi::FS_OK;
}

dm::int3 GVDBVoxelizer::insertTriangles(float ybdiv) {
    m_numBins = int(m_indexBounds.upper().y / ybdiv) + 1;
    const int ybins = m_numBins;
    nvrhi::IBuffer *binCount = prepareAux(AUX_BIN_COUNT, ybins, sizeof(uint32_t), true);
    nvrhi::IBuffer *binOffset = prepareAux(AUX_BIN_OFFSET, ybins, sizeof(uint32_t), false);
    nvrhi::ICommandList *commandList = m_context->getCommandList();
    const uint64_t matrix = deviceAddress(aux(AUX_MATRIX));

    // histogram of triangles per bin
    IComputeKernel *insert = m_volume->getKernel("gvdbInsertTriangles");
    launchKernel(commandList, insert, groupsFor(insert, uint64_t(m_numTriangles)), ybdiv, ybins, deviceAddress(binCount),
                 m_numVertices, m_numTriangles, deviceAddress(m_vertices), deviceAddress(m_indices), matrix);

    // bin offsets, total binned triangles
    m_primitives.prefixSum(binCount, binOffset, ybins);
    std::vector<uint32_t> counts(ybins);
    m_context->readBuffer(binCount, 0, counts.data(), ybins * sizeof(uint32_t));
    int triCnt = 0;
    for (int n = 0; n < ybins; ++n) triCnt += int(counts[n]);

    // deep copy of the transformed triangles into their bins
    commandList = m_context->getCommandList();
    commandList->clearBufferUInt(binCount, 0);
    nvrhi::IBuffer *triBuf = prepareAux(AUX_TRI_BUF, std::max(triCnt, 1), 3 * sizeof(dm::float3), false);
    IComputeKernel *sort = m_volume->getKernel("gvdbSortTriangles");
    launchKernel(commandList, sort, groupsFor(sort, uint64_t(m_numTriangles)), ybdiv, ybins, deviceAddress(binCount),
                 deviceAddress(binOffset), triCnt, deviceAddress(triBuf), m_numVertices, m_numTriangles,
                 deviceAddress(m_vertices), deviceAddress(m_indices), matrix);

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

    nvrhi::IBuffer *voxels = prepareAux(AUX_VOXELIZE, uint64_t(e.icnt), elemSize, true);

    IComputeKernel *voxelize = m_volume->getKernel("gvdbVoxelize");
    launchKernel(m_context->getCommandList(), voxelize, groupsFor(voxelize, dm::uint3(e.ires)), e.vmin, e.vmax, e.ires,
                 deviceAddress(voxels), otype, valSurface, valInside, bdiv, m_numBins, deviceAddress(aux(AUX_BIN_COUNT)),
                 deviceAddress(aux(AUX_BIN_OFFSET)), deviceAddress(aux(AUX_TRI_BUF)));

    if (node->level == 0) {
        // brick: the voxels go straight into the atlas (same command list, so the
        // reuse of AUX_VOXELIZE by the next node is ordered after the copy)
        m_volume->copyAtlasBlock(channel, node->value, voxels, dm::uint3(e.ires));
        return 0;
    }

    // interior node: activate the children that received a value
    std::vector<uint8_t> raw(size_t(e.icnt) * elemSize);
    m_context->readBuffer(voxels, 0, raw.data(), raw.size());
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
