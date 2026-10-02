#include <gvdb/GVDB.h>
#include <nvrhi/core/Foundation.h>
#include <nvrhi/core/Foundation.h>
#include <nvrhi/core/AutoPtr.h>
#include <donut/core/vfs/VFS.h>
#include <gvdb/GVDB.h>
#include "SampleTypes.h"
#include "Voxelizer.h"
// NOTE(migration): countof() came from the old ethereal fork's <ethereal/core/math/basics.h>;
// donut has no equivalent, so std::size() is used instead.
#include <iterator>

namespace SampleUtils {

using namespace donut;

constexpr int T_UCHAR = 0;
constexpr int T_FLOAT = 1;
constexpr int T_INT = 2;

enum class BufferName : int32_t {
    // Aux buffers
    BIN_COUNT,
    BIN_OFFSET,
    AUX_ARRAY1,
    AUX_SCAN1,
    AUX_ARRAY2,
    AUX_SCAN2,
    TRI_BUFFER,
    AUX_VOXELIZE,

    NUM_BUFFERNAMES = 40
};

using BN = BufferName;

// TODO(migration): dropped - base `ethereal::UserAllocated` exists in the old
// ethereal fork; nvrhi::UserAllocated has protected operator new/delete.
struct NamedBuffers {
    NamedBuffers(gp::IDevice *device) : m_device{device} {}

    template <BufferName I>
    gp::IBuffer *get() {
        return m_buffers[(int)I];
    }

    template <BufferName I>
    gp::IBuffer *allocate(uint64_t bytes, bool isStaging = false) {
        gp::BufferDesc desc;
        desc.byteSize = bytes;
        desc.isStaging = isStaging;
        UT_V_GP(m_device->createBuffer(desc, &m_buffers[(int)I]));
        return m_buffers[(int)I];
    }

 private:
    nvrhi::AutoPtr<gp::IDevice> m_device;
    nvrhi::AutoPtr<gp::IBuffer> m_buffers[(int)BufferName::NUM_BUFFERNAMES];
};

enum class KN {
    // Sorting / Points / Triangles
    PREFIXSUM,        // prefixSum
    PREFIXFIXUP,      // prefixFixup
    INSERT_POINTS,    // gvdbInsertPoints
    SORT_POINTS,      // gvdbSortPoints
    SCATTER_DENSITY,  // gvdbScatterPointDensity
    SCATTER_AVG_COL,  // gvdbScatterPointAvgCol
    INSERT_TRIS,      // gvdbInsertTriangles
    SORT_TRIS,        // gvdbSortTriangles
    VOXELIZE,         // gvdbVoxelize
    RESAMPLE,         // gvdbResample
    REDUCTION,        // gvdbReduction
    DOWNSAMPLE,       // gvdbDownsample
    SCALE_PNT_POS,    // gvdbScalePntPos
    CONV_AND_XFORM,   // gvdbConvAndTransform

    // ADD_SUPPORT_VOXEL,      // gvdbAddSupportVoxel
    // INSERT_SUPPORT_POINTS,  // gvdbInsertSupportPoints

    // // Topology
    // FIND_ACTIV_BRICKS,  // gvdbFindActivBricks
    // BITONIC_SORT,       // gvdbBitonicSort
    // CALC_BRICK_ID,      // gvdbCalcBrickId
    // FIND_UNIQUE,        // gvdbFindUnique
    // COMPACT_UNIQUE,     // gvdbCompactUnique
    // LINK_BRICKS,        // gvdbLinkBricks

    // // Incremental Topology
    // CALC_EXTRA_BRICK_ID,  // gvdbCalcExtraBrickId

    // CALC_INCRE_BRICK_ID,        // gvdbCalcIncreBrickId
    // CALC_INCRE_EXTRA_BRICK_ID,  // gvdbCalcIncreExtraBrickId

    // DELINK_LEAF_BRICKS,  // gvdbDelinkLeafBricks
    // DELINK_BRICKS,       // gvdbDelinkBricks
    // MARK_LEAF_NODE,      // gvdbMarkLeafNode

    // // Gathering
    // COUNT_SUBCELL,         // gvdbCountSubcell
    // INSERT_SUBCELL,        // gvdbInsertSubcell
    // INSERT_SUBCELL_FP16,   // gvdbInsertSubcell_fp16
    // GATHER_DENSITY,        // gvdbGatherDensity
    // GATHER_LEVELSET,       // gvdbGatherLevelSet
    // GATHER_LEVELSET_FP16,  // gvdbGatherLevelSet_fp16

    // CALC_SUBCELL_POS,  // gvdbCalcSubcellPos
    // MAP_EXTRA_GVDB,    // gvdbMapExtraGVDB
    // SPLIT_POS,         // gvdbSplitPos
    // SET_FLAG_SUBCELL,  // gvdbSetFlagSubcell

    // READ_GRID_VEL,  // gvdbReadGridVel
    // CHECK_VAL,      // gvdbCheckVal

    // // Operators
    // FILL_F,      // gvdbOpFillF
    // FILL_C,      // gvdbOpFillC
    // FILL_C4,     // gvdbOpFillC4
    // SMOOTH,      // gvdbOpSmooth
    // NOISE,       // gvdbOpNoise
    // CLR_EXPAND,  // gvdbOpClrExpand
    // EXPANDC,     // gvdbOpExpandC

    // COPYDATA_FILL_TEX,          // kernelFillTex
    // COPYDATA_COPY_TEXC,         // kernelCopyTexC
    // COPYDATA_COPY_TEXF,         // kernelCopyTexF
    // COPYDATA_COPY_BUF_TO_TEXC,  // kernelCopyBufToTexC
    // COPYDATA_COPY_BUF_TO_TEXF,  // kernelCopyBufToTexF
    // COPYDATA_COPY_TEXZYX,       // kernelCopyTexZYX
    // COPYDATA_RETRIEVE_TEXXYZ,   // kernelRetrieveTexXYZ
    NUM_KN
};

class NamedKernels : public nvrhi::ObjectImpl<nvrhi::IObject> {
 public:
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(NamedKernels)
    NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IObject)
    NVRHI_END_INTERFACE_TABLE()

    void load(donut::vfs::IFileSystem *vfs, donut::gp::IDevice *device);
    ~NamedKernels();

    template <KN I>
    donut::gp::IKernel *get() const;

 private:
    nvrhi::AutoPtr<donut::gp::IKernel> m_kernels[(int)KN::NUM_KN];
};

template <KN I>
inline donut::gp::IKernel *NamedKernels::get() const {
    if ((int)I > (int)KN::NUM_KN) return nullptr;
    return m_kernels[(int)I].Get();
}

void NamedKernels::load(donut::vfs::IFileSystem *vfs, donut::gp::IDevice *device) {
    nvrhi::AutoPtr<nvrhi::IDataBlob> pLibBlob;
    UT_V_GP(vfs->readFile("sample_utils/kernels/cuda_gvdb_particles.cu", &pLibBlob));
    size_t libBlobSize = pLibBlob->GetSize();
    pLibBlob->Resize(libBlobSize + 1);
    ((uint8_t *)pLibBlob->GetDataPtr())[libBlobSize] = 0;

    nvrhi::AutoPtr<donut::gp::IModule> gvdbModule;
    UT_V_GP(device->createModule({}, pLibBlob->GetDataPtr(), pLibBlob->GetSize(),
                                 &gvdbModule));

    struct KernelNamePair {
        KN index;
        const char *name;
    } kernelNamePairs[] = {
        // clang-format off

        // Sorting / Points / Triangles
        { KN::PREFIXSUM,        "prefixSum" },
        { KN::PREFIXFIXUP,      "prefixFixup" },
        { KN::INSERT_POINTS,    "gvdbInsertPoints" },
        { KN::SORT_POINTS,      "gvdbSortPoints" },
        { KN::SCATTER_DENSITY,  "gvdbScatterPointDensity" },
        { KN::SCATTER_AVG_COL,  "gvdbScatterPointAvgCol" },
        { KN::INSERT_TRIS,      "gvdbInsertTriangles" },
        { KN::SORT_TRIS,        "gvdbSortTriangles" },
        { KN::VOXELIZE,         "gvdbVoxelize" },
        { KN::RESAMPLE,         "gvdbResample" },
        { KN::REDUCTION,        "gvdbReduction" },
        { KN::DOWNSAMPLE,       "gvdbDownsample" },
        { KN::SCALE_PNT_POS,    "gvdbScalePntPos" },
        { KN::CONV_AND_XFORM,   "gvdbConvAndTransform" },

        // { KN::ADD_SUPPORT_VOXEL,      "gvdbAddSupportVoxel" },
        // { KN::INSERT_SUPPORT_POINTS,  "gvdbInsertSupportPoints" },

        // // Topology
        // { KN::FIND_ACTIV_BRICKS,  "gvdbFindActivBricks" },
        // { KN::BITONIC_SORT,       "gvdbBitonicSort" },
        // { KN::CALC_BRICK_ID,      "gvdbCalcBrickId" },
        // { KN::FIND_UNIQUE,        "gvdbFindUnique" },
        // { KN::COMPACT_UNIQUE,     "gvdbCompactUnique" },
        // { KN::LINK_BRICKS,        "gvdbLinkBricks" },

        // // Incremental Topology
        // { KN::CALC_EXTRA_BRICK_ID,  "gvdbCalcExtraBrickId" },

        // { KN::CALC_INCRE_BRICK_ID,        "gvdbCalcIncreBrickId" },
        // { KN::CALC_INCRE_EXTRA_BRICK_ID,  "gvdbCalcIncreExtraBrickId" },

        // { KN::DELINK_LEAF_BRICKS,  "gvdbDelinkLeafBricks" },
        // { KN::DELINK_BRICKS,       "gvdbDelinkBricks" },
        // { KN::MARK_LEAF_NODE,      "gvdbMarkLeafNode" },

        // // Gathering
        // { KN::COUNT_SUBCELL,         "gvdbCountSubcell" },
        // { KN::INSERT_SUBCELL,        "gvdbInsertSubcell" },
        // { KN::INSERT_SUBCELL_FP16,   "gvdbInsertSubcell_fp16" },
        // { KN::GATHER_DENSITY,        "gvdbGatherDensity" },
        // { KN::GATHER_LEVELSET,       "gvdbGatherLevelSet" },
        // { KN::GATHER_LEVELSET_FP16,  "gvdbGatherLevelSet_fp16" },

        // { KN::CALC_SUBCELL_POS,  "gvdbCalcSubcellPos" },
        // { KN::MAP_EXTRA_GVDB,    "gvdbMapExtraGVDB" },
        // { KN::SPLIT_POS,         "gvdbSplitPos" },
        // { KN::SET_FLAG_SUBCELL,  "gvdbSetFlagSubcell" },

        // { KN::READ_GRID_VEL,  "gvdbReadGridVel" },
        // { KN::CHECK_VAL,      "gvdbCheckVal" },
        // clang-format on
    };

    for (auto &kernelNamePair : kernelNamePairs) {
        UT_V_GP(gvdbModule->getKernel(kernelNamePair.name,
                                      &m_kernels[(int)kernelNamePair.index]));
    }
}

NamedKernels::~NamedKernels() {}

struct Voxelizer : public nvrhi::ObjectImpl<IVoxelizer> {
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Voxelizer)
    NVRHI_IMPLEMENTS_INTERFACE(IVoxelizer)
    NVRHI_END_INTERFACE_TABLE()

    nvrhi::FRESULT solidVoxelize(gvdb::GVDB *pGVDB, int channel,
                                    donut::gp::IBuffer *pVertBuffer,
                                    donut::gp::IBuffer *pIndexBuffer,
                                    int numIndices, const dm::box3 &modelBounds,
                                    const dm::affine3 &matModelToVolume, float valSurface,
                                    float valInside) override;

    // Implements
    Voxelizer(gp::IDevice *device, donut::vfs::IFileSystem *vfs);
    ~Voxelizer();

    void computeIndexspaceBounds(const dm::box3 &modelBounds, const dm::affine3 &matModelToIndexspace);

    dm::int3 insertTriangles(float ybdiv);

    void prefixSum(donut::gp::IBuffer *inBuffer, donut::gp::IBuffer *outBuffer,
                   uint32_t numElem);

    uint32_t voxelizeNode(gvdb::GVDB *pGVDB, gvdb::Node *node, float bdiv, float valSurface,
                          float valInside, float valVoidThreshold);

    nvrhi::AutoPtr<donut::gp::IDevice> m_device;
    nvrhi::AutoPtr<donut::gp::IDeviceQueue> m_queue;
    NamedKernels m_kernels;
    NamedBuffers m_buffers;

    donut::gp::IBuffer *m_vertBuffer;
    donut::gp::IBuffer *m_indexBuffer;
    int m_numIndices;

    dm::float4x4 m_matModelToVolume;  // poly-to-voxel transform

    dm::box3 m_volumeBounds;
    dm::box<int, 3> m_voxLeafBounds;
};

Voxelizer::Voxelizer(gp::IDevice *device, donut::vfs::IFileSystem *vfs)
    : m_device(device), m_kernels{}, m_buffers(device) {
    gp::DeviceQueueDesc queueDesc;
    queueDesc.priority = gp::DeviceQueuePriority::Normal;
    UT_V_GP(m_device->createDeviceQueue(queueDesc, &m_queue));

    m_kernels.load(vfs, m_device);
}

Voxelizer::~Voxelizer() {}

void Voxelizer::computeIndexspaceBounds(const dm::box3 &modelBounds,
                                                const dm::affine3 &matModelToIndexspace) {
    dm::float3 e = modelBounds.diagonal() * 0.5f;
    dm::float3 c = modelBounds.center();

    dm::affine3 matAbs{dm::abs(matModelToIndexspace.m_linear[0]),
                       dm::abs(matModelToIndexspace.m_linear[1]),
                       dm::abs(matModelToIndexspace.m_linear[2]), dm::float3{0.f}};
    dm::float3 e2 = matAbs.transformVector(e);
    dm::float3 c2 = matModelToIndexspace.transformPoint(c);

    e2 *= 1.1f;

    m_volumeBounds = {c2 - e2, c2 + e2};
    m_matModelToVolume = dm::affineToHomogeneous(matModelToIndexspace);
}

nvrhi::FRESULT Voxelizer::solidVoxelize(
    gvdb::GVDB *pGVDB, int channel, donut::gp::IBuffer *pVertBuffer,
    donut::gp::IBuffer *pIndexBuffer, int numIndices,
    const dm::box3 &modelBounds, const dm::affine3 &matModelToVolume, float valSurface,
    float valInside) {
    m_vertBuffer = pVertBuffer;
    m_indexBuffer = pIndexBuffer;
    m_numIndices = numIndices;

    pGVDB->clear();

    // Compute bounds in index-space
    computeIndexspaceBounds(modelBounds, matModelToVolume);

    const uint8_t N = pGVDB->getNumLevels();
    gvdb::Extents e = pGVDB->computeExtents(N, m_volumeBounds);
    pGVDB->activateRegion(e);

    // Voxel at each level
    int node_cnt, cnt;
    gvdb::Node *node;
    for (int lev = N - 1; lev >= 0; --lev) {
        int node_cnt = pGVDB->getNumUsedNodes(lev);

        // Insert triangle into bins
        float ybinDiv = pGVDB->getCover(lev).y;
        auto tcnts = insertTriangles(ybinDiv);

        // Voxelize each node at this level
        cnt = 0;
        for (int n = 0; n < node_cnt; ++n) {
            node = pGVDB->getNode(lev, n);
            cnt += voxelizeNode(pGVDB, node, ybinDiv, valSurface, valInside,
                                0.f);
        }

        if (lev == 1) {
            pGVDB->finishTopology();
            pGVDB->updateAtlas();
        }
        log::info("Voxelized.. lev: %d, nodes: %d, new: %d", lev, node_cnt, cnt);
    }

    pGVDB->updateApron();

    m_device->commitQueue(m_queue);
    m_device->waitForQueue(m_queue);

    return nvrhi::FS_OK;
}

dm::int3 Voxelizer::insertTriangles(float ybdiv) {
    int ybins = int(m_volumeBounds.upper().y / ybdiv) + 1;

    gp::IBuffer *buffer;
    buffer = m_buffers.allocate<BN::BIN_COUNT>(ybins * sizeof(uint32_t));
    m_queue->clearBufferUint(buffer, 0);
    buffer = m_buffers.allocate<BN::BIN_OFFSET>(ybins * sizeof(uint32_t));

    int vcnt = m_vertBuffer->getDesc()->byteSize / sizeof(dm::float3);
    int ecnt = m_indexBuffer->getDesc()->byteSize / sizeof(dm::uint3);

    gp::dim3 block{512, 1, 1};
    gp::dim3 grid{dm::div_ceil(ecnt, block.x), 1, 1};
    {
        gp::KernelArg args[] = {gp::KernelArg::Scalar(ybdiv),
                                gp::KernelArg::Scalar(ybins),
                                gp::KernelArg::Buffer(m_buffers.get<BN::BIN_COUNT>()),
                                gp::KernelArg::Scalar(vcnt),
                                gp::KernelArg::Scalar(ecnt),
                                gp::KernelArg::Buffer(m_vertBuffer),
                                gp::KernelArg::Buffer(m_indexBuffer)};
        m_queue->setConstantBuffer(m_kernels.get<KN::INSERT_TRIS>(), "cxform",
                                     &m_matModelToVolume, sizeof(m_matModelToVolume));
        m_queue->launch(m_kernels.get<KN::INSERT_TRIS>(), grid, block, args,
                          std::size(args));
    }

    // prefix sum for bin offsets
    prefixSum(m_buffers.get<BN::BIN_COUNT>(), m_buffers.get<BN::BIN_OFFSET>(), ybins);

    nvrhi::AutoPtr<gp::IBuffer> auxReadbackBuffer;
    gp::BufferDesc auxBufDesc;
    auxBufDesc.byteSize = m_buffers.get<BN::BIN_COUNT>()->getDesc()->byteSize;
    auxBufDesc.isStaging = true;
    UT_V_GP(m_device->createBuffer(auxBufDesc, &auxReadbackBuffer));

    m_queue->copyBufferRegion(auxReadbackBuffer, 0, m_buffers.get<BN::BIN_COUNT>(), 0, 0);
    m_device->commitQueue(m_queue);
    m_device->waitForQueue(m_queue);

    // Readback bin count array
    uint32_t *cnt;
    int32_t tri_cnt = 0;
    m_device->mapBuffer(auxReadbackBuffer, (void **)&cnt);
    for (int n = 0; n < ybins; ++n)
        tri_cnt += cnt[n];
    m_device->unmapBuffer(auxReadbackBuffer);
    auxReadbackBuffer = nullptr;

    m_queue->clearBufferUint(m_buffers.get<BN::BIN_COUNT>(), 0);

    // prepare output triangle buffer
    m_buffers.allocate<BN::TRI_BUFFER>(tri_cnt * (sizeof(dm::float3) * 3));

    gp::KernelArg args2[] = {gp::KernelArg::Scalar(ybdiv),
                             gp::KernelArg::Scalar(ybins),
                             gp::KernelArg::Buffer(m_buffers.get<BN::BIN_COUNT>()),
                             gp::KernelArg::Buffer(m_buffers.get<BN::BIN_OFFSET>()),
                             gp::KernelArg::Scalar(tri_cnt),
                             gp::KernelArg::Buffer(m_buffers.get<BN::TRI_BUFFER>()),
                             gp::KernelArg::Scalar(vcnt),
                             gp::KernelArg::Scalar(ecnt),
                             gp::KernelArg::Buffer(m_vertBuffer),
                             gp::KernelArg::Buffer(m_indexBuffer)};
    // copy sorted tris into output buffer
    m_queue->launch(m_kernels.get<KN::SORT_TRIS>(), grid, block, args2,
                      std::size(args2));

    return dm::int3{ybins, tri_cnt, ecnt};
}

void Voxelizer::prefixSum(gp::IBuffer *inBuffer, gp::IBuffer *outBuffer,
                              uint32_t numElem) {
    NVRHI_ASSERT(numElem < (1 << 30) && "Element number exceed maximum bound");
    constexpr int naux = GVDB_SCAN_BLOCKSIZE << 1;  // must be 1024
    int grid1 = dm::div_ceil(int(numElem), naux);
    int grid2 = dm::div_ceil(grid1, naux);
    constexpr gp::dim3 blockDim = {GVDB_SCAN_BLOCKSIZE, 1, 1};
    constexpr int zon = 1;

    gp::BufferDesc auxBufDesc;
    uint64_t byteSize = grid1 * sizeof(uint32_t);
    m_buffers.allocate<BN::AUX_ARRAY1>(byteSize);
    m_buffers.allocate<BN::AUX_SCAN1>(byteSize);
    byteSize = grid2 * sizeof(uint32_t);
    m_buffers.allocate<BN::AUX_ARRAY2>(byteSize);
    m_buffers.allocate<BN::AUX_SCAN2>(byteSize);

    gp::KernelArg argsA[] = {gp::KernelArg::Buffer(inBuffer),
                             gp::KernelArg::Buffer(outBuffer),
                             gp::KernelArg::Buffer(m_buffers.get<BN::AUX_ARRAY1>()),
                             gp::KernelArg::Scalar(numElem), gp::KernelArg::Scalar(zon)};
    m_queue->launch(m_kernels.get<KN::PREFIXSUM>(), gp::dim3{grid1, 1, 1},
                      blockDim, argsA, std::size(argsA));

    if (grid1 > 1) {
        gp::KernelArg argsB[] = {gp::KernelArg::Buffer(m_buffers.get<BN::AUX_ARRAY1>()),
                                 gp::KernelArg::Buffer(m_buffers.get<BN::AUX_SCAN1>()),
                                 gp::KernelArg::Buffer(m_buffers.get<BN::AUX_ARRAY2>()),
                                 gp::KernelArg::Scalar(grid1), gp::KernelArg::Scalar(zon)};
        m_queue->launch(m_kernels.get<KN::PREFIXSUM>(), gp::dim3{grid2, 1, 1},
                          blockDim, argsB, std::size(argsB));

        if (grid2 > 1) {
            gp::KernelArg argsC[] = {gp::KernelArg::Buffer(m_buffers.get<BN::AUX_ARRAY2>()),
                                     gp::KernelArg::Buffer(m_buffers.get<BN::AUX_SCAN2>()),
                                     gp::KernelArg::Buffer(nullptr),
                                     gp::KernelArg::Scalar(grid2),
                                     gp::KernelArg::Scalar(zon)};
            m_queue->launch(m_kernels.get<KN::PREFIXSUM>(), gp::dim3{1, 1, 1},
                              blockDim, argsC, std::size(argsC));

            gp::KernelArg argsD[] = {gp::KernelArg::Buffer(m_buffers.get<BN::AUX_SCAN1>()),
                                     gp::KernelArg::Buffer(m_buffers.get<BN::AUX_SCAN2>()),
                                     gp::KernelArg::Scalar(grid1)};
            m_queue->launch(m_kernels.get<KN::PREFIXFIXUP>(),
                              gp::dim3{grid2, 1, 1}, blockDim, argsD, std::size(argsD));
        }

        gp::KernelArg argsE[] = {gp::KernelArg::Buffer(outBuffer),
                                 gp::KernelArg::Buffer(m_buffers.get<BN::AUX_SCAN1>()),
                                 gp::KernelArg::Scalar(numElem)};
        m_queue->launch(m_kernels.get<KN::PREFIXFIXUP>(), gp::dim3{grid1, 1, 1},
                          blockDim, argsE, std::size(argsE));
    }
}

uint32_t Voxelizer::voxelizeNode(gvdb::GVDB *pGVDB, gvdb::Node *node, float bdiv,
                                     float valSurface, float valInside,
                                     float valVoidThreshold) {

    uint32_t cnt = 0;
    auto e = pGVDB->computeExtents(node);
    int bmax = m_buffers.get<BN::BIN_OFFSET>()->getDesc()->byteSize / sizeof(int);

    const int elemSize = sizeof(float);

    // each child voxel has a value represents surface(outside), inside, or void
    auto buffer = m_buffers.allocate<BN::AUX_VOXELIZE>(e.icnt * elemSize);
    m_queue->clearBufferUint(buffer, *(const uint32_t *)&valVoidThreshold);

    gp::dim3 blockDim{8, 8, 8};
    gp::dim3 gridDim{dm::div_ceil(e.ires.x, blockDim.x), dm::div_ceil(e.ires.y, blockDim.y),
                     dm::div_ceil(e.ires.z, blockDim.z)};
    gp::KernelArg args[] = {gp::KernelArg::Scalar(e.vmin),
                            gp::KernelArg::Scalar(e.vmax),
                            gp::KernelArg::Scalar(e.ires),
                            gp::KernelArg::Buffer(m_buffers.get<BN::AUX_VOXELIZE>()),
                            gp::KernelArg::Scalar(1),
                            gp::KernelArg::Scalar(valSurface),
                            gp::KernelArg::Scalar(valInside),
                            gp::KernelArg::Scalar(bdiv),
                            gp::KernelArg::Scalar(bmax),
                            gp::KernelArg::Buffer(m_buffers.get<BN::BIN_COUNT>()),
                            gp::KernelArg::Buffer(m_buffers.get<BN::BIN_OFFSET>()),
                            gp::KernelArg::Buffer(m_buffers.get<BN::TRI_BUFFER>())};

    m_queue->launch(m_kernels.get<KN::VOXELIZE>(), gridDim, blockDim, args,
                      std::size(args));

    if (node->level == 0) {
        pGVDB->copyAtlasBlock(0, node->value, m_buffers.get<BN::AUX_VOXELIZE>(),
                              dm::uint3{e.ires});

        //   We may use AUX_VOXELIZE next time, so synchronize queue here
        m_device->commitQueue(m_queue);
        m_device->waitForQueue(m_queue);
    } else {
        // retrieve children voxel values
        gp::BufferDesc bufDesc;
        nvrhi::AutoPtr<gp::IBuffer> voxValBuff;
        bufDesc.byteSize = m_buffers.get<BN::AUX_VOXELIZE>()->getDesc()->byteSize;
        bufDesc.isStaging = true;
        UT_V_GP(m_device->createBuffer(bufDesc, &voxValBuff));
        m_queue->copyBufferRegion(voxValBuff, 0, m_buffers.get<BN::AUX_VOXELIZE>(), 0, 0);
        m_device->commitQueue(m_queue);
        m_device->waitForQueue(m_queue);

        void *masksInput;
        UT_V_GP(m_device->mapBuffer(voxValBuff, &masksInput));
        std::vector<uint8_t> childrenMasks(size_t(e.icnt));

        float *maskInput2 = (float *)masksInput;
        for (int z = 0; z < e.ires.z; ++z)
            for (int y = 0; y < e.ires.y; ++y)
                for (int x = 0; x < e.ires.x; ++x) {
                    int voxIdx = (z * e.ires.y + y) * e.ires.x + x;
                    float vset = maskInput2[voxIdx];
                    childrenMasks[voxIdx] = (vset > valVoidThreshold);
                }

        m_device->unmapBuffer(voxValBuff);
        voxValBuff = nullptr;

        // TODO: pass node in may accelerate hierarchy lookup
        cnt = pGVDB->activateRegionFromMasks(e, childrenMasks.data());
    }

    return cnt;
}

nvrhi::FRESULT createVoxelizer(donut::gp::IDevice *device,
                                  donut::vfs::IFileSystem *vfs, IVoxelizer **ppVoxelizer) {
    auto pVoxelizer = MAKE_RC_OBJ(Voxelizer, device, vfs);
    if (ppVoxelizer) *ppVoxelizer = pVoxelizer;
    return nvrhi::FS_OK;
}

}  // namespace SampleUtils