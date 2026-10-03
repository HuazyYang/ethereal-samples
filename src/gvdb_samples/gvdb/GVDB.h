#ifndef GVDB_GVDB_H
#define GVDB_GVDB_H
// GVDB sparse voxel database (port of NVIDIA GVDB Voxels 1.1.1) on the gp
// compute abstraction, as a set of COM-style interfaces:
//
//   IGVDBVolume      the data structure: level configuration, node pools
//                    (host arrays mirrored to GPU buffers), atlas map, channel
//                    textures, the VDBInfo block the kernels consume. It owns no
//                    algorithm beyond keeping the hierarchy consistent.
//   IGVDBVoxelOps    operators on channel data (fill, smooth, apron, resample ...)
//   IGVDBPointOps    point clouds -> topology / voxels (GPU rebuild, gather, scatter)
//   IGVDBVoxelizer   triangle meshes -> topology / voxels
//   IGVDBSerializer  VBX file I/O
//
// Operation objects are created for one volume (factory functions below) and
// only use the public IGVDBVolume interface, so new operations can be added
// without touching the data structure. Renderers (CUDA raycaster, OptiX) live
// in the samples and follow the same pattern.
//
// Coordinate conventions: "index space" is the voxel grid (one unit = one
// voxel); renderers map it to application space with their own transform.
// Bricks are the level-0 nodes; their voxels live in one 3D texture per
// channel ("atlas"), each brick padded by an apron of `apron` voxels.
#include <gvdb/GPDevice.h>
#include <nvrhi/core/foundation.h>
#include <nvrhi/core/autoptr.h>
#include <nvrhi/core/datablob.h>
#include <donut/core/math/math.h>
#include <vector>

namespace donut::vfs {
class IFileSystem;
}

namespace gvdb {

// ============================================================================
// Plain data types shared by the host interfaces and the kernels
// ============================================================================

#ifdef _WIN32
#define GVDB_ALIGN(x) __declspec(align(x))
#else
#define GVDB_ALIGN(x) __attribute__((aligned(x)))
#endif

#define GVDB_SCAN_BLOCKSIZE 512

// PTX modules of the library, as served by the file system given to createGVDBVolume().
#define GVDB_PTX_OPERATORS "ptx/GVDBOperators.ptx"
#define GVDB_PTX_PARTICLES "ptx/GVDBParticles.ptx"

enum { MAXLEV = 10 };
enum { MAX_CHANNEL = 32 };
enum { CHAN_UNDEF = 255 };            // "no colour channel" in the kernels
enum { TRANSFER_FUNC_SIZE = 16384 };  // float4 entries of a transfer function

using uchar = unsigned char;

enum AtlasFormat {
    ATLAS_FORMAT_R8_UINT,       // T_UCHAR
    ATLAS_FORMAT_RGBA8_UINT,    // T_UCHAR4
    ATLAS_FORMAT_R32_FLOAT,     // T_FLOAT
    ATLAS_FORMAT_RGB32_FLOAT,   // T_FLOAT3 (storage only: no apron / operator kernels)
    ATLAS_FORMAT_RGBA32_FLOAT,  // T_FLOAT4
};

uint32_t atlasFormatBytes(AtlasFormat format);
donut::gp::Format atlasFormatToGP(AtlasFormat format);
// GVDB channel type codes used in VBX files (T_UCHAR=0, T_UCHAR4=2, T_FLOAT=3, T_FLOAT3=4, T_FLOAT4=5).
int atlasFormatToVBXType(AtlasFormat format);
bool atlasFormatFromVBXType(int vbxType, AtlasFormat &format);

// Pool reference: group (0 = nodes, 1 = child lists), level, index. Same
// layout as the 64-bit ids the kernels decode (grp | lev << 8 | idx << 16).
struct NodeId {
    NodeId(uint8_t grp, uint8_t lev, uint64_t ndx) : grp{grp}, lev{lev}, idx{ndx} {}
    constexpr NodeId(uint64_t data = ~0ull) : data{data} {}
    static constexpr NodeId null() { return {}; }

    uint8_t group() const { return grp; }
    uint8_t level() const { return lev; }
    uint64_t index() const { return idx; }
    uint64_t raw() const { return data; }
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

// One node of the hierarchy. Same layout on host, GPU and in VBX files.
struct GVDB_ALIGN(16) Node {
    //                                                   Size(byte): Range:
    uint8_t level;          // Tree level                1           0~255
    uint8_t flags;          // Flags                     1           true - active, false - inactive
    uint8_t priority;       // Priority                  1
    uint8_t pad;            //                           1
    dm::int3 pos;           // Pos in Index space        12
    dm::int3 value;         // offset in atlas (bricks)  12
    dm::float3 range;       // Value min, max average    12
    NodeId parent;          // Parent ID pool reference  8
    NodeId childList;       // Child list index          8
    uint64_t mask;          // Start of BITMASK          8
                            // HEADER TOTAL              64
};
static_assert(sizeof(Node) == 64, "Node must be 64 bytes (VBX compatibility)");

// A node's children as a grid: used to iterate / activate a region.
struct GVDB_ALIGN(16) Extents {
    int lev;                    // level of the parent node
    dm::float3 vmin, vmax;      // index-space bounds of the children
    dm::int3 imin, imax, ires;  // child index extents (inclusive imin..imax) and count per axis
    dm::float3 cover;           // child width in voxels
    int icnt;                   // number of children covered
};

// Atlas slot -> brick (one entry per atlas slot, row-major over the atlas grid).
struct AtlasNode {
    dm::int3 pos;   // brick position in index space
    int leafNode;   // brick index in level 0, or -1 for an empty slot
};

// The GPU-side description of a volume, consumed by every kernel (VDBInfo in GVDB.cuh).
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
    int				brick_res; // Resolution of a single brick, including the apron
    int				top_lev; // Top level (i.e. tree spans from voxels to level 0 to level top_lev)
    dm::float3		bmin; // Inclusive minimum of axis-aligned bounding box in voxels
    dm::float3		bmax; // Inclusive maximum of axis-aligned bounding box in voxels
    donut::gp::NativeHandle	volIn[MAX_CHANNEL]; // Texture reference (read plus interpolation) to atlas per channel
    donut::gp::NativeHandle	volOut[MAX_CHANNEL]; // Surface reference (read and write) to atlas per channel
};

// ============================================================================
// Descriptors
// ============================================================================

// Hierarchy: log2 branching factor per level, level 0 = bricks. configure(3,3,3,3,5)
// in the reference is {numLevels=5, logDim={5,3,3,3,3}}.
struct GVDBLevelConfig {
    uint32_t numLevels = 0;
    uint32_t logDim[MAXLEV] = {};
    uint32_t initialNodes[MAXLEV] = {};   // initial pool reservation per level (0 = default)
    bool useBitmasks = false;             // reserved; non-bitmask child lists are used

    static GVDBLevelConfig fromBranching(int r4, int r3, int r2, int r1, int r0);
};

struct GVDBAtlasConfig {
    dm::uint3 gridDim = {16u, 16u, 1u};   // bricks per atlas axis; z grows on demand
    uint32_t apron = 1;                   // apron voxels around each brick (kernels assume 1)
};

struct GVDBChannelDesc {
    AtlasFormat format = ATLAS_FORMAT_R32_FLOAT;
    donut::gp::SamplerDesc samplerDesc;            // filtering / addressing of the atlas texture
    dm::float4 voidValue = dm::float4::zero();     // apron value outside the volume
};

// Read-only view of one host pool (nodes or child lists of one level).
struct GVDBPoolView {
    void *data = nullptr;        // first element
    uint64_t count = 0;          // allocated elements
    uint64_t stride = 0;         // bytes per element
    donut::gp::IBuffer *gpu = nullptr;   // GPU mirror (valid after commitPools / updateAtlas)
};

// A strided array of per-point attributes in a gp buffer.
struct PointData {
    donut::gp::IBuffer *buffer = nullptr;
    uint64_t offset = 0;    // byte offset of the first element
    uint32_t stride = 0;    // byte stride between elements
    uint32_t count = 0;
};

// Voxel operators of IGVDBVoxelOps::compute(). Parameters p1..p3 as in the
// reference kernels: Smooth (p1 = centre weight, p2 = bias), Noise (p1 =
// amplitude), Grow (p1), ClrExpand (p1, p2), ExpandC (p1 = match, p2 = fill).
enum class ComputeOp { Smooth, Noise, Grow, Cut, ClrExpand, ExpandC };

// Grid transform stored in VBX files (1.11+): index -> application space as
// pretrans, then scale, then rotation (euler degrees, ZYX), then trans.
struct VBXTransform {
    dm::float3 pretrans = dm::float3::zero();
    dm::float3 angles = dm::float3::zero();   // degrees
    dm::float3 scale = dm::float3{1.f};
    dm::float3 trans = dm::float3::zero();
};

struct GVDBUsage {
    dm::int3 extents = dm::int3::zero();   // voxel extents of the topology bounds
    uint64_t numBricks = 0;                // active bricks
    double millionVoxels = 0.0;
    float occupancyPercent = 0.f;          // active bricks / bricks covering the extents
    float topologyMB = 0.f;
    float atlasMB = 0.f;
    float gpuFreeMB = 0.f;
    float gpuTotalMB = 0.f;
};

// ============================================================================
// IGVDBVolume: the data structure
// ============================================================================

NVRHI_IID(IGVDBVolume, "6f1c2a40-5b7e-4d0a-9e3b-1a2c3d4e5f60")
struct IGVDBVolume : public nvrhi::IObject {
    NVRHI_DECLARE_UUID_TRAITS(IGVDBVolume)

    // ---- configuration ----
    virtual void configure(const GVDBLevelConfig &levels) = 0;
    virtual void configureAtlas(const GVDBAtlasConfig &atlas) = 0;
    virtual const GVDBLevelConfig &getLevelConfig() const = 0;
    virtual const GVDBAtlasConfig &getAtlasConfig() const = 0;
    virtual int addChannel(const GVDBChannelDesc &desc) = 0;   // returns the channel index
    virtual void destroyChannels() = 0;
    virtual int getNumChannels() const = 0;
    virtual const GVDBChannelDesc &getChannelDesc(int channel) const = 0;
    virtual donut::gp::ITexture *getChannelTexture(int channel) = 0;   // valid after updateAtlas()

    // ---- level geometry (functions of the configuration) ----
    virtual uint8_t getNumLevels() const = 0;
    virtual uint32_t getLogDim(uint8_t lev) const = 0;
    virtual uint32_t getResDim(uint8_t lev) const = 0;          // children per side
    virtual uint64_t getVoxelCnt(uint8_t lev) const = 0;        // children per node
    virtual uint32_t getRange(int8_t lev) const = 0;            // voxels per side covered by a node (-1 -> 1)
    virtual dm::float3 getCover(int8_t lev) const = 0;          // getRange as float3
    virtual dm::float3 getLevelColor(uint8_t lev) const = 0;    // debug colour per level
    virtual dm::uint3 getBrickDim() const = 0;                  // voxels per brick side
    virtual dm::uint3 getBrickDimWithApron() const = 0;
    virtual Extents computeExtents(Node *node) = 0;
    virtual Extents computeExtents(uint8_t lev, const dm::box3 &bounds) = 0;

    // ---- node pools (host data) ----
    virtual NodeId getRootId() const = 0;
    virtual uint64_t getNumNodes(uint8_t lev) const = 0;        // allocated nodes (bricks may be inactive: flags == 0)
    virtual Node *getNode(uint8_t lev, uint64_t idx) = 0;
    virtual Node *getNode(NodeId id) = 0;
    virtual NodeId *getChildList(Node *node) = 0;               // getVoxelCnt(level) entries, or nullptr
    virtual GVDBPoolView getNodePool(uint8_t lev) = 0;
    virtual GVDBPoolView getChildListPool(uint8_t lev) = 0;
    // Low-level editing for GPU topology builders: allocates an unlinked node /
    // child list (child list entries set to NodeId::null()).
    virtual NodeId allocateNode(uint8_t lev, dm::int3 pos, bool active = true) = 0;
    virtual NodeId allocateChildList(uint8_t lev) = 0;
    virtual void setRoot(NodeId root) = 0;
    virtual dm::box3 getNodeBounds(Node *node) = 0;            // index-space bounds

    // ---- topology editing (host) ----
    virtual void clear() = 0;                                   // empties pools and atlas map; keeps config and channels
    virtual NodeId activateSpace(dm::float3 pos) = 0;           // activates the brick covering a point
    virtual NodeId activateSpaceAtLevel(uint8_t lev, dm::int3 pos) = 0;
    virtual int activateRegion(const Extents &e) = 0;
    virtual int activateRegionFromMasks(const Extents &e, const uint8_t *masks) = 0;
    virtual int activateRegionFromValues(const Extents &e, const float *values, float threshold) = 0;
    virtual int activateHalo(const Extents &e) = 0;             // inactive children with an active face neighbour
    virtual bool isActive(dm::int3 pos) = 0;
    // Recomputes the voxel bounds and the active-brick count, commits the pools.
    virtual void finishTopology(bool computeBounds = true) = 0;
    virtual dm::box<int, 3> getVoxelBounds() const = 0;
    virtual uint64_t getNumActiveBricks() const = 0;

    // ---- atlas ----
    // Assigns atlas slots to the active bricks, uploads pools and atlas map,
    // (re)creates channel textures. Call after topology changes.
    virtual void updateAtlas() = 0;
    virtual dm::uint3 getAtlasGridDim() const = 0;              // bricks per atlas axis
    virtual dm::uint3 getAtlasResDim() const = 0;               // voxels (with aprons)
    virtual dm::uint3 getAtlasPackedResDim() const = 0;         // voxels (without aprons)
    virtual const std::vector<AtlasNode> &getAtlasMap() const = 0;
    virtual donut::gp::IBuffer *getAtlasMapGPU() = 0;
    // Copies a tightly packed block of srcBlockDim voxels into a channel at dstOffset (voxels).
    virtual void copyAtlasBlock(int channel, dm::int3 dstOffset, donut::gp::IBuffer *src, dm::uint3 srcBlockDim) = 0;

    // ---- GPU mirror ----
    virtual donut::gp::IDevice *getDevice() const = 0;
    virtual donut::gp::IDeviceQueue *getQueue() const = 0;
    virtual void commitPools() = 0;                             // host pools -> GPU
    virtual void fetchPools() = 0;                              // GPU -> host pools (after GPU topology edits)
    virtual void fillVDBInfo(VDBInfo &info, bool withAtlas) const = 0;
    virtual void invalidateVDBInfo() = 0;
    virtual void prepareVDB() = 0;                              // uploads VDBInfo when out of date
    virtual void prepareVDBPartially() = 0;                     // VDBInfo without atlas (GPU topology kernels)
    virtual donut::gp::IBuffer *getVDBInfoGPU() = 0;
    // A kernel of the library PTX modules, cached by name (shared by the operation objects).
    virtual donut::gp::IKernel *getKernel(const char *name) = 0;

    // ---- statistics ----
    virtual GVDBUsage getUsage() = 0;
    virtual void logMeasure() = 0;
};

// ============================================================================
// Operations
// ============================================================================

NVRHI_IID(IGVDBVoxelOps, "6f1c2a40-5b7e-4d0a-9e3b-1a2c3d4e5f61")
struct IGVDBVoxelOps : public nvrhi::IObject {
    NVRHI_DECLARE_UUID_TRAITS(IGVDBVoxelOps)

    virtual IGVDBVolume *getVolume() const = 0;

    virtual void fillChannel(int channel, dm::float4 value) = 0;
    virtual void clearChannel(int channel) = 0;
    virtual void clearAllChannels() = 0;
    // Runs an operator over the atlas of a channel (8x8x8 blocks); params = p1..p3.
    virtual void compute(ComputeOp op, int channel, int numIterations, dm::float3 params, bool updateApron,
                         bool skipOverAprons, float boundValue = 0.f) = 0;
    // Runs a user kernel `k(VDBInfo*, int3 atlasRes, uchar channel)` over the atlas.
    virtual void computeKernel(donut::gp::IKernel *kernel, int channel, bool updateApron, bool skipOverAprons) = 0;
    virtual void updateApron() = 0;                              // all channels, boundary = voidValue
    virtual void updateApron(int channel, float boundValue) = 0;
    // Samples a dense float volume (src, srcRes) into the channel. xform maps
    // index space to source voxels; values remapped from inRange to outRange.
    virtual void resample(int channel, const dm::float4x4 &xform, dm::int3 srcRes, donut::gp::IBuffer *src,
                          dm::float3 inRange, dm::float3 outRange) = 0;
    // Averages the dense source onto a dstRes grid covering [0, dstMax]; read back to the host.
    virtual void downsample(const dm::float4x4 &xform, dm::int3 srcRes, donut::gp::IBuffer *src, dm::int3 dstRes,
                            dm::float3 dstMax, dm::float3 inRange, dm::float3 outRange,
                            std::vector<float> &outValues) = 0;
    virtual float reduction(int channel) = 0;                    // sum of a float channel
};

NVRHI_IID(IGVDBPointOps, "6f1c2a40-5b7e-4d0a-9e3b-1a2c3d4e5f62")
struct IGVDBPointOps : public nvrhi::IObject {
    NVRHI_DECLARE_UUID_TRAITS(IGVDBPointOps)

    virtual IGVDBVolume *getVolume() const = 0;

    // Points used by every call below. vel / clr may be empty (buffer == nullptr).
    virtual void setPoints(const PointData &pos, const PointData &vel, const PointData &clr) = 0;
    virtual dm::box3 computePointBounds(uint32_t numPnts, dm::float3 trans) = 0;   // on the GPU

    // Topology from points (GPU): every point (+ radius) ends up inside a brick.
    virtual void rebuildTopology(uint32_t numPnts, float radius, dm::float3 origin) = 0;
    virtual void accumulateTopology(uint32_t numPnts, float radius, dm::float3 origin, int minDepth = 1) = 0;
    virtual void requestFullRebuild(bool rebuild) = 0;

    // Points to voxels. insertPointsSubcell bins the points into brick sub-cells;
    // scPntsLength receives the number of (point, sub-cell) pairs for the gathers.
    virtual void insertPointsSubcell(int subcellSize, uint32_t numPnts, float radius, dm::float3 trans,
                                     int &scPntsLength) = 0;
    virtual void insertPointsSubcellFP16(int subcellSize, uint32_t numPnts, float radius, dm::float3 trans,
                                         int &scPntsLength) = 0;
    virtual void gatherDensity(int subcellSize, uint32_t numPnts, float radius, dm::float3 trans, int scPntsLength,
                               int chanDensity, int chanClr, bool accumulate = false) = 0;
    virtual void gatherLevelSet(int subcellSize, uint32_t numPnts, float radius, dm::float3 trans, int scPntsLength,
                                int chanLevelSet, int chanClr, bool accumulate = false) = 0;
    virtual void gatherLevelSetFP16(int subcellSize, uint32_t numPnts, float radius, dm::float3 trans,
                                    int scPntsLength, int chanLevelSet, int chanClr) = 0;
    virtual void insertPoints(uint32_t numPnts, dm::float3 trans, bool prefix = false) = 0;
    virtual void scatterDensity(uint32_t numPnts, float radius, float amp, dm::float3 trans, bool expand = true,
                                bool avgColor = false) = 0;
    // Converts packed positions (bits: 1 = byte, 2 = ushort, 4 = float, 8 = double per
    // component): dst = (wMin + src * wDelta) * scale + trans.
    virtual void convertAndTransform(const PointData &src, int srcBits, const PointData &dst, int dstBits,
                                     uint32_t numPnts, dm::float3 wMin, dm::float3 wDelta, dm::float3 trans,
                                     dm::float3 scale) = 0;
    virtual void scalePointPositions(uint32_t numPnts, float scale) = 0;
};

NVRHI_IID(IGVDBVoxelizer, "6f1c2a40-5b7e-4d0a-9e3b-1a2c3d4e5f63")
struct IGVDBVoxelizer : public nvrhi::IObject {
    NVRHI_DECLARE_UUID_TRAITS(IGVDBVoxelizer)

    virtual IGVDBVolume *getVolume() const = 0;

    // Rebuilds the topology from a triangle mesh (float3 vertices, uint3 indices
    // in gp buffers) and writes valSurface / valInside into the channel.
    // matModelToIndex maps model space to index space.
    virtual nvrhi::FRESULT solidVoxelize(int channel, donut::gp::IBuffer *vertices, uint32_t numVertices,
                                         donut::gp::IBuffer *indices, uint32_t numIndices,
                                         const dm::box3 &modelBounds, const dm::affine3 &matModelToIndex,
                                         float valSurface, float valInside) = 0;
};

NVRHI_IID(IGVDBSerializer, "6f1c2a40-5b7e-4d0a-9e3b-1a2c3d4e5f64")
struct IGVDBSerializer : public nvrhi::IObject {
    NVRHI_DECLARE_UUID_TRAITS(IGVDBSerializer)

    virtual IGVDBVolume *getVolume() const = 0;

    // VBX (GVDB 1.0 .. 2.0 headers). outTransform receives the grid transform (1.11+).
    virtual nvrhi::FRESULT loadVBX(nvrhi::IDataBlob *vbx, VBXTransform *outTransform = nullptr) = 0;
    virtual nvrhi::FRESULT saveVBX(nvrhi::IDataBlob **outVbx, const VBXTransform *transform = nullptr) = 0;
};

// ============================================================================
// Factories
// ============================================================================

// vfs must serve GVDB_PTX_OPERATORS and GVDB_PTX_PARTICLES.
nvrhi::FRESULT createGVDBVolume(donut::gp::IDeviceQueue *queue, donut::vfs::IFileSystem *vfs, IGVDBVolume **volume);
nvrhi::FRESULT createGVDBVoxelOps(IGVDBVolume *volume, IGVDBVoxelOps **ops);
nvrhi::FRESULT createGVDBPointOps(IGVDBVolume *volume, IGVDBPointOps **ops);
nvrhi::FRESULT createGVDBVoxelizer(IGVDBVolume *volume, IGVDBVoxelizer **voxelizer);
nvrhi::FRESULT createGVDBSerializer(IGVDBVolume *volume, IGVDBSerializer **serializer);

}  // namespace gvdb

#endif /* GVDB_GVDB_H */
