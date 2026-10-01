#ifndef SRC_GVDBVOLUME_H
#define SRC_GVDBVOLUME_H
#include <gvdb/GPDevice.h>
#include <donut/core/object/Foundation.h>
#include <donut/core/object/AutoPtr.h>
#include <donut/core/math/math.h>
#include <donut/core/log.h>
#include <vector>

namespace donut {
struct IDataBlob;
}
namespace donut::vfs {
class IFileSystem;
}

namespace gvdb {

#pragma region "GVDBTypes"

#ifdef _WIN32
#define GVDB_ALIGN(x) __declspec(align(x))
#else
#define GVDB_ALIGN(x) __attribute__((aligned(x)))
#endif

#define GVDB_USE_BITMASKS 0

#define GVDB_SCAN_BLOCKSIZE 512

enum { MAXLEV = 10 };

enum { MAX_CHANNEL = 32 };

using uchar = unsigned char;

enum AtlasFormat {
    ATLAS_FORMAT_R8_UINT,
    ATLAS_FORMAT_RGBA8_UINT,
    ATLAS_FORMAT_R32_FLOAT,
    ATLAS_FORMAT_RGB32_FLOAT,
    ATLAS_FORMAT_RGBA32_FLOAT,
};

#pragma endregion "GVDBTypes"

#pragma region "GVDB"

struct NodeId {
    NodeId(uint8_t grp, uint8_t lev, uint64_t ndx)
        : grp{grp}, lev{lev}, idx{ndx} {}

    constexpr NodeId(uint64_t data = ~0ull): data{data} {}

    static constexpr NodeId null();

    uint8_t group() const { return grp; }

    uint8_t level() const { return lev; }

    uint64_t index() const { return idx; }

    bool operator==(const NodeId &rhs) const { return data == rhs.data; }

    bool operator!=(const NodeId &rhs) const { return data != rhs.data; }

 private:
    union {
        struct {
            uint64_t grp : 8;
            uint64_t lev : 8;
            uint64_t idx : 48;
        };
        uint64_t data;
    };
};
static_assert(sizeof(NodeId) == sizeof(uint64_t), "NodeId must be 8 bytes long");

struct GVDB_ALIGN(16) Node {
    //                                                   Size(byte): Range:
    uint8_t level;          // Tree level                1           0~255
    uint8_t flags;          // Flags                     1           true - used, false - discard
    uint8_t priority;       // Priority                  1
    uint8_t pad;            //                           1
    dm::int3 pos;           // Pos in Index space        12
    dm::int3 value;         // offset in atlas           12
    dm::float3 range;       // Value min, max average    12
    NodeId parent;          // Parent ID pool reference  8
    NodeId childList;       // Child list index          8
    uint64_t mask;          // Start of BITMASK          8
                            // HEADER TOTAL              64
};

struct GVDB_ALIGN(16) Extents {
    int lev;
    dm::float3 vmin, vmax; // L0 index-space bounds of children
    dm::int3 imin, imax, ires; //absolute index-space extents of children
    dm::float3 cover; // child width
    int icnt;
};

struct AtlasNode {
    dm::int3 pos; // Leaf node position in GVDB-tree level 0S
    int leafNode; // Leaf node index in GVDB-tree level 0
};

// Contains information for a GVDB volume.
// Levels vary from 0 (bricks) to top_lev (root node).
struct GVDB_ALIGN(16) VDBInfo {
    int				dim[MAXLEV]; // Log base 2 of lateral resolution of each node per level
    int				res[MAXLEV]; // Lateral resolution of each node per level
    dm::float3		vdel[MAXLEV]; // How many voxels on a side a child of each level covers
    dm::int3		noderange[MAXLEV]; // How many voxels on a side a node of each level covers
    int				nodecnt[MAXLEV]; // Total number of allocated nodes per level
    int				nodewid[MAXLEV]; // Size of a node at each level in bytes
    int				childwid[MAXLEV]; // Size of the child list per node at each level in bytes
    donut::gp::NativeHandle		nodelist[MAXLEV]; // GPU pointer to each level's pool group 0 (nodes)
    donut::gp::NativeHandle		childlist[MAXLEV]; // GPU pointer to each level's pool group 1 (child lists)
    donut::gp::NativeHandle		atlas_map; // GPU pointer to the atlas map (which maps from atlas to world space)
    dm::int3		atlas_cnt; // Number of bricks on each axis of the atlas
    dm::int3		atlas_res; // Total resolution in voxels of the atlas
    int				atlas_apron; // Apron size
    int				brick_res; // Resolution of a single brick
    int				top_lev; // Top level (i.e. tree spans from voxels to level 0 to level top_lev)
    dm::float3		bmin; // Inclusive minimum of axis-aligned bounding box in voxels
    dm::float3		bmax; // Inclusive maximum of axis-aligned bounding box in voxels
    donut::gp::NativeHandle	volIn[MAX_CHANNEL]; // Texture reference (read plus interpolation) to atlas per channel
    donut::gp::NativeHandle	volOut[MAX_CHANNEL]; // Surface reference (read and write) to atlas per channel
};

struct GVDBAtlasResourceDesc {
    AtlasFormat format;
    donut::gp::SamplerDesc samplerDesc;
    dm::float4 voidValue;
};

struct AtlasMapping;
class GVDBAllocator;

class GVDB: public donut::ObjectImpl<donut::IObject> {
 public:
    GVDB(donut::gp::IDeviceQueue *queue, donut::vfs::IFileSystem *vfs);
    ~GVDB();

    void configLevels(int r4, int r3, int r2, int r1, int r0);
    void configLevels(uint32_t numLevels, const uint32_t *levelLogDims, const uint32_t *numcnt, bool useBitmasks = false);
    void configAtlas(dm::uint3 defaultAtlasGridDim, int apron);

    void addChannel(const GVDBAtlasResourceDesc &resourceDesc);

    void destroyChannels();
 
    void clear();

    dm::uint getApron();

    dm::uint3 getAtlasResDim();

    dm::uint3 getAtlasGridDim();

    dm::uint3 getAtlasBlockDim();

    dm::uint3 getAtlasBlockDimWithoutApron();

    uint32_t getRootRange();

    uint8_t getNumLevels();

    uint64_t getNumUsedNodes(uint8_t level);

    // Gets the resolution (branching factor per side) of a node as a `dm::int3`.
    uint32_t getResDim(uint8_t lev);
    // Gets the lateral size a node at the given level covers in voxels.
    uint32_t getRange(int8_t lev);

    // Gets the size a node at the given level covers in voxels.
    dm::float3 getCover(uint8_t lev);

    dm::float3 getColorDim(uint8_t lev);

    uint64_t getVoxelCnt(uint8_t lev);

    Node *getNode(uint8_t lev, int idx);

    dm::box3 getNodeWorldBounds(Node *node);

    Extents computeExtents(Node *node);
    Extents computeExtents(uint8_t lev, const dm::box3 &bounds);

    int activateRegion(const Extents &e);
    int activateRegionFromMasks(const Extents &e, const uint8_t *masks);

    void copyAtlasBlock(int channel, dm::int3 dstOffset, donut::gp::IBuffer *srcBuffer, dm::uint3 srcBlockDim);

    void finishTopology();

    void updateAtlas();

    void updateApron();

    void resizeAtlasResources();

    donut::gp::IBuffer *getVBDInfoGPU() const;

    void convertBitmaskToNonBitmask();

    donut::FRESULT loadVBXC(donut::IDataBlob *pVBXC);

    donut::FRESULT storeVBXC(donut::IDataBlob **pVBXC);

 private:
    /**
     * @brief Activate space
     * @param currId  Starting sub-tree for activation
     * @param pos Index-space position to activate
     * @param newNode Returns true if the brick was added
     * @param stopId Specific node to stop activation
     * @param stopLevel Specific level to stop activation
     * @return Node id in \p stopLevel
     */
    NodeId activateSpace(NodeId currId, dm::int3 pos, bool &newNode, NodeId stopId = NodeId::null(), uint8_t stopLevel = 0);

    NodeId activateSpaceAtLevel(uint8_t level, dm::int3 pos);

    NodeId reparent(uint8_t level, NodeId prevrootId, dm::int3 pos, bool &newNode);

    dm::int3 getCoveringNodePos(uint8_t level, dm::int3 pos);

    NodeId addNode(uint8_t level, dm::int3 pos);

    NodeId insertChildNode(NodeId currId, NodeId childId, uint32_t i);

    NodeId insertChildNode(NodeId currId, uint32_t i);

    Node *getNode(NodeId id);

    bool getPosInNode(NodeId currId, dm::int3 pos, uint32_t *pBits = nullptr);

    uint32_t getBitPos(uint8_t level, dm::int3 pos);
    dm::int3 getPosFromBit(uint8_t level, uint32_t bits);

    NodeId getChildNode(NodeId currId, uint32_t b);

    uint32_t getLevelMaskBits(uint8_t lev);

    bool isOn(NodeId currId, uint32_t b);

    bool isLeaf(NodeId currId);

    dm::int3 getAtlasBlockPos(uint64_t nodeIdx);
    dm::int3 getAtlasPos(uint64_t nodeIdx);

    void updateAtlasMap();

    void prepareVDB();

    dm::box<int, 3> computeVolumeBounds(int lev);

    void resizeAtlasResource(int c);
    bool writeAtlasResource(int c, const void *data, uint32_t height, uint32_t rowPitch);

    struct LevelInfo {
        uint32_t logDim;
        uint32_t voxDim;
        uint64_t numVoxel;
        uint32_t range;
        uint32_t bitmasksBits;
    };

    struct AtlasMappingInfo {
        dm::uint3 defaultGridDim;
        dm::uint3 apron;
    };

    struct ChildIdPool {
        uint32_t maxNumVoxel;
        uint32_t numHeap;
        std::vector<NodeId> childList;
    };

    // Configuration
    // {
    // Hierarchy config
    std::vector<LevelInfo> m_levelInfos; // level information

    // Color for each level, for debug purpose
    std::vector<dm::float3> m_levelColors;

    // Atlas mapping config
    AtlasMappingInfo m_atlasMapInfo;

    // Atlas resource channels
    std::vector<GVDBAtlasResourceDesc> m_atlasResourceChannels;
    // }

    // Node structure
    // {
    // sparse node hierarchy
    // child index collection for each node
    std::unique_ptr<GVDBAllocator> m_allocator;
    NodeId m_rootNodeId;
    // }

    // Atlas structure
    // {
    dm::uint3 m_atlasBrickDim;
    dm::uint3 m_atlasBrickDimWithApron;
    dm::uint3 m_atlasGridDim;
    std::vector<AtlasNode> m_atlasMap;
    // }

    // GPU resources
    donut::AutoPtr<donut::gp::IDeviceQueue> m_gpQueue;
    std::vector<donut::AutoPtr<donut::gp::IBuffer>> m_nodePoolsGPU;
    std::vector<donut::AutoPtr<donut::gp::IBuffer>> m_childIdPoolsGPU;
    donut::AutoPtr<donut::gp::IBuffer> m_atlasMapGPU;
    std::vector<donut::AutoPtr<donut::gp::ITexture>> m_atlasResourceChannelsGPU;
    donut::AutoPtr<donut::gp::IBuffer> m_VDBInfoGPU;
    donut::AutoPtr<donut::gp::IKernel> m_updateApronKernels[4];

    // leaf node bounds in voxel index-space
    dm::box<int, 3> m_voxLeafBounds;
};

#pragma endregion "GVDB"

}  // namespace gvdb

#endif /* SRC_GVDBVOLUME_H */
