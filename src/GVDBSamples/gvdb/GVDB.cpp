#include <gvdb/GVDB.h>
#include "GVDBAllocator.h"
#include <donut/core/log.h>
#include <donut/core/vfs/VFS.h>
// NOTE(migration): countof() came from the old ethereal fork's <ethereal/core/math/basics.h>;
// donut has no equivalent, so std::size() is used instead.
#include <iterator>

namespace gvdb {

using namespace donut;

#define GVDB_V_GP(expr)                                                      \
    do {                                                                     \
        auto rc = (expr);                                                    \
        if (FFAILED(rc)) {                                                   \
            donut::log::error("GPDevice failed with error: %d", (int)rc); \
            DONUT_ASSERT(0);                                              \
        }                                                                    \
    } while (0)

namespace bitop {

// Bit operations:
// Based on Bithacks (Sean Eron Anderson,
// http://graphics.stanford.edu/~seander/bithacks.html)
//
inline uint64_t numBitsOn(uint8_t v) {
    static const uint8_t numBits[256] = {
#define B2(n) n, n + 1, n + 1, n + 2
#define B4(n) B2(n), B2(n + 1), B2(n + 1), B2(n + 2)
#define B6(n) B4(n), B4(n + 1), B4(n + 1), B4(n + 2)
        B6(0), B6(1), B6(1), B6(2)};
    return numBits[v];
}
inline uint64_t numBitsOff(uint8_t v) { return numBitsOn((uint8_t)~v); }
inline uint64_t numBitsOn(uint32_t v) {
    v = v - ((v >> 1) & 0x55555555U);
    v = (v & 0x33333333U) + ((v >> 2) & 0x33333333U);
    return ((v + (v >> 4) & 0xF0F0F0FU) * 0x1010101U) >> 24;
}
inline uint64_t numBitsOff(uint32_t v) { return numBitsOn(~v); }
inline uint64_t numBitsOn(uint64_t v) {
    v = v - ((v >> 1) & UINT64_C(0x5555555555555555));
    v = (v & UINT64_C(0x3333333333333333)) + ((v >> 2) & UINT64_C(0x3333333333333333));
    return ((v + (v >> 4) & UINT64_C(0xF0F0F0F0F0F0F0F)) * UINT64_C(0x101010101010101)) >>
           56;
}
inline uint64_t numBitsOff(uint64_t v) { return numBitsOn((uint64_t)~v); }
inline uint64_t firstBitOn(uint8_t v) {
    assert(v);  // make sure not 0
    static const uint8_t DeBruijn[8] = {0, 1, 6, 2, 7, 5, 4, 3};
    return DeBruijn[uint8_t((v & -v) * 0x1DU) >> 5];
}
inline uint64_t firstBitOn(uint32_t v) {
    assert(v);
    static const uint8_t DeBruijnBitPos[32] = {0,  1,  28, 2,  29, 14, 24, 3,  30, 22, 20,
                                            15, 25, 17, 4,  8,  31, 27, 13, 23, 21, 19,
                                            16, 7,  26, 12, 18, 6,  11, 5,  10, 9};
    return DeBruijnBitPos[uint32_t((int(v) & -int(v)) * 0x077CB531U) >> 27];
}
inline uint64_t firstBitOn(uint64_t v) {
    assert(v);
    static const uint8_t DeBruijn[64] = {
        0,  1,  2,  53, 3,  7,  54, 27, 4,  38, 41, 8,  34, 55, 48, 28,
        62, 5,  39, 46, 44, 42, 22, 9,  24, 35, 59, 56, 49, 18, 29, 11,
        63, 52, 6,  26, 37, 40, 33, 47, 61, 45, 43, 21, 23, 58, 17, 10,
        51, 25, 36, 32, 60, 20, 57, 16, 50, 31, 19, 15, 30, 14, 13, 12,
    };
    return DeBruijn[uint64_t((int64_t(v) & -int64_t(v)) * UINT64_C(0x022FDD63CC95386D)) >> 58];
}
inline uint64_t lastBitOn(uint32_t v) {
    static const uint8_t DeBruijn[32] = {0,  9,  1,  10, 13, 21, 2,  29, 11, 14, 16,
                                      18, 22, 25, 3,  30, 8,  12, 20, 28, 15, 17,
                                      24, 7,  19, 27, 23, 6,  26, 5,  4,  31};
    v |= v >> 1;  // first round down to one less than a power of 2
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return DeBruijn[uint32_t(v * 0x07C4ACDDU) >> 27];
}

void clearMask(Node* node, uint32_t b) {
    memset(&node->mask, 0, b >> 3);
}
// set operator
void set(Node* node, Node* op2, uint32_t b) {
    uint64_t* w1 = (uint64_t*)&node->mask;
    uint64_t* we = (uint64_t*)&node->mask + (b >> 6);
    uint64_t* w2 = (uint64_t*)&op2->mask;
    for (; w1 != we;)
        *w1++ = *w2++;
}
bool isEqual(Node* node, Node* op2, uint32_t b) {
    int sz = (int)(b >> 6);
    uint64_t* w1 = (uint64_t*)&node->mask;
    uint64_t* we = (uint64_t*)&node->mask + sz;
    uint64_t* w2 = (uint64_t*)&op2->mask;
    for (; w1 != we && (*w1++ == *w2++);)
        ;
    return w1 == we;
}
uint64_t countOn(Node* node, uint32_t b) {
    uint64_t sum = 0;
    uint64_t* w1 = (uint64_t*)&node->mask;
    uint64_t* we = (uint64_t*)&node->mask + (b >> 6);
    for (; w1 != we;)
        sum += (int)numBitsOn(*w1++);
    if(b & 63) {
        uint64_t w2 = *w1;
        w2 = w2 & ((uint64_t(1) << (b & 63)) - 1);
        sum += numBitsOn(w2);
    }
    return sum;
}
uint64_t countOff(Node* node, uint32_t b) {
    return b * 8 - countOn(node, b);
}

void setAll(Node* node, uint32_t b, bool on) {
    const uint64_t val = on ? ~uint64_t(0) : uint64_t(0);
    uint64_t* w1 = (uint64_t*)&node->mask;
    uint64_t* we = (uint64_t*)&node->mask + (b >> 6);
    for (; w1 != we;)
        *w1++ = val;
}
void setOn(Node* node, uint32_t n) { (&node->mask)[n >> 6] |= uint64_t(1) << (n & 63); }
void setOff(Node* node, uint32_t n) { (&node->mask)[n >> 6] &= ~(uint64_t(1) << (n & 63)); }
bool isOn(Node* node, uint64_t n) {
    return (((uint64_t*)&node->mask)[n >> 6] & (uint64_t(1) << (n & 63))) != 0;
}
bool isOff(Node* node, uint64_t n) {
    return (((uint64_t*)&node->mask)[n >> 6] & (uint64_t(1) << (n & 63))) == 0;
}

uint32_t countToIndex(Node* node, uint32_t b, uint64_t count) {
    uint64_t sum = 0;
    uint64_t* w1 = (uint64_t*)&node->mask;
    uint64_t* we = (uint64_t*)&node->mask + (b >> 6);
    uint32_t bits = 0;
    count++;
    for (; sum < count; bits++) {
        if (isOn(node, bits)) sum++;
    }
    return bits - 1;
}

}

constexpr NodeId NodeId::null() { return {}; }

// TODO(migration): dropped - ETHEREAL_NEW (leak-tracking placement new) exists in
// the old ethereal fork but not in donut; plain operator new is used instead.
GVDB::GVDB(gp::IDeviceQueue *queue, donut::vfs::IFileSystem *vfs)
    : m_gpQueue(queue), m_allocator(new GVDBAllocator()) {
    configAtlas({16u, 16u, 1u}, 1);

    AutoPtr<IDataBlob> pBlob;
    AutoPtr<gp::IModule> pModule;

    GVDB_V_GP(vfs->readFile("gvdb/GVDBUpdateApron.cu", &pBlob));
    size_t len = pBlob->GetSize();
    pBlob->Resize(len + 1);
    ((char*)pBlob->GetDataPtr())[len] = 0;

    GVDB_V_GP(m_gpQueue->getDevice()->createModule({}, pBlob, pBlob->GetSize(), &pModule));

    GVDB_V_GP(pModule->getKernel("gvdbUpdateApronC", &m_updateApronKernels[0]));
    GVDB_V_GP(pModule->getKernel("gvdbUpdateApronC4", &m_updateApronKernels[1]));
    GVDB_V_GP(pModule->getKernel("gvdbUpdateApronF", &m_updateApronKernels[2]));
    GVDB_V_GP(pModule->getKernel("gvdbUpdateApronF4", &m_updateApronKernels[3]));
}

GVDB::~GVDB() {}

void GVDB::configLevels(int r4, int r3, int r2, int r1, int r0) {
    uint32_t levelLogDims[5];
    levelLogDims[0] = r0;
    levelLogDims[1] = r1;
    levelLogDims[2] = r2;
    levelLogDims[3] = r3;
    levelLogDims[4] = r4;
    uint32_t numcnt[] = {4, 4, 2, 1, 1};
    configLevels(std::size(levelLogDims), levelLogDims, numcnt);
}

void GVDB::configAtlas(dm::uint3 defaultAtlasGridDim, int apron) {
    uint32_t blockDim;
    if(!m_levelInfos.empty()) {
        blockDim = getResDim(0);
    } else
        blockDim = 0;
    m_atlasMapInfo.defaultGridDim = defaultAtlasGridDim;
    m_atlasMapInfo.apron = apron;

    m_atlasGridDim = defaultAtlasGridDim;
    m_atlasBrickDim = blockDim;
    m_atlasBrickDimWithApron = blockDim + 2 * apron;
}

void GVDB::addChannel(const GVDBAtlasResourceDesc &resourceDesc) {
    if(m_atlasResourceChannels.size() < MAX_CHANNEL) {
        m_atlasResourceChannels.push_back(resourceDesc);
    }
}

void GVDB::destroyChannels() {
    m_atlasResourceChannels.clear();
    m_atlasResourceChannelsGPU.clear();
    m_atlasMap.clear();
    m_atlasMapGPU = nullptr;
}

void GVDB::clear() {
    m_allocator->poolClearAll();
    m_nodePoolsGPU.clear();
    m_childIdPoolsGPU.clear();

    m_rootNodeId = NodeId::null();
}

dm::uint GVDB::getApron() {
    return m_atlasMapInfo.apron.x;
}

dm::uint3 GVDB::getAtlasBlockDim() {
    return m_atlasBrickDim;
}

dm::uint3 GVDB::getAtlasBlockDimWithoutApron() {
    return m_atlasBrickDimWithApron;
}

dm::uint3 GVDB::getAtlasGridDim() {
    return m_atlasGridDim;
}

dm::uint3 GVDB::getAtlasResDim() {
    return m_atlasGridDim * m_atlasBrickDimWithApron;
}

void GVDB::configLevels(uint32_t numLevels, const uint32_t* levelLogDims, const uint32_t *numcnt, bool useBitmasks) {
    std::vector<int> numReservedNodes((size_t)numLevels);

    m_levelInfos.clear();

    for(uint32_t i = 0; i < numLevels; ++i) {
        numReservedNodes[i] = numcnt[i] == 0 ? 1 : numcnt[i];

        LevelInfo levelInfo;
        levelInfo.logDim = levelLogDims[i] == 0 ? 1 : levelLogDims[i];
        uint32_t childGridDim = 1 << levelInfo.logDim;
        levelInfo.voxDim = childGridDim;
        levelInfo.numVoxel = childGridDim * childGridDim * childGridDim;
        levelInfo.range = i == 0 ? levelInfo.voxDim : m_levelInfos[i-1].range * childGridDim;
        levelInfo.bitmasksBits = i == 0 ? 0 : ((levelInfo.numVoxel + 63) & ~63);

        m_levelInfos.push_back(levelInfo);
    }

    m_allocator->poolReleaseAll();

    uint64_t nodeElementWidth;
    for(uint32_t i = 0; i < numLevels; ++i) {
        nodeElementWidth = sizeof(Node);
        if (useBitmasks) nodeElementWidth += (m_levelInfos[i].bitmasksBits >> 3);
        m_allocator->poolCreate(0, i, nodeElementWidth, numReservedNodes[i]);
    }

    m_allocator->poolCreate(1, 0, 0, numReservedNodes[0]);
    for(uint32_t i = 1; i < numLevels; ++i)
        m_allocator->poolCreate(1, i, sizeof(NodeId) * m_levelInfos[i].numVoxel, numReservedNodes[i]);

    m_levelColors.resize(MAXLEV);
    m_levelColors[0] = dm::float3(0, 0, 1);           // blue
    m_levelColors[1] = dm::float3(0, 1, 0);           // green
    m_levelColors[2] = dm::float3(1, 0, 0);           // red
    m_levelColors[3] = dm::float3(1, 1, 0);           // yellow
    m_levelColors[4] = dm::float3(1, 0, 1);           // purple
    m_levelColors[5] = dm::float3(0, 1, 1);           // aqua
    m_levelColors[6] = dm::float3(1.f, 0.5f, 0.f);         // orange
    m_levelColors[7] = dm::float3(0.f, 0.5f, 1.f);         // green-blue
    m_levelColors[8] = dm::float3(0.7f, 0.7f, 0.7f);  // grey

    // atlas config.
    m_atlasBrickDim = getResDim(0);
    m_atlasBrickDimWithApron = m_atlasBrickDim + 2u * m_atlasMapInfo.apron;
}

uint32_t GVDB::getRootRange() { return m_levelInfos.back().range; }

uint8_t GVDB::getNumLevels() { return static_cast<uint8_t>(m_levelInfos.size()); }

uint64_t GVDB::getNumUsedNodes(uint8_t level) {
    return m_allocator->poolGetNumUsed(0, level);
}

uint32_t GVDB::getResDim(uint8_t lev) { return m_levelInfos[lev].voxDim; }

uint32_t GVDB::getRange(int8_t lev) {
    if(lev == -1)
        return 1;
    return m_levelInfos[lev].range;
}

dm::float3 GVDB::getCover(uint8_t lev) { return { (float)getRange(lev) }; }

dm::float3 GVDB::getColorDim(uint8_t lev) { return m_levelColors[lev]; }

uint64_t GVDB::getVoxelCnt(uint8_t lev) { return m_levelInfos[lev].numVoxel; }

Extents GVDB::computeExtents(Node *node) {
    Extents e;
    e.lev = node->level;
    int32_t cover = getRange(e.lev - 1);
    e.cover = getCover(e.lev - 1);
    e.imin = node->pos / cover;
    e.imax = e.imin + ((int)getResDim(e.lev) - 1);
    e.vmin = dm::float3{e.imin * cover};
    e.vmax = dm::float3{(e.imax + 1) * cover};
    e.ires = e.imax - e.imin + 1;
    e.icnt = e.ires.x * e.ires.y * e.ires.z;
    return e;
}

Node *GVDB::getNode(uint8_t lev, int idx) {
    return (Node *)m_allocator->poolData(NodeId{0, lev, uint64_t(idx)});
}

dm::box3 GVDB::getNodeWorldBounds(Node *node) {
    return dm::box3{
        dm::float3{node->pos},
        dm::float3{node->pos} + getCover(node->level)
    };
}

int GVDB::activateRegion(const Extents &e) {

    int lev = e.lev - 1;

    dm::int3 posLB;
    int cnt = 0;
    NodeId leaf;

    for(int z = e.imin.z; z <= e.imax.z; ++z)
        for(int y = e.imin.y; y <= e.imax.y; ++y)
            for(int x = e.imin.x; x <= e.imax.x; ++x) {
                posLB = dm::int3{x, y, z} * dm::int3{e.cover};
                leaf = activateSpaceAtLevel(lev, posLB);
                ++cnt;
            }

    return cnt;
}

int GVDB::activateRegionFromMasks(const Extents &e, const uint8_t *masks) {
    int lev = e.lev - 1;

    dm::int3 voxIdx;
    dm::int3 posLB;
    int cnt = 0;
    NodeId leaf;

    for (int z = e.imin.z; z <= e.imax.z; ++z)
        for (int y = e.imin.y; y <= e.imax.y; ++y)
            for (int x = e.imin.x; x <= e.imax.x; ++x) {
                voxIdx = dm::int3{x, y, z} - e.imin;
                if (masks[(voxIdx.z * e.ires.y + voxIdx.y) * e.ires.x + voxIdx.x]) {
                    posLB = dm::int3{x, y, z} * dm::int3{e.cover};
                    leaf = activateSpaceAtLevel(lev, posLB);
                    ++cnt;
                }
            }
    return cnt;
}

void GVDB::copyAtlasBlock(int channel, dm::int3 dstOffset, gp::IBuffer *srcBuffer, dm::uint3 srcBlockDim) {
    auto dstTexture = m_atlasResourceChannelsGPU[channel].Get();

    gp::TextureCopyLocation srcLoc = {}, dstLoc = {};
    srcLoc.type = gp::TextureCopyType::PlacedFootprint;
    srcLoc.resource = srcBuffer;
    uint32_t bytesPerBlock = 0;

    switch (m_atlasResourceChannels[channel].format) {
        case ATLAS_FORMAT_R8_UINT:
            srcLoc.placeFootprint.format = gp::Format::R8_UINT;
            bytesPerBlock = 1;
            break;
        case ATLAS_FORMAT_RGBA8_UINT:
            srcLoc.placeFootprint.format = gp::Format::RGBA8_UINT;
            bytesPerBlock = 4;
            break;
        case ATLAS_FORMAT_R32_FLOAT:
            srcLoc.placeFootprint.format = gp::Format::R32_FLOAT;
            bytesPerBlock = 4;
            break;
        case ATLAS_FORMAT_RGB32_FLOAT:
            srcLoc.placeFootprint.format = gp::Format::RGB32_FLOAT;
            bytesPerBlock = 12;
            break;
        case ATLAS_FORMAT_RGBA32_FLOAT:
            srcLoc.placeFootprint.format = gp::Format::RGBA32_FLOAT;
            bytesPerBlock = 16;
            break;
    }

    srcLoc.placeFootprint.width = srcBlockDim.x;
    srcLoc.placeFootprint.height = srcBlockDim.y;
    srcLoc.placeFootprint.depth = srcBlockDim.z;
    srcLoc.placeFootprint.rowPitch = srcBlockDim.x * bytesPerBlock;

    dstLoc.type = gp::TextureCopyType::SubresourceIndex;
    dstLoc.resource = dstTexture;

    GVDB_V_GP(m_gpQueue->copyTextureRegion(dstLoc, dstOffset.x, dstOffset.y, dstOffset.z,
                                           srcLoc, nullptr));
}

void GVDB::finishTopology() { computeVolumeBounds(0); }

void GVDB::updateAtlas() {
    updateAtlasMap();
    resizeAtlasResources();
}

void GVDB::updateApron() {
    prepareVDB();

    // Dimensions
    int bricks = static_cast<int>(getNumUsedNodes(0));
    int brickres = getAtlasBlockDimWithoutApron().x;
    int brickwid = getAtlasBlockDim().x;

    if (bricks == 0) return;

    gp::dim3 threadDim{bricks, brickres, brickres};
    gp::dim3 blockDim{6 * int(getApron()), 8, 8};
    gp::dim3 gridDim{threadDim.x, dm::div_ceil(threadDim.y, blockDim.y),
                     dm::div_ceil(threadDim.z, blockDim.z)};

    for (uint32_t i = 0; i < (uint32_t)m_atlasResourceChannelsGPU.size(); ++i) {
        gp::IKernel *pKernel;
        switch (m_atlasResourceChannels[i].format) {
            case ATLAS_FORMAT_R8_UINT:
                pKernel = m_updateApronKernels[0];
                break;
            case ATLAS_FORMAT_RGBA8_UINT:
                pKernel = m_updateApronKernels[1];
                break;
            case ATLAS_FORMAT_R32_FLOAT:
                pKernel = m_updateApronKernels[2];
                break;
            case ATLAS_FORMAT_RGB32_FLOAT:
                pKernel = m_updateApronKernels[3];
                break;
            case ATLAS_FORMAT_RGBA32_FLOAT:
                pKernel = m_updateApronKernels[3];
                break;
        }

        gp::KernelArg args[] = {
            gp::KernelArg::Buffer(m_VDBInfoGPU),
            gp::KernelArg::Scalar(i),
            gp::KernelArg::Scalar(bricks),
            gp::KernelArg::Scalar(brickres),
            gp::KernelArg::Scalar(brickwid),
            gp::KernelArg::Scalar(m_atlasResourceChannels[i].voidValue)};
        GVDB_V_GP(m_gpQueue->launch(pKernel, gridDim,
                                    blockDim, args, std::size(args)));
    }
}

gp::IBuffer *GVDB::getVBDInfoGPU() const {
    DONUT_ASSERT(m_VDBInfoGPU);
    return m_VDBInfoGPU.Get();
}

void GVDB::convertBitmaskToNonBitmask() {
    Node* node;
    NodeId* clist;
    NodeId childId;
    uint64_t ccnt, cmax, bitchk;
    uint32_t ndx;
    uint32_t b;

    // Activate all bricks (assuming earlier file did not use flags)
    for (int n = 0; n < getNumUsedNodes(0); ++n) {
        node = getNode(0, n);
        node->flags = 1;
    }

    // Convert upper nodes to non-bitmask
    for (int lv = 1; lv < getNumLevels(); ++lv) {
        b = getLevelMaskBits(lv);
        for (int n = 0; n < getNumUsedNodes(lv); ++n) {
            node = getNode(lv, n);
            if(node->childList == NodeId::null()) {
                donut::log::error("convertBitmaskToNonBitmask, child list is null.");
                continue;
            }

            ccnt = bitop::countOn(node, b);
            cmax = getVoxelCnt(lv);
            clist = (NodeId*)m_allocator->poolData(node->childList);

            // pad remainder of child list with null
            memset(clist + ccnt, 0xFF, sizeof(NodeId) * (cmax - ccnt));

            // move children into index locations
            for (int64_t j = static_cast<int64_t>(ccnt - 1); j >= 0; --j) {
                ndx = bitop::countToIndex(node, b, j);
                bitchk = bitop::countOn(node, ndx);
                if(bitchk != j) {
                    donut::log::error("countToIndex error.");
                    continue;
                }
                childId = clist[j];
                clist[ndx] = childId;
                clist[j] = NodeId::null();
            }
        }
    }
}

donut::FRESULT GVDB::loadVBXC(donut::IDataBlob* pVBXC) {

    clear();

    auto dp = (const uint8_t *)pVBXC->GetDataPtr();

    // ---- grid header
    char grid_name[256];      // grid name
    uint8_t grid_dtype;       // grid data type
    uint8_t read_masks;  // grid components
    uint8_t grid_compress;    // grid compression (0=none, 1=blosc, 2=..)
    dm::float3 voxel_size;    // voxel size
    int32_t leafcnt;          // total brick count
    dm::int3 leaf_dim;        // brick dimensions
    int32_t apron;            // brick apron
    int32_t num_channel;      // number of channels
    uint64_t atlas_size;      // total atlas size (all channels)
    uint8_t grid_topotype;    // topology type? (0=none, 1=reuse, 2=gvdb, 3=..)
    int32_t grid_reuse;       // topology reuse
    uint8_t grid_layout;      // brick layout? (0=atlas, 1=brick)
    dm::int3 axiscnt;         // atlas brick count
    dm::int3 axisres;         // atlas res

    memcpy(grid_name, dp, 256);
    dp += 256;
    memcpy(&grid_dtype, dp, sizeof(uint8_t));
    dp += sizeof(uint8_t);
    memcpy(&read_masks, dp, sizeof(uint8_t));
    dp += sizeof(uint8_t);
    memcpy(&grid_compress, dp, sizeof(uint8_t));
    dp += sizeof(uint8_t);
    memcpy(&voxel_size, dp, sizeof(dm::float3));
    dp += sizeof(dm::float3);
    memcpy(&leafcnt, dp, sizeof(int32_t));
    dp += sizeof(int32_t);
    memcpy(&leaf_dim, dp, sizeof(dm::int3));
    dp += sizeof(dm::int3);
    memcpy(&apron, dp, sizeof(int32_t));
    dp += sizeof(int32_t);
    memcpy(&num_channel, dp, sizeof(int32_t));
    dp += sizeof(int32_t);
    memcpy(&atlas_size, dp, sizeof(uint64_t));
    dp += sizeof(int64_t);
    memcpy(&grid_topotype, dp, sizeof(uint8_t));
    dp += sizeof(uint8_t);
    memcpy(&grid_reuse, dp, sizeof(int32_t));
    dp += sizeof(int32_t);
    memcpy(&grid_layout, dp, sizeof(uint8_t));
    dp += sizeof(uint8_t);
    memcpy(&axiscnt, dp, sizeof(dm::int3));
    dp += sizeof(dm::int3);
    memcpy(&axisres, dp, sizeof(dm::int3));
    dp += sizeof(dm::int3);

    // ---- topology section

    int32_t num_levels;  // num levels
    uint64_t root_id;    // root id
    int32_t log_dim[MAXLEV];
    int32_t voxel_res[MAXLEV];
    dm::int3 range[MAXLEV];
    int32_t p1_cnt[MAXLEV];
    int32_t p1_width[MAXLEV];
    int32_t p2_cnt[MAXLEV];
    int32_t p2_width[MAXLEV];

    memcpy(&num_levels, dp, sizeof(int32_t));
    dp += sizeof(int32_t);
    memcpy(&root_id, dp, sizeof(uint64_t));
    dp += sizeof(uint64_t);

    for (int n = 0; n < num_levels; ++n) {
        memcpy(&log_dim[n], dp, sizeof(int32_t));
        dp += sizeof(int32_t);
        memcpy(&voxel_res[n], dp, sizeof(int32_t));
        dp += sizeof(int32_t);
        memcpy(&range[n], dp, sizeof(dm::int3));
        dp += sizeof(dm::int3);
        memcpy(&p1_cnt[n], dp, sizeof(int32_t));
        dp += sizeof(int32_t);
        memcpy(&p1_width[n], dp, sizeof(int32_t));
        dp += sizeof(int32_t);
        memcpy(&p2_cnt[n], dp, sizeof(int32_t));
        dp += sizeof(int32_t);
        memcpy(&p2_width[n], dp, sizeof(int32_t));
        dp += sizeof(int32_t);
    }
    if (p1_width[0] != sizeof(Node)) {
        donut::log::error(
            "VBX file VBX file contains nodes incompatible with current gvdb "
            "library.");
        return donut::FE_GENERIC_ERROR;
    }

    configLevels(num_levels, (const uint32_t*)log_dim, (const uint32_t*)p1_cnt,
                      (read_masks == 1));
    configAtlas(dm::uint3{axiscnt}, apron);

    m_rootNodeId = NodeId{root_id};

    uint64_t pool_mem_size;
    for (int n = 0; n < num_levels; ++n) {
        pool_mem_size = p1_cnt[n] * p1_width[n];
        memcpy(m_allocator->poolData(0, n), dp, pool_mem_size);
        m_allocator->poolSetSize(0, n, p1_cnt[n]);
        dp += pool_mem_size;
    }
    for (int n = 0; n < num_levels; ++n) {
        pool_mem_size = p2_cnt[n] * p2_width[n];
        memcpy(m_allocator->poolData(1, n), dp, pool_mem_size);
        m_allocator->poolSetSize(1, n, p2_cnt[n]);
        dp += pool_mem_size;
    }

    // Convert bitmasks to non-bitmasks
    convertBitmaskToNonBitmask();

    finishTopology();

    // Atlas section
    destroyChannels();

    updateAtlasMap();

    if (num_channel) {
        if (dm::any(getAtlasGridDim() != dm::uint3(axiscnt))) {
            donut::log::error("Atlas dimension does not coincident.");
            return donut::FE_GENERIC_ERROR;
        }
    }

    // Read atlas into GPU slice-by-slice to conserve CPU and GPU mem
    for (int chan = 0; chan < num_channel; ++chan) {
        int32_t chan_type, chan_stride;
        memcpy(&chan_type, dp, sizeof(int32_t));
        dp += sizeof(int32_t);
        memcpy(&chan_stride, dp, sizeof(int32_t));

        GVDBAtlasResourceDesc resDesc = {};
        donut::gp::SubresourceFootprint footprint = {};
        switch (chan_type) {
            case 0:
                resDesc.format = AtlasFormat::ATLAS_FORMAT_R8_UINT;
                break;
            case 2:
                resDesc.format = AtlasFormat::ATLAS_FORMAT_RGBA8_UINT;
                break;
            case 3:
                resDesc.format = AtlasFormat::ATLAS_FORMAT_R32_FLOAT;
                break;
            case 4:
                resDesc.format = AtlasFormat::ATLAS_FORMAT_RGB32_FLOAT;
            case 5:
                resDesc.format = AtlasFormat::ATLAS_FORMAT_RGBA32_FLOAT;
            default:
                donut::log::error("Unknown atlas resource format: %d", chan_type);
                return donut::FE_GENERIC_ERROR;
        }
        resDesc.samplerDesc.addressU = donut::gp::SamplerAddressMode::Border;
        resDesc.samplerDesc.addressV = donut::gp::SamplerAddressMode::Border;
        resDesc.samplerDesc.addressW = donut::gp::SamplerAddressMode::Border;

        addChannel(resDesc);
        resizeAtlasResource(chan);
        if (!writeAtlasResource(chan, dp, axisres.y, axisres.x * chan_stride))
            return donut::FE_GENERIC_ERROR;

        dp += uint64_t(axisres.x) * axisres.y * axisres.z * chan_stride;
    }

    m_gpQueue->getDevice()->commitQueue(m_gpQueue);
    return donut::FS_OK;
}

dm::box<int, 3> GVDB::computeVolumeBounds(int lev) {
    dm::box<int, 3> bounds = dm::box<int, 3>::empty();

    dm::int3 cover = (int)getRange(lev);

    Node *curr;
    dm::int3 posLB, posRT;
    for(int n = 0; n < m_allocator->poolGetNumUsed(0, lev); ++n) {
        curr = getNode({0, (uint8_t)lev, (uint64_t)n});
        if(!curr->flags) continue; // inactivated, skip

        posLB = curr->pos;
        posRT = posLB + cover;

        bounds |= posLB;
        bounds |= posRT;
    }

    m_voxLeafBounds = bounds;

    return bounds;
}

void GVDB::resizeAtlasResource(int c) {
    // Update atlas resource
    auto device = m_gpQueue->getDevice();

    if(c >= m_atlasResourceChannelsGPU.size())
        m_atlasResourceChannelsGPU.resize(c + 1);

    auto resDim = getAtlasResDim();
    gp::TextureDesc texDesc;
    texDesc.dimension = gp::TextureDimension::Texture3D;
    texDesc.width = resDim.x;
    texDesc.height = resDim.y;
    texDesc.depthOrArraySize = resDim.z;

    auto atlasFormat = m_atlasResourceChannels[c].format;
    switch (atlasFormat) {
        case ATLAS_FORMAT_R8_UINT:
            texDesc.format = gp::Format::R8_UINT;
            break;
        case ATLAS_FORMAT_RGBA8_UINT:
            texDesc.format = gp::Format::RGBA8_UINT;
            break;
        case ATLAS_FORMAT_R32_FLOAT:
            texDesc.format = gp::Format::R32_FLOAT;
            break;
        case ATLAS_FORMAT_RGB32_FLOAT:
            texDesc.format = gp::Format::RGB32_FLOAT;
            break;
        case ATLAS_FORMAT_RGBA32_FLOAT:
            texDesc.format = gp::Format::RGBA32_FLOAT;
            break;
    }
    texDesc.samplerDesc = m_atlasResourceChannels[c].samplerDesc;

    if (m_atlasResourceChannelsGPU[c]) {
        auto texDesc0 = m_atlasResourceChannelsGPU[c]->getDesc();
        if (texDesc0->format != texDesc.format || texDesc0->width < texDesc.width ||
            texDesc0->height < texDesc.height ||
            texDesc0->depthOrArraySize < texDesc.depthOrArraySize)
            m_atlasResourceChannelsGPU[c] = nullptr;  // request recreate atlas.
    }

    if (!m_atlasResourceChannelsGPU[c])
        GVDB_V_GP(device->createTexture(texDesc, &m_atlasResourceChannelsGPU[c]));
}

bool GVDB::writeAtlasResource(int c, const void* data, uint32_t height,
                                    uint32_t rowPitch) {
    auto atlasTexture = m_atlasResourceChannelsGPU[c].Get();
    auto texDesc = atlasTexture->getDesc();

    donut::gp::SubresourceFootprint footprint = {};
    footprint.format = texDesc->format;
    footprint.width = texDesc->width;
    footprint.height = height;
    footprint.depth = texDesc->depthOrArraySize;
    footprint.rowPitch = rowPitch;

    FRESULT fr;
    if(FFAILED(fr = m_gpQueue->writeTextureRegion(atlasTexture, 0, data, footprint, 0, 0, 0, nullptr))) {
        GVDB_V_GP(fr);
        return false;
    }

    return true;
}

void GVDB::resizeAtlasResources() {
    m_atlasResourceChannelsGPU.resize(m_atlasResourceChannels.size());
    for (int c = 0; c < (int)m_atlasResourceChannels.size(); ++c)
        resizeAtlasResource(c);
}

NodeId GVDB::activateSpaceAtLevel(uint8_t level, dm::int3 pos) {
    bool bn = false;
    NodeId nodeId = activateSpace(m_rootNodeId, pos, bn, NodeId::null(), level);
    if(nodeId == NodeId::null()) return NodeId::null();
    if(!bn) return nodeId;
    return NodeId::null();
}

NodeId GVDB::activateSpace(NodeId currId, dm::int3 pos, bool& newNode,
                                 NodeId stopId, uint8_t stopLevel) {
    if(m_rootNodeId == NodeId::null() && m_rootNodeId == currId) {
        // Create new root
        auto p = getCoveringNodePos(stopLevel, pos);
        m_rootNodeId = addNode(stopLevel, p);
        currId = m_rootNodeId;
    }

    // Activate recursively to leaf
    auto curr = getNode(currId);
    uint32_t b;

    if(getPosInNode(currId, pos, &b)) {
        // check stop node
        if(stopId != NodeId::null()) {
            Node *stopNode = getNode(stopId);
            if(dm::all(pos == stopNode->pos) && !isOn(currId, b) && stopNode->level + 1 == curr->level) {
                // if same position as stopnode, and bit is not on.
                return insertChildNode(currId, stopId, b);
            }
        }

        // check stop level
        if(curr->level == stopLevel) return currId;

        NodeId childId;
        // point is inside this node, add children
        if(!isOn(currId, b)) {
            childId = insertChildNode(currId, b);
            if(curr->level == 1) newNode = true;
        } else
            childId = getChildNode(currId, b);

        if(isLeaf(childId)) return childId;

        return activateSpace(childId, pos, newNode, stopId, stopLevel);
    } else {
        // point is outside this node
        auto parent = curr->parent;
        if(parent == NodeId::null()) {
            parent = reparent(curr->level, currId, pos, newNode);
            if(parent == NodeId::null()) return NodeId::null();
        }

        // active point inside the (possibly new) parent
        return activateSpace(parent, pos, newNode, stopId, stopLevel);
    }
}

NodeId GVDB::reparent(uint8_t level, NodeId prevrootId, dm::int3 pos, bool& newNode) {
    Node *prevroot = getNode(prevrootId);
    dm::int3 prevrootPos = prevroot->pos;
    dm::int3 p, pos1, pos2;
    bool cover = false;
    
    // find a level node which covers both the child (former root)
    // and the new position
    while (!cover && level < m_levelInfos.size()) {
        level++;
        pos1 = getCoveringNodePos(level, pos);
        pos2 = getCoveringNodePos(level, prevrootPos);
        cover = dm::all(pos1 == pos2);
    }
    if(level >= m_levelInfos.size())
        return NodeId::null();

    // create new covering root
    auto newRootId = addNode(level, pos1);

    // insert prevroot into new root
    bool bn = false; // prev path does not create new leaf, so ignore newNode
    activateSpace(newRootId, prevrootPos, bn, prevrootId); // use stopnode to connect paths

    // insert new pos into root
    NodeId leafId = activateSpace(newRootId, pos, bn);
    
    // update root
    m_rootNodeId = newRootId;

    return getNode(leafId)->parent; // return parent of the new pos
}

Extents GVDB::computeExtents(uint8_t lev, const dm::box3 &bounds) {
    Extents e;
    e.lev = lev;
    e.vmin = bounds.lower();
    e.vmax = bounds.upper();
    e.cover = getCover(lev - 1).x;
    e.imin = dm::int3(e.vmin / e.cover);
    e.imax = dm::int3(e.vmax / e.cover - dm::float3{1.f});
    e.ires = e.imax - e.imin + 1;
    e.icnt = e.ires.x * e.ires.y * e.ires.z;
    return e;
}

dm::int3 GVDB::getCoveringNodePos(uint8_t level, dm::int3 pos) {
    int range = m_levelInfos[level].range;
    dm::int3 nodepos = pos;
    nodepos /= int(range);
    nodepos = nodepos * range;
    if(pos.x < nodepos.x) nodepos.x -= range;
    if(pos.y < nodepos.y) nodepos.y -= range;
    if(pos.z < nodepos.z) nodepos.z -= range;
    return nodepos;
}

NodeId GVDB::addNode(uint8_t level, dm::int3 pos) {
    auto nodeId = m_allocator->poolAlloc(0, level);
    auto node = getNode(nodeId);
    node->level = level;
    node->pos = pos;
    node->childList = NodeId::null();
    node->parent = NodeId::null();
    node->value = dm::int3{-1};
    node->flags = 1;
    return nodeId;
}

NodeId GVDB::insertChildNode(NodeId currId, NodeId childId, uint32_t i) {
    Node *curr = getNode(currId);
    Node *child = getNode(childId);
    child->parent = currId;

    if (curr->childList == NodeId::null()) {
        // Allocate a new block
        curr->childList = m_allocator->poolAlloc(1, curr->level);
        uint64_t* clist = (uint64_t*)m_allocator->poolData(curr->childList);
        memset(clist, 0xFF, m_allocator->poolGetElementStride(1, curr->level));
    }

    // Insert into child list
    NodeId* clist = (NodeId*)m_allocator->poolData(curr->childList);
    clist[i] = childId;

    return childId;
}

NodeId GVDB::insertChildNode(NodeId currId, uint32_t i) {
    auto curr = getNode(currId);
    auto p = getPosFromBit(curr->level, i);
    p = p * int(m_levelInfos[curr->level - 1].range) + curr->pos;

    auto childId = addNode(curr->level - 1, p);
    return insertChildNode(currId, childId, i);
}

Node* GVDB::getNode(NodeId id) {
    DONUT_ASSERT(id.group() == 0);
    return (Node *)m_allocator->poolData(id);
}

bool GVDB::getPosInNode(NodeId currId, dm::int3 pos, uint32_t* pBits) {
    auto curr = getNode(currId);
    int res = m_levelInfos[curr->level].voxDim;
    int range = m_levelInfos[curr->level].range;

    dm::int3 p = pos - curr->pos;

    if(dm::all(p >= dm::int3::zero() & p < dm::int3(range))) {
        // point is inside this node
        p = (p * res) / range;
        if(pBits)
            *pBits = getBitPos(curr->level, p);
        return true;
    } else {
        if(pBits) *pBits = 0;
        return false;
    }
}

uint32_t GVDB::getBitPos(uint8_t level, dm::int3 pos) {
    auto dim = m_levelInfos[level].voxDim;
    return (pos.z * dim + pos.y) * dim + pos.x;
}

dm::int3 GVDB::getPosFromBit(uint8_t level, uint32_t bits) {
    uint32_t logDim = m_levelInfos[level].logDim;
    uint32_t mask = (1 << logDim) - 1;
    int z = (bits >> (logDim << 1)) & mask;
    int y = (bits >> logDim) & mask;
    int x = bits & mask;
    return dm::int3{x, y, z};
}

NodeId GVDB::getChildNode(NodeId currId, uint32_t b) {
    auto curr = getNode(currId);
    if(curr->childList == NodeId::null()) return NodeId::null();

    NodeId *clist = (NodeId *)m_allocator->poolData(curr->childList);
    return clist[b];
}

uint32_t GVDB::getLevelMaskBits(uint8_t lev) { return m_levelInfos[lev].bitmasksBits; }

bool GVDB::isOn(NodeId currId, uint32_t b) {
    return getChildNode(currId, b) != NodeId::null();
}

bool GVDB::isLeaf(NodeId currId) { return currId.level() == 0; }

dm::int3 GVDB::getAtlasBlockPos(uint64_t nodeIdx) {
    dm::int3 atlasPos;
    uint64_t a1 = m_atlasGridDim.x;
    uint64_t a2 = m_atlasGridDim.x * m_atlasGridDim.y;
    atlasPos.z = int(nodeIdx / a2);
    nodeIdx -= uint64_t(atlasPos.z) * a2;
    atlasPos.y = int(nodeIdx / a1);
    nodeIdx -= uint64_t(atlasPos.y) * a1;
    atlasPos.x = uint32_t(nodeIdx);

    return atlasPos;
}

dm::int3 GVDB::getAtlasPos(uint64_t nodeIdx) {
    auto blockIdx = getAtlasBlockPos(nodeIdx);
    // return blockIdx * dm::int3(m_blockDimWithApron) + dm::int3(m_apron);
    return blockIdx * dm::int3(m_atlasBrickDimWithApron) +
           dm::int3{m_atlasMapInfo.apron};
}

void GVDB::updateAtlasMap() {
    // Resize atlas map
    uint64_t leafCnt = getNumUsedNodes(0);
    m_atlasGridDim.z = (uint32_t)std::ceil(leafCnt / float(m_atlasGridDim.x * m_atlasGridDim.y));
    uint64_t cnt = m_atlasGridDim.x * m_atlasGridDim.y * m_atlasGridDim.z;

    // Assign new nodes to atlas map
    // map node index to atlas texel position
    uint64_t nextBrickIdx = 0;
    for (uint64_t i = 0; i < leafCnt; ++i) {
        auto node = getNode(0, i);
        if (!node->flags) continue;
        dm::int3 brickPos = getAtlasPos(nextBrickIdx++);
        node->value = brickPos;
    }

    m_atlasMap.resize(nextBrickIdx);
    // Initialize atlas map
    for (auto &atlasNode : m_atlasMap) {
        atlasNode.pos = dm::int3(-1u);
        atlasNode.leafNode = -1u;
    }

    // map atlas block index to node index
    for (uint64_t i = 0; i < leafCnt; ++i) {
        auto node = getNode(0, i);
        if (!node->flags) continue;

        dm::int3 blockIdx = node->value / dm::int3{m_atlasBrickDimWithApron};
        uint64_t linearIdx =
            (blockIdx.z * m_atlasGridDim.y + blockIdx.y) * m_atlasGridDim.x + blockIdx.x;
        m_atlasMap[linearIdx].pos = node->pos;
        m_atlasMap[linearIdx].leafNode = static_cast<int>(i);
    }

    auto device = m_gpQueue->getDevice();

    // commit Pool I
    {
        m_nodePoolsGPU.resize(m_allocator->poolGetLevelCount(0));
        gp::BufferDesc bufDesc;
        for (size_t i = 0; i < m_nodePoolsGPU.size(); ++i) {
            bufDesc.byteSize = m_allocator->poolGetMemoryUsedWidth(0, i);
            device->createBuffer(bufDesc, &m_nodePoolsGPU[i]);
            m_gpQueue->writeBuffer(m_nodePoolsGPU[i], m_allocator->poolData(0, i),
                                   bufDesc.byteSize, 0);
        }
    }

    // commit Pool II
    {
        m_childIdPoolsGPU.resize(m_allocator->poolGetLevelCount(1));
        gp::BufferDesc bufDesc;
        for (size_t i = 0; i < m_childIdPoolsGPU.size(); ++i) {
            bufDesc.byteSize = m_allocator->poolGetMemoryUsedWidth(1, i);
            device->createBuffer(bufDesc, &m_childIdPoolsGPU[i]);
            m_gpQueue->writeBuffer(m_childIdPoolsGPU[i], m_allocator->poolData(1, i),
                                   bufDesc.byteSize, 0);
        }
    }

    // commit atlas map
    {
        gp::BufferDesc buffDesc;
        buffDesc.byteSize = sizeof(AtlasNode) * m_atlasMap.size();
        device->createBuffer(buffDesc, &m_atlasMapGPU);
        m_gpQueue->writeBuffer(m_atlasMapGPU, m_atlasMap.data(), buffDesc.byteSize, 0);
    }
}

void GVDB::prepareVDB() {
    VDBInfo vdbInfo;
    // Fill in VBD info
    {
        int tlev = 1;
        int levs = m_allocator->poolGetLevelCount(0);
        for (int n = levs - 1; n >= 0; --n) {
            vdbInfo.dim[n] = m_levelInfos[n].logDim;
            vdbInfo.res[n] = getResDim(n);
            vdbInfo.vdel[n] = getCover(n - 1);
            vdbInfo.noderange[n] = getRange(n);
            vdbInfo.nodecnt[n] = static_cast<int>(m_allocator->poolGetNumUsed(0, n));
            vdbInfo.nodewid[n] = m_allocator->poolGetElementStride(0, n);
            vdbInfo.childwid[n] = m_allocator->poolGetElementStride(1, n);
            vdbInfo.nodelist[n] = m_nodePoolsGPU[n]->getNativeHandle();
            vdbInfo.childlist[n] = m_childIdPoolsGPU[n]->getNativeHandle();
            if (vdbInfo.nodecnt[n] == 1) tlev = n;  // get top level for rendering
        }
        vdbInfo.atlas_map = m_atlasMapGPU->getNativeHandle();
        vdbInfo.atlas_apron = m_atlasMapInfo.apron.x;
        vdbInfo.atlas_cnt = dm::int3{getAtlasGridDim()};
        vdbInfo.atlas_res = dm::int3{getAtlasResDim()};
        vdbInfo.brick_res = getAtlasBlockDimWithoutApron().x;
        int blkres = getAtlasBlockDimWithoutApron().x;
        vdbInfo.top_lev = tlev;
        vdbInfo.bmin = dm::float3{m_voxLeafBounds.lower()};
        vdbInfo.bmax = dm::float3{m_voxLeafBounds.upper()};

        for (uint32_t i = 0; i < (uint32_t)m_atlasResourceChannelsGPU.size(); ++i) {
            vdbInfo.volIn[i] = m_atlasResourceChannelsGPU[i]->getNativeHandle();
            vdbInfo.volOut[i] = m_atlasResourceChannelsGPU[i]->getUnorderedAccessHandle(0);
        }
    }

    if(!m_VDBInfoGPU) {
        gp::BufferDesc bufDesc;
        bufDesc.byteSize = sizeof(vdbInfo);
        m_gpQueue->getDevice()->createBuffer(bufDesc, &m_VDBInfoGPU);
    }

    m_gpQueue->writeBuffer(m_VDBInfoGPU, &vdbInfo, sizeof(vdbInfo), 0);
}

}  // namespace gvdb

