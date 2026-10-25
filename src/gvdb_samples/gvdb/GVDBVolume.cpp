// IGVDBVolume: the GVDB hierarchy (port of VolumeGVDB + the pool / atlas half
// of Allocator onto the nvrhi CUDA backend).
//
// Host pools hold the nodes and child lists; commitPools() mirrors them into
// buffers of the CUDA device sized to the allocated count. Bricks (level-0 nodes) that are
// active (flags != 0) get a slot in the atlas textures in updateAtlas(); the
// atlas map gives the inverse mapping (slot -> brick). The VDBInfo block the
// kernels read is rebuilt lazily by prepareVDB().
#include <gvdb/GVDB.h>
#include "GVDBAllocator.h"
#include "GVDBPrimitives.h"
#include <donut/core/log.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

namespace gvdb {

using namespace donut;

namespace {

constexpr uint32_t DEFAULT_INITIAL_NODES = 4;
constexpr double MB = 1024.0 * 1024.0;

struct LevelInfo {
    uint32_t logDim = 1;   // log2 children per side
    uint32_t resDim = 2;   // children per side
    uint64_t voxelCnt = 8; // children per node
    uint32_t range = 2;    // voxels per side covered by a node
};

struct PoolGPU {
    nvrhi::BufferHandle buffer;
    uint64_t capacity = 0;   // bytes
};

// The containers the kernels of the library come from (getKernel()).
const char *const s_shaderContainers[] = {GVDB_SHADERS_OPERATORS, GVDB_SHADERS_PARTICLES, GVDB_SHADERS_PRIMITIVES};

class GVDBVolume final : public nvrhi::ObjectImpl<IGVDBVolume> {
 public:
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(GVDBVolume)
    NVRHI_IMPLEMENTS_INTERFACE(IGVDBVolume)
    NVRHI_END_INTERFACE_TABLE()

    explicit GVDBVolume(IComputeContext *context)
        : m_context(context), m_device(context->getDevice()), m_primitives(context) {
        m_atlasGridDim = m_atlasCfg.gridDim;
    }

    ~GVDBVolume() {
        // GPU work on the pools / atlas may still be in flight
        if (m_context) m_context->finish();
    }

    // ---- configuration -------------------------------------------------

    void configure(const GVDBLevelConfig &levels) override {
        uint32_t n = std::min<uint32_t>(levels.numLevels, MAXLEV);
        if (n == 0) {
            log::error("GVDB: configure() needs at least one level");
            return;
        }
        m_levelCfg = levels;
        m_levelCfg.numLevels = n;
        m_levels.assign(n, {});
        for (uint32_t i = 0; i < n; ++i) {
            LevelInfo &li = m_levels[i];
            li.logDim = levels.logDim[i] == 0 ? 1 : levels.logDim[i];
            li.resDim = 1u << li.logDim;
            li.voxelCnt = uint64_t(li.resDim) * li.resDim * li.resDim;
            li.range = i == 0 ? li.resDim : m_levels[i - 1].range * li.resDim;
            m_levelCfg.logDim[i] = li.logDim;
            if (m_levelCfg.initialNodes[i] == 0) m_levelCfg.initialNodes[i] = DEFAULT_INITIAL_NODES;
        }
        for (uint32_t i = n; i < MAXLEV; ++i) {
            m_levelCfg.logDim[i] = 0;
            m_levelCfg.initialNodes[i] = 0;
        }

        // host pools: nodes (group 0) and child lists (group 1; none for bricks)
        m_pool.poolReleaseAll();
        for (uint32_t i = 0; i < n; ++i) m_pool.poolCreate(0, uint8_t(i), sizeof(Node), m_levelCfg.initialNodes[i]);
        m_pool.poolCreate(1, 0, 0, 0);
        for (uint32_t i = 1; i < n; ++i)
            m_pool.poolCreate(1, uint8_t(i), sizeof(NodeId) * m_levels[i].voxelCnt, m_levelCfg.initialNodes[i]);

        // GPU mirrors are stale; they are recreated by the next commit
        for (auto &p : m_nodePoolsGPU) dropPool(p);
        for (auto &p : m_childPoolsGPU) dropPool(p);

        m_voxResMax = dm::int3::zero();
        clear();
        ensureChannelTextures();
    }

    void configureAtlas(const GVDBAtlasConfig &atlas) override {
        m_atlasCfg = atlas;
        m_atlasCfg.gridDim = dm::max(m_atlasCfg.gridDim, dm::uint3(1u));
        m_atlasGridDim = m_atlasCfg.gridDim;
        ensureChannelTextures();
        invalidateVDBInfo();
    }

    const GVDBLevelConfig &getLevelConfig() const override { return m_levelCfg; }
    const GVDBAtlasConfig &getAtlasConfig() const override { return m_atlasCfg; }

    int addChannel(const GVDBChannelDesc &desc) override {
        if (m_channels.size() >= MAX_CHANNEL) {
            log::error("GVDB: addChannel(): the maximum of %d channels is reached", int(MAX_CHANNEL));
            return -1;
        }
        m_channels.push_back(desc);
        m_channelTextures.emplace_back();
        m_channelSamplers.emplace_back();
        int channel = int(m_channels.size()) - 1;
        ensureChannelTexture(channel);
        invalidateVDBInfo();
        return channel;
    }

    void destroyChannels() override {
        for (auto &texture : m_channelTextures) m_context->releaseAfterUse(texture.Get());
        m_channels.clear();
        m_channelTextures.clear();
        m_channelSamplers.clear();
        invalidateVDBInfo();
    }

    int getNumChannels() const override { return int(m_channels.size()); }

    const GVDBChannelDesc &getChannelDesc(int channel) const override {
        static const GVDBChannelDesc s_default;
        if (channel < 0 || channel >= int(m_channels.size())) return s_default;
        return m_channels[channel];
    }

    nvrhi::ITexture *getChannelTexture(int channel) override {
        if (channel < 0 || channel >= int(m_channelTextures.size())) return nullptr;
        return m_channelTextures[channel].Get();
    }

    // ---- level geometry ------------------------------------------------

    uint8_t getNumLevels() const override { return uint8_t(m_levels.size()); }

    uint32_t getLogDim(uint8_t lev) const override { return lev < m_levels.size() ? m_levels[lev].logDim : 0; }
    uint32_t getResDim(uint8_t lev) const override { return lev < m_levels.size() ? m_levels[lev].resDim : 1; }
    uint64_t getVoxelCnt(uint8_t lev) const override { return lev < m_levels.size() ? m_levels[lev].voxelCnt : 1; }

    uint32_t getRange(int8_t lev) const override {
        if (lev < 0) return 1;
        if (uint8_t(lev) >= m_levels.size()) return m_levels.empty() ? 1 : m_levels.back().range;
        return m_levels[lev].range;
    }

    dm::float3 getCover(int8_t lev) const override { return dm::float3(float(getRange(lev))); }

    dm::float3 getLevelColor(uint8_t lev) const override {
        static const dm::float3 s_colors[] = {
            {0.f, 0.f, 1.f},     // blue
            {0.f, 1.f, 0.f},     // green
            {1.f, 0.f, 0.f},     // red
            {1.f, 1.f, 0.f},     // yellow
            {1.f, 0.f, 1.f},     // purple
            {0.f, 1.f, 1.f},     // aqua
            {1.f, 0.5f, 0.f},    // orange
            {0.f, 0.5f, 1.f},    // green-blue
            {0.7f, 0.7f, 0.7f},  // grey
        };
        constexpr size_t n = sizeof(s_colors) / sizeof(s_colors[0]);
        return s_colors[lev < n ? lev : n - 1];
    }

    dm::uint3 getBrickDim() const override { return dm::uint3(getResDim(0)); }
    dm::uint3 getBrickDimWithApron() const override { return getBrickDim() + dm::uint3(2u * m_atlasCfg.apron); }

    Extents computeExtents(Node *node) override {
        Extents e = {};
        if (!node) return e;
        e.lev = node->level;
        int range = int(getRange(int8_t(e.lev - 1)));
        e.cover = getCover(int8_t(e.lev - 1));
        e.imin = node->pos / range;
        e.imax = e.imin + dm::int3(int(getResDim(uint8_t(e.lev))) - 1);
        e.vmin = dm::float3(e.imin) * e.cover;
        e.vmax = dm::float3(e.imax + dm::int3(1)) * e.cover;
        e.ires = e.imax - e.imin + dm::int3(1);
        e.icnt = e.ires.x * e.ires.y * e.ires.z;
        return e;
    }

    Extents computeExtents(uint8_t lev, const dm::box3 &bounds) override {
        Extents e = {};
        e.lev = lev;
        e.vmin = bounds.lower();
        e.vmax = bounds.upper();
        e.cover = getCover(int8_t(lev - 1));
        e.imin = dm::int3(e.vmin / e.cover);
        // Reference semantics: subtract in float, then truncate toward zero
        // (so a top level whose cover exceeds the bounds still yields imax = 0).
        e.imax = dm::int3(e.vmax / e.cover - dm::float3(1.f));
        e.ires = e.imax - e.imin + dm::int3(1);
        e.icnt = e.ires.x * e.ires.y * e.ires.z;
        return e;
    }

    // ---- node pools ----------------------------------------------------

    NodeId getRootId() const override { return m_root; }

    uint64_t getNumNodes(uint8_t lev) const override { return m_pool.poolGetNumUsed(0, lev); }

    Node *getNode(uint8_t lev, uint64_t idx) override {
        if (lev >= m_levels.size() || idx >= m_pool.poolGetNumUsed(0, lev)) return nullptr;
        return (Node *)m_pool.poolData(NodeId{0, lev, idx});
    }

    Node *getNode(NodeId id) override {
        if (id == NodeId::null() || id.group() != 0) return nullptr;
        return getNode(id.level(), id.index());
    }

    NodeId *getChildList(Node *node) override {
        if (!node || node->childList == NodeId::null()) return nullptr;
        return (NodeId *)m_pool.poolData(node->childList);
    }

    GVDBPoolView getNodePool(uint8_t lev) override {
        GVDBPoolView v;
        v.data = m_pool.poolData(0, lev);
        v.count = m_pool.poolGetNumUsed(0, lev);
        v.stride = m_pool.poolGetElementStride(0, lev);
        v.gpu = lev < MAXLEV ? m_nodePoolsGPU[lev].buffer.Get() : nullptr;
        return v;
    }

    GVDBPoolView getChildListPool(uint8_t lev) override {
        GVDBPoolView v;
        v.data = m_pool.poolData(1, lev);
        v.count = m_pool.poolGetNumUsed(1, lev);
        v.stride = m_pool.poolGetElementStride(1, lev);
        v.gpu = lev < MAXLEV ? m_childPoolsGPU[lev].buffer.Get() : nullptr;
        return v;
    }

    NodeId allocateNode(uint8_t lev, dm::int3 pos, bool active) override {
        if (lev >= m_levels.size()) {
            log::error("GVDB: allocateNode(): level %d is not configured", lev);
            return NodeId::null();
        }
        NodeId id = m_pool.poolAlloc(0, lev);
        if (id == NodeId::null()) return id;
        setupNode((Node *)m_pool.poolData(id), lev, pos, active);
        return id;
    }

    NodeId allocateChildList(uint8_t lev) override {
        if (lev == 0 || lev >= m_levels.size()) {
            log::error("GVDB: allocateChildList(): level %d has no child lists", lev);
            return NodeId::null();
        }
        NodeId id = m_pool.poolAlloc(1, lev);
        if (id == NodeId::null()) return id;
        memset(m_pool.poolData(id), 0xFF, size_t(m_pool.poolGetElementStride(1, lev)));
        return id;
    }

    void setRoot(NodeId root) override {
        m_root = root;
        invalidateVDBInfo();
    }

    dm::box3 getNodeBounds(Node *node) override {
        if (!node) return dm::box3::empty();
        dm::float3 lo(node->pos);
        return dm::box3(lo, lo + getCover(int8_t(node->level)));
    }

    // ---- topology editing ---------------------------------------------

    void clear() override {
        m_pool.poolClearAll();
        m_root = NodeId::null();
        m_atlasMap.clear();
        m_numActiveBricks = 0;
        m_voxBounds = dm::box<int, 3>::empty();
        invalidateVDBInfo();
    }

    NodeId activateSpace(dm::float3 pos) override {
        if (m_levels.empty()) return NodeId::null();
        bool bNew = false;
        dm::int3 ipos(std::floor(pos.x), std::floor(pos.y), std::floor(pos.z));
        return activateSpaceRec(m_root, ipos, bNew, NodeId::null(), 0);
    }

    NodeId activateSpaceAtLevel(uint8_t lev, dm::int3 pos) override {
        if (lev >= m_levels.size()) return NodeId::null();
        bool bNew = false;
        return activateSpaceRec(m_root, pos, bNew, NodeId::null(), lev);
    }

    int activateRegion(const Extents &e) override {
        if (e.lev <= 0) return 0;
        uint8_t lev = uint8_t(e.lev - 1);
        dm::int3 cover(e.cover);
        int cnt = 0;
        for (int z = e.imin.z; z <= e.imax.z; ++z)
            for (int y = e.imin.y; y <= e.imax.y; ++y)
                for (int x = e.imin.x; x <= e.imax.x; ++x) {
                    activateSpaceAtLevel(lev, dm::int3(x, y, z) * cover);
                    ++cnt;
                }
        return cnt;
    }

    int activateRegionFromMasks(const Extents &e, const uint8_t *masks) override {
        if (e.lev <= 0 || !masks) return 0;
        uint8_t lev = uint8_t(e.lev - 1);
        dm::int3 cover(e.cover);
        int cnt = 0;
        for (int z = e.imin.z; z <= e.imax.z; ++z)
            for (int y = e.imin.y; y <= e.imax.y; ++y)
                for (int x = e.imin.x; x <= e.imax.x; ++x) {
                    if (!masks[extentIndex(e, x, y, z)]) continue;
                    activateSpaceAtLevel(lev, dm::int3(x, y, z) * cover);
                    ++cnt;
                }
        return cnt;
    }

    int activateRegionFromValues(const Extents &e, const float *values, float threshold) override {
        if (e.lev <= 0 || !values) return 0;
        uint8_t lev = uint8_t(e.lev - 1);
        dm::int3 cover(e.cover);
        int cnt = 0;
        for (int z = e.imin.z; z <= e.imax.z; ++z)
            for (int y = e.imin.y; y <= e.imax.y; ++y)
                for (int x = e.imin.x; x <= e.imax.x; ++x) {
                    if (!(values[extentIndex(e, x, y, z)] > threshold)) continue;
                    activateSpaceAtLevel(lev, dm::int3(x, y, z) * cover);
                    ++cnt;
                }
        return cnt;
    }

    int activateHalo(const Extents &e) override {
        if (e.lev <= 0 || e.icnt <= 0) return 0;
        uint8_t lev = uint8_t(e.lev - 1);
        dm::int3 cover(e.cover);
        std::vector<uint8_t> tags(size_t(e.icnt), 0);

        static const dm::int3 s_nbr[6] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};

        // tag inactive cells with an active face neighbour
        for (int z = e.imin.z; z <= e.imax.z; ++z)
            for (int y = e.imin.y; y <= e.imax.y; ++y)
                for (int x = e.imin.x; x <= e.imax.x; ++x) {
                    dm::int3 pos = dm::int3(x, y, z) * cover;
                    if (isActive(pos)) continue;
                    for (const auto &d : s_nbr) {
                        if (isActive(pos + d * cover)) {
                            tags[extentIndex(e, x, y, z)] = 1;
                            break;
                        }
                    }
                }

        // activate the tags
        int cnt = 0;
        for (int z = e.imin.z; z <= e.imax.z; ++z)
            for (int y = e.imin.y; y <= e.imax.y; ++y)
                for (int x = e.imin.x; x <= e.imax.x; ++x) {
                    if (!tags[extentIndex(e, x, y, z)]) continue;
                    activateSpaceAtLevel(lev, dm::int3(x, y, z) * cover);
                    ++cnt;
                }
        return cnt;
    }

    bool isActive(dm::int3 pos) override {
        if (m_root == NodeId::null()) return false;
        return isActiveRec(pos, m_root);
    }

    void finishTopology(bool computeBounds) override {
        if (computeBounds) computeVoxelBounds();
        m_numActiveBricks = countActiveBricks();
        commitPools();
        invalidateVDBInfo();
    }

    dm::box<int, 3> getVoxelBounds() const override { return m_voxBounds; }

    uint64_t getNumActiveBricks() const override { return m_numActiveBricks; }

    // ---- atlas ---------------------------------------------------------

    void updateAtlas() override {
        if (m_levels.empty()) {
            log::error("GVDB: updateAtlas(): configure() was not called");
            return;
        }
        const uint64_t leafCnt = getNumNodes(0);
        const uint64_t active = countActiveBricks();
        m_numActiveBricks = active;

        // grow the atlas along z when the active bricks do not fit
        const uint64_t perLayer = uint64_t(m_atlasGridDim.x) * m_atlasGridDim.y;
        const uint32_t needZ = uint32_t(std::max<uint64_t>(1, (active + perLayer - 1) / perLayer));
        if (needZ > m_atlasGridDim.z) m_atlasGridDim.z = needZ;

        // assign slots to the active bricks in pool order
        const dm::int3 bwa(getBrickDimWithApron());
        const int apron = int(m_atlasCfg.apron);
        uint64_t slot = 0;
        for (uint64_t n = 0; n < leafCnt; ++n) {
            Node *node = (Node *)m_pool.poolData(NodeId{0, 0, n});
            if (!node->flags) {
                node->value = dm::int3(-1);
                continue;
            }
            node->value = atlasBlockIndex(slot++) * bwa + dm::int3(apron);
        }

        // atlas map: slot -> brick
        const uint64_t total = perLayer * m_atlasGridDim.z;
        m_atlasMap.assign(size_t(total), AtlasNode{dm::int3(-1), -1});
        for (uint64_t n = 0; n < leafCnt; ++n) {
            Node *node = (Node *)m_pool.poolData(NodeId{0, 0, n});
            if (!node->flags) continue;
            dm::int3 b = node->value / bwa;
            uint64_t linear = (uint64_t(b.z) * m_atlasGridDim.y + b.y) * m_atlasGridDim.x + b.x;
            if (linear >= total) {
                log::error("GVDB: updateAtlas(): brick %llu exceeds the atlas (%d %d %d)", (unsigned long long)n,
                           node->value.x, node->value.y, node->value.z);
                continue;
            }
            m_atlasMap[size_t(linear)] = AtlasNode{node->pos, int(n)};
        }

        commitPools();
        uploadAtlasMap();
        ensureChannelTextures();
        invalidateVDBInfo();
    }

    dm::uint3 getAtlasGridDim() const override { return m_atlasGridDim; }
    dm::uint3 getAtlasResDim() const override { return m_atlasGridDim * getBrickDimWithApron(); }
    dm::uint3 getAtlasPackedResDim() const override { return m_atlasGridDim * getBrickDim(); }
    const std::vector<AtlasNode> &getAtlasMap() const override { return m_atlasMap; }
    nvrhi::IBuffer *getAtlasMapGPU() override { return m_atlasMapGPU.Get(); }

    void copyAtlasBlock(int channel, dm::int3 dstOffset, nvrhi::IBuffer *src, dm::uint3 srcBlockDim) override {
        nvrhi::ITexture *dst = getChannelTexture(channel);
        if (!dst || !src) {
            log::error("GVDB: copyAtlasBlock(): channel %d has no atlas texture", channel);
            return;
        }
        m_primitives.copyBlock(src, srcBlockDim, dstOffset, dst, m_channels[channel].format);
    }

    // ---- GPU mirror ----------------------------------------------------

    nvrhi::cuda::IDevice *getDevice() const override { return m_device.Get(); }
    IComputeContext *getContext() const override { return m_context.Get(); }

    void commitPools() override {
        for (uint8_t lev = 0; lev < m_levels.size(); ++lev) {
            commitPool(0, lev, m_nodePoolsGPU[lev]);
            commitPool(1, lev, m_childPoolsGPU[lev]);
        }
        invalidateVDBInfo();
    }

    void fetchPools() override {
        // Every pool is copied into one readback buffer, read after one wait.
        struct Job {
            nvrhi::IBuffer *src;
            void *dst;
            uint64_t offset;
            uint64_t bytes;
        };
        std::vector<Job> jobs;
        uint64_t total = 0;
        auto queueFetch = [&](uint8_t grp, uint8_t lev, PoolGPU &gpu) {
            uint64_t bytes = m_pool.poolGetMemoryUsedWidth(grp, lev);
            if (bytes == 0 || !gpu.buffer) return;
            if (gpu.capacity < bytes) {
                log::error("GVDB: fetchPools(): pool (%d, %d) GPU mirror is smaller than the host pool", grp, lev);
                return;
            }
            jobs.push_back({gpu.buffer.Get(), m_pool.poolData(grp, lev), total, bytes});
            total += (bytes + 15) & ~uint64_t(15);
        };
        for (uint8_t lev = 0; lev < m_levels.size(); ++lev) {
            queueFetch(0, lev, m_nodePoolsGPU[lev]);
            queueFetch(1, lev, m_childPoolsGPU[lev]);
        }
        if (jobs.empty()) return;

        if (!m_fetchBuffer || m_fetchBuffer->getDesc().byteSize < total) {
            nvrhi::BufferDesc desc;
            desc.byteSize = total;
            desc.cpuAccess = nvrhi::CpuAccessMode::Read;
            desc.debugName = "GVDB pool readback";
            m_fetchBuffer = nullptr;
            if (NVRHI_FAILED(m_device->createBuffer(desc, &m_fetchBuffer))) {
                log::error("GVDB: fetchPools(): cannot create a readback buffer of %llu bytes", (unsigned long long)total);
                return;
            }
        }
        nvrhi::ICommandList *commandList = m_context->getCommandList();
        for (const Job &job : jobs) commandList->copyBuffer(m_fetchBuffer, job.offset, job.src, 0, job.bytes);
        m_context->finish();

        const uint8_t *mapped = static_cast<const uint8_t *>(m_device->mapBuffer(m_fetchBuffer, nvrhi::CpuAccessMode::Read));
        if (mapped) {
            for (const Job &job : jobs) memcpy(job.dst, mapped + job.offset, size_t(job.bytes));
        }
        m_device->unmapBuffer(m_fetchBuffer);
        invalidateVDBInfo();
    }

    void fillVDBInfo(VDBInfo &info, bool withAtlas) const override {
        memset(&info, 0, sizeof(info));
        const int levs = int(m_levels.size());
        int tlev = 1;
        for (int n = levs - 1; n >= 0; --n) {
            const uint8_t lev = uint8_t(n);
            info.dim[n] = int(m_levels[n].logDim);
            info.res[n] = int(m_levels[n].resDim);
            info.vdel[n] = dm::float3(float(m_levels[n].range) / float(m_levels[n].resDim));
            info.noderange[n] = dm::int3(int(m_levels[n].range));
            info.nodecnt[n] = int(m_pool.poolGetNumUsed(0, lev));
            info.nodewid[n] = int(m_pool.poolGetElementStride(0, lev));
            info.childwid[n] = int(m_pool.poolGetElementStride(1, lev));
            info.nodelist[n] = deviceAddress(m_nodePoolsGPU[n].buffer);
            info.childlist[n] = deviceAddress(m_childPoolsGPU[n].buffer);
            if (info.nodecnt[n] == 1) tlev = n;   // top level for rendering (reference loop)
        }
        info.top_lev = tlev;
        if (m_voxBounds.isempty()) {
            info.bmin = info.bmax = dm::float3::zero();
        } else {
            info.bmin = dm::float3(m_voxBounds.lower());
            info.bmax = dm::float3(m_voxBounds.upper());
        }

        if (withAtlas) {
            info.atlas_map = deviceAddress(m_atlasMapGPU);
            info.atlas_cnt = dm::int3(m_atlasGridDim);
            info.atlas_res = dm::int3(getAtlasResDim());
            info.atlas_apron = int(m_atlasCfg.apron);
            info.brick_res = int(getBrickDimWithApron().x);   // brick side INCLUDING the apron
            const dm::uint3 atlasRes = getAtlasResDim();
            info.atlas_rcp = dm::float3(1.f / float(atlasRes.x), 1.f / float(atlasRes.y), 1.f / float(atlasRes.z));
            // The handles of the channel textures: each samples its atlas with its channel's sampler.
            for (size_t c = 0; c < m_channelTextures.size() && c < MAX_CHANNEL; ++c) {
                nvrhi::ITexture *tex = m_channelTextures[c].Get();
                if (!tex) continue;
                info.volIn[c] = m_device->getTextureHandle(tex, m_channelSamplers[c], nvrhi::AllSubresources,
                                                           nvrhi::Format::UNKNOWN);
                info.volOut[c] = m_device->getSurfaceHandle(tex, 0);
            }
        }
    }

    void invalidateVDBInfo() override { m_vdbInfoDirty = true; }

    void prepareVDB() override {
        if (!ensureVDBInfoBuffer()) return;
        if (!m_vdbInfoDirty) return;
        VDBInfo info;
        fillVDBInfo(info, true);
        m_context->getCommandList()->writeBuffer(m_vdbInfoGPU, &info, sizeof(info), 0);
        m_vdbInfoDirty = false;
    }

    void prepareVDBPartially() override {
        if (!ensureVDBInfoBuffer()) return;
        VDBInfo info;
        fillVDBInfo(info, false);
        m_context->getCommandList()->writeBuffer(m_vdbInfoGPU, &info, sizeof(info), 0);
        m_vdbInfoDirty = true;   // the full block (with atlas) still has to go up
    }

    nvrhi::IBuffer *getVDBInfoGPU() override {
        if (!m_vdbInfoGPU) prepareVDB();
        return m_vdbInfoGPU.Get();
    }

    IComputeKernel *getKernel(const char *name) override {
        if (!name || !*name) return nullptr;
        IComputeKernel *kernel = m_context->findKernel(s_shaderContainers, uint32_t(std::size(s_shaderContainers)), name);
        if (kernel && !m_layoutChecked) {
            m_layoutChecked = true;
            checkVDBInfoLayout(kernel);
        }
        return kernel;
    }

    // ---- statistics ----------------------------------------------------

    GVDBUsage getUsage() override {
        GVDBUsage u;
        dm::int3 ext = m_voxResMax;
        if (dm::all(ext == dm::int3::zero()) && !m_voxBounds.isempty()) ext = m_voxBounds.diagonal();
        u.extents = ext;
        u.numBricks = m_numActiveBricks;

        const uint64_t leafdim = getResDim(0);
        const double voxPerBrick = double(leafdim) * leafdim * leafdim;
        u.millionVoxels = double(u.numBricks) * voxPerBrick / 1.0e6;
        dm::int3 vb = ext / int(leafdim);
        const double coverBricks = double(vb.x) * vb.y * vb.z;
        u.occupancyPercent = coverBricks > 0 ? float(double(u.numBricks) * 100.0 / coverBricks) : 0.f;

        u.topologyMB = measurePoolsMB();
        u.atlasMB = float(atlasBytesTotal() / MB);
        u.gpuFreeMB = 0.f;    // the CUDA backend has no memory query
        u.gpuTotalMB = 0.f;
        return u;
    }

    void logMeasure() override {
        const int levs = int(m_levels.size());
        dm::int3 vmin = m_voxBounds.isempty() ? dm::int3::zero() : m_voxBounds.lower();
        dm::int3 vmax = m_voxBounds.isempty() ? dm::int3::zero() : m_voxBounds.upper();
        dm::int3 vres = vmax - vmin;

        log::info("  EXTENTS:");
        log::info("   Volume Res: %d x %d x %d", vres.x, vres.y, vres.z);
        log::info("   Volume Max: %d x %d x %d", m_voxResMax.x, m_voxResMax.y, m_voxResMax.z);
        log::info("   Bound Min:  %d x %d x %d", vmin.x, vmin.y, vmin.z);
        log::info("   Bound Max:  %d x %d x %d", vmax.x, vmax.y, vmax.z);

        std::string cfg;
        for (int n = levs - 1; n >= 0; --n) {
            cfg += std::to_string(m_levels[n].logDim);
            if (n) cfg += ", ";
        }
        log::info("  VDB CONFIG: <%s>:", cfg.c_str());
        log::info("             # Nodes / # Pool   Pool Size");
        uint64_t numTotal = 0, maxTotal = 0;
        for (int n = 0; n < levs; ++n) {
            uint64_t used = m_pool.poolGetNumUsed(0, uint8_t(n));
            uint64_t reserved = m_pool.poolGetNumReserved(0, uint8_t(n));
            numTotal += used;
            maxTotal += reserved;
            double sizeMB = double(m_pool.poolGetMemoryReservedWidth(0, uint8_t(n)) +
                                   m_pool.poolGetMemoryReservedWidth(1, uint8_t(n))) / MB;
            log::info("   Level %d: %8llu %8llu  %8.2f MB", n, (unsigned long long)used,
                      (unsigned long long)reserved, sizeMB);
        }
        log::info("   Percent Pool Used: %4.2f%%", maxTotal ? double(numTotal) * 100.0 / double(maxTotal) : 0.0);

        if (!m_channels.empty()) {
            const int leafdim = int(getResDim(0));
            dm::uint3 axiscnt = m_atlasGridDim;
            dm::uint3 axisres = getAtlasResDim();
            const double voxPerBrick = double(leafdim) * leafdim * leafdim;
            dm::int3 vb = m_voxResMax / leafdim;
            const double vbrk = double(vb.x) * vb.y * vb.z;
            const double sbrk = double(axiscnt.x) * axiscnt.y * axiscnt.z;
            const double abrk = double(m_numActiveBricks);
            log::info("  ATLAS STORAGE");
            log::info("   Atlas Res:     %u x %u x %u  LeafCnt: %u x %u x %u  LeafDim: %d^3", axisres.x, axisres.y,
                      axisres.z, axiscnt.x, axiscnt.y, axiscnt.z, leafdim);
            log::info("   Vol Extents:   %.0f bricks,  %5.2f million voxels", vbrk, vbrk * voxPerBrick / 1.0e6);
            log::info("   Atlas Storage: %.0f bricks,  %5.2f million voxels", sbrk, sbrk * voxPerBrick / 1.0e6);
            log::info("   Atlas Active:  %.0f bricks,  %5.2f million voxels", abrk, abrk * voxPerBrick / 1.0e6);
            log::info("   Occupancy:     %6.2f%%", vbrk > 0 ? abrk * 100.0 / vbrk : 0.0);
        }

        log::info("  MEMORY USAGE:");
        log::info("   Topology Total:    %6.2f MB", measurePoolsMB());
        log::info("   Atlas:");
        double volTotal = 0, volDense = 0;
        const double denseVoxels = double(m_voxResMax.x) * m_voxResMax.y * m_voxResMax.z;
        for (size_t c = 0; c < m_channels.size(); ++c) {
            uint32_t bpv = atlasFormatBytes(m_channels[c].format);
            double bytes = double(atlasBytes(int(c)));
            log::info("     Channel %d:       %6.2f MB (%u bytes/vox)", int(c), bytes / MB, bpv);
            volTotal += bytes;
            volDense += denseVoxels * bpv;
        }
        log::info("   Atlas Total:       %6.2f MB (Dense: %10.2f MB)", volTotal / MB, volDense / MB);
    }

 private:
    // ---- nodes -----------------------------------------------------------

    static void setupNode(Node *node, uint8_t lev, dm::int3 pos, bool active) {
        node->level = lev;
        node->flags = active ? 1 : 0;
        node->priority = 0;
        node->pad = 0;
        node->pos = pos;
        node->value = dm::int3(-1);
        node->range = dm::float3::zero();
        node->parent = NodeId::null();
        node->childList = NodeId::null();
        node->mask = 0;
    }

    Node *nodePtr(NodeId id) { return (Node *)m_pool.poolData(id); }

    static size_t extentIndex(const Extents &e, int x, int y, int z) {
        return size_t(((z - e.imin.z) * e.ires.y + (y - e.imin.y)) * e.ires.x + (x - e.imin.x));
    }

    // index-space corner of the node at `lev` that covers pos
    dm::int3 getCoveringNodePos(uint8_t lev, dm::int3 pos) const {
        const int range = int(getRange(int8_t(lev)));
        dm::int3 np = (pos / range) * range;
        if (pos.x < np.x) np.x -= range;
        if (pos.y < np.y) np.y -= range;
        if (pos.z < np.z) np.z -= range;
        return np;
    }

    uint32_t getBitPos(uint8_t lev, dm::int3 p) const {
        const uint32_t res = getResDim(lev);
        return (uint32_t(p.z) * res + uint32_t(p.y)) * res + uint32_t(p.x);
    }

    dm::int3 getPosFromBit(uint8_t lev, uint32_t bit) const {
        const uint32_t logDim = getLogDim(lev);
        const uint32_t mask = (1u << logDim) - 1;
        return dm::int3(int(bit & mask), int((bit >> logDim) & mask), int((bit >> (2 * logDim)) & mask));
    }

    // true when pos lies inside node; bit receives the child index covering pos
    bool getPosInNode(const Node *node, dm::int3 pos, uint32_t &bit) const {
        const int range = int(getRange(int8_t(node->level)));
        const dm::int3 p = pos - node->pos;
        if (p.x >= 0 && p.y >= 0 && p.z >= 0 && p.x < range && p.y < range && p.z < range) {
            const int childRange = int(getRange(int8_t(node->level - 1)));
            bit = getBitPos(node->level, p / childRange);
            return true;
        }
        bit = 0;
        return false;
    }

    NodeId getChild(Node *node, uint32_t b) {
        NodeId *clist = getChildList(node);
        return clist ? clist[b] : NodeId::null();
    }

    bool isOn(Node *node, uint32_t b) { return getChild(node, b) != NodeId::null(); }

    NodeId insertChild(NodeId parentId, NodeId childId, uint32_t b) {
        Node *parent = nodePtr(parentId);
        Node *child = nodePtr(childId);
        child->parent = parentId;
        if (parent->childList == NodeId::null()) {
            parent->childList = allocateChildList(parent->level);
            if (parent->childList == NodeId::null()) return NodeId::null();
        }
        NodeId *clist = (NodeId *)m_pool.poolData(parent->childList);
        clist[b] = childId;
        return childId;
    }

    NodeId addChildNode(NodeId parentId, uint32_t b) {
        Node *parent = nodePtr(parentId);
        const uint8_t clev = uint8_t(parent->level - 1);
        dm::int3 p = parent->pos + getPosFromBit(parent->level, b) * int(getRange(int8_t(clev)));
        NodeId childId = allocateNode(clev, p, true);
        if (childId == NodeId::null()) return childId;
        return insertChild(parentId, childId, b);
    }

    // Activates pos below nodeId down to stopLev (0 = brick). stopId: an existing
    // node to link into the path (reparenting). bNew: a brick was created.
    NodeId activateSpaceRec(NodeId nodeId, dm::int3 pos, bool &bNew, NodeId stopId, uint8_t stopLev) {
        if (m_root == NodeId::null() && nodeId == m_root) {
            // create a root at the stop level covering pos
            m_root = allocateNode(stopLev, getCoveringNodePos(stopLev, pos), true);
            if (m_root == NodeId::null()) return m_root;
            nodeId = m_root;
            if (stopLev == 0) bNew = true;
        }
        Node *curr = nodePtr(nodeId);
        if (!curr) return NodeId::null();

        uint32_t b;
        if (getPosInNode(curr, pos, b)) {
            if (stopId != NodeId::null()) {
                Node *stopn = nodePtr(stopId);
                if (dm::all(pos == stopn->pos) && !isOn(curr, b) && curr->level == stopn->level + 1)
                    return insertChild(nodeId, stopId, b);
            }
            if (curr->level == stopLev) return nodeId;

            NodeId childId;
            if (!isOn(curr, b)) {
                childId = addChildNode(nodeId, b);
                if (childId == NodeId::null()) return childId;
                if (curr->level == 1) bNew = true;
            } else {
                childId = getChild(curr, b);
            }
            if (childId.level() == 0) return childId;
            return activateSpaceRec(childId, pos, bNew, stopId, stopLev);
        }

        // pos is outside this node: climb, creating a new root when needed
        NodeId parent = curr->parent;
        if (parent == NodeId::null()) {
            parent = reparent(curr->level, nodeId, pos, bNew, stopLev);
            if (parent == NodeId::null()) return parent;
        }
        return activateSpaceRec(parent, pos, bNew, stopId, stopLev);
    }

    // Creates a new root covering both the previous root and pos, links the
    // previous root and activates pos. Returns the parent of the node at pos.
    NodeId reparent(uint8_t level, NodeId prevRootId, dm::int3 pos, bool &bNew, uint8_t stopLev) {
        const dm::int3 prevRootPos = nodePtr(prevRootId)->pos;
        const uint8_t numLevels = getNumLevels();
        bool cover = false;
        uint8_t lev = level;
        dm::int3 pos1 = dm::int3::zero();
        // the covering level must also reach the requested stop level
        while ((!cover || lev < stopLev) && lev + 1 < numLevels) {
            ++lev;
            pos1 = getCoveringNodePos(lev, pos);
            dm::int3 pos2 = getCoveringNodePos(lev, prevRootPos);
            cover = dm::all(pos1 == pos2);
        }
        if (!cover || lev < stopLev) return NodeId::null();   // level limit exceeded

        NodeId newRootId = allocateNode(lev, pos1, true);
        if (newRootId == NodeId::null()) return newRootId;
        m_root = newRootId;

        // link the previous root into the new root (no new bricks on this path)
        bool bn = false;
        activateSpaceRec(newRootId, prevRootPos, bn, prevRootId, 0);

        // activate pos below the new root
        NodeId nodeId = activateSpaceRec(newRootId, pos, bNew, NodeId::null(), stopLev);
        if (nodeId == NodeId::null()) return nodeId;
        NodeId parent = nodePtr(nodeId)->parent;
        return parent == NodeId::null() ? nodeId : parent;
    }

    bool isActiveRec(dm::int3 pos, NodeId nodeId) {
        Node *node = nodePtr(nodeId);
        if (!node) return false;
        uint32_t b;
        if (!getPosInNode(node, pos, b)) return false;
        if (node->level == 0) return true;
        NodeId child = getChild(node, b);
        if (child == NodeId::null()) return false;
        return isActiveRec(pos, child);
    }

    uint64_t countActiveBricks() {
        const uint64_t leafCnt = getNumNodes(0);
        uint64_t cnt = 0;
        for (uint64_t n = 0; n < leafCnt; ++n)
            if (((Node *)m_pool.poolData(NodeId{0, 0, n}))->flags) ++cnt;
        return cnt;
    }

    void computeVoxelBounds() {
        dm::box<int, 3> bounds = dm::box<int, 3>::empty();
        const dm::int3 range(int(getRange(0)));
        const uint64_t leafCnt = getNumNodes(0);
        for (uint64_t n = 0; n < leafCnt; ++n) {
            Node *node = (Node *)m_pool.poolData(NodeId{0, 0, n});
            if (!node->flags) continue;
            bounds.lower() = dm::min(bounds.lower(), node->pos);
            bounds.upper() = dm::max(bounds.upper(), node->pos + range);
        }
        m_voxBounds = bounds;
        if (!bounds.isempty()) m_voxResMax = dm::max(m_voxResMax, bounds.diagonal());
    }

    // ---- atlas -----------------------------------------------------------

    dm::int3 atlasBlockIndex(uint64_t slot) const {
        const uint64_t a1 = m_atlasGridDim.x;
        const uint64_t a2 = a1 * m_atlasGridDim.y;
        dm::int3 b;
        b.z = int(slot / a2);
        slot -= uint64_t(b.z) * a2;
        b.y = int(slot / a1);
        slot -= uint64_t(b.y) * a1;
        b.x = int(slot);
        return b;
    }

    void uploadAtlasMap() {
        const uint64_t bytes = uint64_t(m_atlasMap.size()) * sizeof(AtlasNode);
        if (bytes == 0) {
            m_context->releaseAfterUse(m_atlasMapGPU.Get());
            m_atlasMapGPU = nullptr;
            return;
        }
        m_atlasMapGPU = GVDBPrimitives::ensureBuffer(m_context, m_atlasMapGPU, bytes, "GVDB atlas map");
        if (m_atlasMapGPU) m_context->getCommandList()->writeBuffer(m_atlasMapGPU, m_atlasMap.data(), bytes, 0);
    }

    void ensureChannelTextures() {
        for (int c = 0; c < int(m_channels.size()); ++c) ensureChannelTexture(c);
    }

    // (Re)creates the atlas texture of a channel when it is missing, too small
    // or of another format; existing voxels are carried over.
    void ensureChannelTexture(int c) {
        if (m_levels.empty()) return;   // no brick size yet
        const dm::uint3 res = getAtlasResDim();
        const GVDBChannelDesc &channel = m_channels[c];
        if (channel.format == ATLAS_FORMAT_RGB32_FLOAT) {
            log::error("GVDB: channel %d: RGB32_FLOAT atlases are not supported (CUDA arrays have no 3-channel formats)", c);
            return;
        }

        if (!m_channelSamplers[c]) {
            if (NVRHI_FAILED(m_device->createSampler(channel.samplerDesc, &m_channelSamplers[c]))) {
                log::error("GVDB: cannot create the sampler of channel %d", c);
                return;
            }
        }

        nvrhi::TextureDesc desc;
        desc.dimension = nvrhi::TextureDimension::Texture3D;
        desc.width = res.x;
        desc.height = res.y;
        desc.depth = res.z;
        desc.mipLevels = 1;
        desc.format = atlasFormatToNVRHI(channel.format);
        desc.isUAV = true;   // the kernels write through surfaces
        desc.debugName = "GVDB atlas channel " + std::to_string(c);

        auto &tex = m_channelTextures[c];
        if (tex) {
            const nvrhi::TextureDesc &old = tex->getDesc();
            if (old.format == desc.format && old.width >= desc.width && old.height >= desc.height && old.depth >= desc.depth)
                return;
        }

        nvrhi::TextureHandle newTex;
        if (NVRHI_FAILED(m_device->createTexture(desc, &newTex)) || !newTex) {
            log::error("GVDB: cannot create the atlas of channel %d (%u x %u x %u)", c, desc.width, desc.height, desc.depth);
            return;
        }

        if (tex && tex->getDesc().format == desc.format) {
            // preserve the existing atlas content
            const nvrhi::TextureDesc &old = tex->getDesc();
            nvrhi::TextureSlice slice;
            slice.width = std::min(old.width, desc.width);
            slice.height = std::min(old.height, desc.height);
            slice.depth = std::min(old.depth, desc.depth);
            m_context->getCommandList()->copyTexture1(newTex, slice, tex, slice);
        }
        m_context->releaseAfterUse(tex.Get());   // kernels may still use its handles
        tex = newTex;
        invalidateVDBInfo();
    }

    uint64_t atlasBytes(int c) const {
        if (c < 0 || c >= int(m_channelTextures.size()) || !m_channelTextures[c]) return 0;
        const nvrhi::TextureDesc &d = m_channelTextures[c]->getDesc();
        return uint64_t(d.width) * d.height * d.depth * atlasFormatBytes(m_channels[c].format);
    }

    uint64_t atlasBytesTotal() const {
        uint64_t total = 0;
        for (int c = 0; c < int(m_channels.size()); ++c) total += atlasBytes(c);
        return total;
    }

    float measurePoolsMB() const {
        uint64_t total = 0;
        for (uint8_t lev = 0; lev < m_levels.size(); ++lev)
            total += m_pool.poolGetMemoryReservedWidth(0, lev) + m_pool.poolGetMemoryReservedWidth(1, lev);
        return float(double(total) / MB);
    }

    // ---- GPU mirror --------------------------------------------------------

    void commitPool(uint8_t grp, uint8_t lev, PoolGPU &gpu) {
        const uint64_t bytes = m_pool.poolGetMemoryUsedWidth(grp, lev);
        if (bytes == 0) return;   // nothing allocated: keep whatever mirror exists
        if (!gpu.buffer || gpu.capacity < bytes) {
            // size the mirror like the host reservation so that it grows in steps
            const uint64_t capacity = std::max(bytes, m_pool.poolGetMemoryReservedWidth(grp, lev));
            dropPool(gpu);
            gpu.buffer = GVDBPrimitives::ensureBuffer(m_context, gpu.buffer, capacity, grp == 0 ? "GVDB nodes" : "GVDB child lists");
            gpu.capacity = gpu.buffer ? capacity : 0;
        }
        if (gpu.buffer) m_context->getCommandList()->writeBuffer(gpu.buffer, m_pool.poolData(grp, lev), bytes, 0);
    }

    // Drops a pool mirror that recorded kernels may still read.
    void dropPool(PoolGPU &gpu) {
        if (m_context) m_context->releaseAfterUse(gpu.buffer.Get());
        gpu = {};
    }

    bool ensureVDBInfoBuffer() {
        if (m_vdbInfoGPU) return true;
        m_vdbInfoGPU = GVDBPrimitives::ensureBuffer(m_context, m_vdbInfoGPU, sizeof(VDBInfo), "GVDB VDBInfo");
        m_vdbInfoDirty = true;
        return m_vdbInfoGPU != nullptr;
    }

    // The kernels see VDBInfo as shaders/GVDB.slangh declares it: compares its fields with gvdb::VDBInfo
    // through the reflection of a kernel whose first argument is `VDBInfo* gvdb`.
    void checkVDBInfoLayout(IComputeKernel *kernel) {
        const nvrhi::ShaderLibraryReflection *reflection = kernel->getLibraryReflection();
        const nvrhi::ShaderParameterReflection *argument = kernel->getArgument(0);
        if (!reflection || !argument || argument->name != "gvdb" || argument->typeIndex >= reflection->types.size()) {
            m_layoutChecked = false;   // check with the next kernel
            return;
        }
        const uint32_t vdbInfo = reflection->types[argument->typeIndex].elementType;   // the pointee
        struct Field {
            const char *name;
            size_t offset;
        };
        static const Field s_fields[] = {
            {"nodelist", offsetof(VDBInfo, nodelist)},   {"childlist", offsetof(VDBInfo, childlist)},
            {"atlas_map", offsetof(VDBInfo, atlas_map)}, {"atlas_res", offsetof(VDBInfo, atlas_res)},
            {"top_lev", offsetof(VDBInfo, top_lev)},     {"bmax", offsetof(VDBInfo, bmax)},
            {"atlas_rcp", offsetof(VDBInfo, atlas_rcp)}, {"volIn", offsetof(VDBInfo, volIn)},
            {"volOut", offsetof(VDBInfo, volOut)},
        };
        for (const Field &f : s_fields) {
            const nvrhi::ShaderFieldReflection *field = nvrhi::findShaderField(reflection, vdbInfo, f.name);
            if (!field || field->offset != f.offset) {
                log::fatal("GVDB: VDBInfo.%s is at offset %d in the kernels, %zu in GVDB.h: the layouts differ",
                           f.name, field ? int(field->offset) : -1, f.offset);
                return;
            }
        }
    }

    // ---- data --------------------------------------------------------------

    nvrhi::AutoPtr<IComputeContext> m_context;
    nvrhi::AutoPtr<nvrhi::cuda::IDevice> m_device;
    GVDBPrimitives m_primitives;

    GVDBLevelConfig m_levelCfg;
    GVDBAtlasConfig m_atlasCfg;
    std::vector<LevelInfo> m_levels;
    GVDBAllocator m_pool;
    NodeId m_root = NodeId::null();

    std::vector<GVDBChannelDesc> m_channels;
    std::vector<nvrhi::TextureHandle> m_channelTextures;
    std::vector<nvrhi::SamplerHandle> m_channelSamplers;

    dm::uint3 m_atlasGridDim = {16u, 16u, 1u};   // configured x / y, z grows
    std::vector<AtlasNode> m_atlasMap;
    nvrhi::BufferHandle m_atlasMapGPU;

    PoolGPU m_nodePoolsGPU[MAXLEV];
    PoolGPU m_childPoolsGPU[MAXLEV];

    dm::box<int, 3> m_voxBounds = dm::box<int, 3>::empty();
    dm::int3 m_voxResMax = dm::int3::zero();   // largest extents seen (reference mVoxResMax)
    uint64_t m_numActiveBricks = 0;

    nvrhi::BufferHandle m_vdbInfoGPU;
    bool m_vdbInfoDirty = true;
    nvrhi::BufferHandle m_fetchBuffer;   // fetchPools() readback
    bool m_layoutChecked = false;
};

}  // namespace

nvrhi::FRESULT createGVDBVolume(IComputeContext *context, IGVDBVolume **volume) {
    if (!context || !volume) return nvrhi::FE_INVALID_ARGS;
    *volume = MAKE_RC_OBJ(GVDBVolume, context);
    return *volume ? nvrhi::FS_OK : nvrhi::FE_OUT_OF_MEMORY;
}

}  // namespace gvdb
