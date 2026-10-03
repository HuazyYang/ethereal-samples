// IGVDBSerializer: VBX file I/O (LoadVBX / SaveVBX of the reference).
//
// File layout (GVDB_FILESPEC):
//   header   major, minor (uchar); 1.11+: pretrans, angles, scale, trans (3 floats
//            each); num_grids (int); 2.x: use_bitmasks (uchar); grid offsets (uint64 each)
//   grid     name[256], dtype, components, compress (uchar), voxel size (3 floats),
//            leafcnt (int), leafdim (3 int), apron, num_chan (int), atlas_sz (uint64),
//            topotype (uchar), reuse (int), layout (uchar), axiscnt, axisres (3 int each)
//   topology levels (int), root (uint64); per level: logdim, res (int), range (3 int),
//            cnt0, width0, cnt1, width1 (int); then the node pools, then the child
//            list pools (cnt * width bytes each)
//   atlas    per channel: type, stride (int), then axisres.x*y*z*stride bytes of voxels
// Files of GVDB 1.0 (and 2.x with the flag) store bitmask nodes: node records are
// wider than Node (the mask follows the header, starting at Node::mask) and child
// lists are packed in bit order; they are converted to indexed child lists.
#include <gvdb/GVDB.h>
#include <donut/core/log.h>
#include <nvrhi/core/datablob.h>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>

namespace gvdb {

using namespace donut;

namespace {

constexpr uint8_t VBX_MAJOR = 1;
constexpr uint8_t VBX_MINOR = 11;
constexpr uint64_t SLAB_BYTES = 64ull << 20;   // atlas transfer granularity (pinned memory)
constexpr uint64_t VBX_ID_UNDEFL = 0xFFFFFFFFull;   // the reference's "undefined" node / list id in files

struct Reader {
    const uint8_t *p;
    const uint8_t *begin;
    const uint8_t *end;
    bool ok = true;

    Reader(const void *data, size_t size)
        : p((const uint8_t *)data), begin((const uint8_t *)data), end((const uint8_t *)data + size) {}

    size_t offset() const { return size_t(p - begin); }
    bool has(uint64_t n) const { return ok && uint64_t(end - p) >= n; }

    bool bytes(void *dst, uint64_t n) {
        if (!has(n)) {
            ok = false;
            return false;
        }
        if (dst) memcpy(dst, p, size_t(n));
        p += n;
        return true;
    }
    bool skip(uint64_t n) { return bytes(nullptr, n); }
    bool seek(uint64_t off) {
        if (!ok || off > uint64_t(end - begin)) {
            ok = false;
            return false;
        }
        p = begin + off;
        return true;
    }
    template <typename T>
    bool read(T &v) {
        return bytes(&v, sizeof(T));
    }
};

struct Writer {
    uint8_t *p;
    uint8_t *begin;
    uint8_t *end;

    Writer(void *data, size_t size) : p((uint8_t *)data), begin((uint8_t *)data), end((uint8_t *)data + size) {}

    size_t offset() const { return size_t(p - begin); }
    void bytes(const void *src, uint64_t n) {
        NVRHI_ASSERT(uint64_t(end - p) >= n);
        if (src) memcpy(p, src, size_t(n));
        else memset(p, 0, size_t(n));
        p += n;
    }
    template <typename T>
    void write(const T &v) {
        bytes(&v, sizeof(T));
    }
    template <typename T>
    void writeAt(size_t off, const T &v) {
        NVRHI_ASSERT(off + sizeof(T) <= size_t(end - begin));
        memcpy(begin + off, &v, sizeof(T));
    }
};

// ---- bitmask helpers (reference Node::countOn / countToIndex) ---------------

inline uint64_t popcount64(uint64_t v) {
    v = v - ((v >> 1) & 0x5555555555555555ull);
    v = (v & 0x3333333333333333ull) + ((v >> 2) & 0x3333333333333333ull);
    return (((v + (v >> 4)) & 0x0F0F0F0F0F0F0F0Full) * 0x0101010101010101ull) >> 56;
}

// number of set bits below bit b
inline uint64_t countOn(const uint64_t *mask, uint32_t b) {
    uint64_t sum = 0;
    const uint32_t words = b >> 6;
    for (uint32_t w = 0; w < words; ++w) sum += popcount64(mask[w]);
    if (b & 63) sum += popcount64(mask[words] & ((1ull << (b & 63)) - 1));
    return sum;
}

inline bool isBitOn(const uint64_t *mask, uint32_t b) { return (mask[b >> 6] & (1ull << (b & 63))) != 0; }

// bit index of the count-th set bit (count from 0), or numBits when there is none
inline uint32_t countToIndex(const uint64_t *mask, uint32_t numBits, uint64_t count) {
    uint64_t seen = 0;
    for (uint32_t b = 0; b < numBits; ++b) {
        if (!isBitOn(mask, b)) continue;
        if (seen == count) return b;
        ++seen;
    }
    return numBits;
}

class GVDBSerializer final : public nvrhi::ObjectImpl<IGVDBSerializer> {
 public:
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(GVDBSerializer)
    NVRHI_IMPLEMENTS_INTERFACE(IGVDBSerializer)
    NVRHI_END_INTERFACE_TABLE()

    explicit GVDBSerializer(IGVDBVolume *volume) : m_volume(volume) {}

    IGVDBVolume *getVolume() const override { return m_volume.Get(); }

    nvrhi::FRESULT loadVBX(nvrhi::IDataBlob *vbx, VBXTransform *outTransform) override {
        if (!vbx || !vbx->GetDataPtr()) return nvrhi::FE_INVALID_ARGS;
        IGVDBVolume *vol = m_volume.Get();
        Reader r(vbx->GetDataPtr(), vbx->GetSize());

        // ---- file header
        uint8_t major = 0, minor = 0;
        r.read(major);
        r.read(minor);
        VBXTransform xf;
        if ((major == 1 && minor >= 11) || major > 1) {
            r.read(xf.pretrans);
            r.read(xf.angles);
            r.read(xf.scale);
            r.read(xf.trans);
        }
        int32_t numGrids = 0;
        r.read(numGrids);
        uint8_t readMasks = 0;
        if (major >= 2) r.read(readMasks);
        else if (major == 1 && minor == 0) readMasks = 1;   // GVDB 1.0 always used bitmasks
        if (!r.ok || numGrids < 1) {
            log::error("GVDB: loadVBX(): not a VBX file (ver %d.%d, %d grids)", major, minor, numGrids);
            return nvrhi::FE_INVALID_ARGS;
        }
        std::vector<uint64_t> gridOffs(size_t(numGrids), 0);
        for (auto &o : gridOffs) r.read(o);
        if (!r.ok) return nvrhi::FE_INVALID_ARGS;
        if (numGrids > 1) log::warning("GVDB: loadVBX(): %d grids in the file, only the first one is loaded", numGrids);
        log::info("GVDB: loadVBX (ver %d.%d%s)", major, minor, readMasks ? ", bitmasks" : "");
        if (gridOffs[0] != 0 && gridOffs[0] != r.offset()) r.seek(gridOffs[0]);

        // ---- grid header
        char gridName[256];
        uint8_t gridDtype, gridComponents, gridCompress, gridTopotype, gridLayout;
        dm::float3 voxelSize;
        int32_t leafcnt, apron, numChan, gridReuse;
        dm::int3 leafdim, axiscnt, axisres;
        uint64_t atlasSz;
        r.bytes(gridName, sizeof(gridName));
        r.read(gridDtype);
        r.read(gridComponents);
        r.read(gridCompress);
        r.read(voxelSize);
        r.read(leafcnt);
        r.read(leafdim);
        r.read(apron);
        r.read(numChan);
        r.read(atlasSz);
        r.read(gridTopotype);
        r.read(gridReuse);
        r.read(gridLayout);
        r.read(axiscnt);
        r.read(axisres);

        // ---- topology section
        int32_t levels = 0;
        uint64_t root = ~0ull;
        int32_t ld[MAXLEV] = {}, res[MAXLEV] = {}, cnt0[MAXLEV] = {}, width0[MAXLEV] = {}, cnt1[MAXLEV] = {},
                width1[MAXLEV] = {};
        dm::int3 range[MAXLEV] = {};
        r.read(levels);
        r.read(root);
        if (!r.ok || levels < 1 || levels > MAXLEV) {
            log::error("GVDB: loadVBX(): invalid level count %d", levels);
            return nvrhi::FE_INVALID_ARGS;
        }
        for (int n = 0; n < levels; ++n) {
            r.read(ld[n]);
            r.read(res[n]);
            r.read(range[n]);
            r.read(cnt0[n]);
            r.read(width0[n]);
            r.read(cnt1[n]);
            r.read(width1[n]);
        }
        if (!r.ok) return nvrhi::FE_INVALID_ARGS;
        if (width0[0] < int(sizeof(Node))) {
            log::error("GVDB: loadVBX(): node size in file (%d) is incompatible with this library (%d)", width0[0],
                       int(sizeof(Node)));
            return nvrhi::FE_UNSUPPORTED;
        }
        for (int n = 0; n < levels; ++n) {
            if (cnt0[n] < 0 || cnt1[n] < 0 || width0[n] < int(sizeof(Node)) || (n > 0 && cnt1[n] > 0 && width1[n] <= 0)) {
                log::error("GVDB: loadVBX(): invalid pool description at level %d", n);
                return nvrhi::FE_INVALID_ARGS;
            }
        }

        // ---- configure the volume
        GVDBLevelConfig cfg;
        cfg.numLevels = uint32_t(levels);
        for (int n = 0; n < levels; ++n) {
            cfg.logDim[n] = uint32_t(std::max(ld[n], 1));
            cfg.initialNodes[n] = uint32_t(std::max(cnt0[n], 1));
        }
        vol->configure(cfg);
        GVDBAtlasConfig acfg;
        acfg.gridDim = dm::uint3(dm::max(axiscnt, dm::int3(1)));
        acfg.apron = uint32_t(std::max(apron, 0));
        vol->configureAtlas(acfg);
        if (int(vol->getBrickDim().x) != leafdim.x || int(vol->getResDim(0)) != res[0]) {
            log::error("GVDB: loadVBX(): brick dimension %d does not match the level configuration (%u)", leafdim.x,
                       vol->getBrickDim().x);
            return nvrhi::FE_INVALID_ARGS;
        }

        // ---- node pools (keep the file records: the masks live behind the header)
        const uint8_t *nodeData[MAXLEV] = {};
        for (int n = 0; n < levels; ++n) {
            const uint64_t bytes = uint64_t(cnt0[n]) * uint64_t(width0[n]);
            if (!r.has(bytes)) {
                log::error("GVDB: loadVBX(): truncated node pool at level %d", n);
                return nvrhi::FE_INVALID_ARGS;
            }
            nodeData[n] = r.p;
            for (int i = 0; i < cnt0[n]; ++i) {
                NodeId id = vol->allocateNode(uint8_t(n), dm::int3::zero(), true);
                if (id == NodeId::null() || id.index() != uint64_t(i)) return nvrhi::FE_GENERIC_ERROR;
                Node *node = vol->getNode(id);
                memcpy(node, nodeData[n] + uint64_t(i) * width0[n], sizeof(Node));
                // the reference writes its 32-bit "undefined" id (ID_UNDEFL) into these
                if (node->parent.raw() == VBX_ID_UNDEFL) node->parent = NodeId::null();
                if (node->childList.raw() == VBX_ID_UNDEFL) node->childList = NodeId::null();
            }
            r.skip(bytes);
        }
        // ---- child list pools
        for (int n = 0; n < levels; ++n) {
            const uint64_t bytes = uint64_t(cnt1[n]) * uint64_t(width1[n]);
            if (!r.has(bytes)) {
                log::error("GVDB: loadVBX(): truncated child list pool at level %d", n);
                return nvrhi::FE_INVALID_ARGS;
            }
            if (n > 0 && cnt1[n] > 0) {
                for (int i = 0; i < cnt1[n]; ++i) {
                    NodeId id = vol->allocateChildList(uint8_t(n));
                    if (id == NodeId::null() || id.index() != uint64_t(i)) return nvrhi::FE_GENERIC_ERROR;
                }
                GVDBPoolView view = vol->getChildListPool(uint8_t(n));
                const uint64_t copy = std::min<uint64_t>(view.stride, uint64_t(width1[n]));
                for (int i = 0; i < cnt1[n]; ++i)
                    memcpy((uint8_t *)view.data + uint64_t(i) * view.stride, r.p + uint64_t(i) * width1[n], size_t(copy));
            }
            r.skip(bytes);
        }
        vol->setRoot(NodeId{root});

        if (readMasks) convertBitmaskToNonBitmask(levels, cnt0, width0, nodeData);

        vol->finishTopology(true);

        // ---- atlas section
        vol->destroyChannels();
        vol->updateAtlas();
        if (numChan > 0) {
            dm::uint3 grid = vol->getAtlasGridDim();
            if (dm::any(grid != dm::uint3(axiscnt))) {
                log::error("GVDB: loadVBX(): atlas layout %u x %u x %u does not match the file (%d x %d x %d)", grid.x,
                           grid.y, grid.z, axiscnt.x, axiscnt.y, axiscnt.z);
                return nvrhi::FE_INVALID_ARGS;
            }
            dm::uint3 ares = vol->getAtlasResDim();
            if (dm::any(ares != dm::uint3(axisres))) {
                log::error("GVDB: loadVBX(): atlas resolution %u x %u x %u does not match the file (%d x %d x %d)",
                           ares.x, ares.y, ares.z, axisres.x, axisres.y, axisres.z);
                return nvrhi::FE_INVALID_ARGS;
            }
        }

        gp::IDevice *device = vol->getDevice();
        gp::IDeviceQueue *queue = vol->getQueue();
        for (int chan = 0; chan < numChan; ++chan) {
            int32_t chanType = 0, chanStride = 0;
            r.read(chanType);
            r.read(chanStride);
            if (!r.ok) return nvrhi::FE_INVALID_ARGS;

            GVDBChannelDesc desc;
            if (!atlasFormatFromVBXType(chanType, desc.format)) {
                log::error("GVDB: loadVBX(): unknown channel type %d", chanType);
                return nvrhi::FE_UNSUPPORTED;
            }
            if (int(atlasFormatBytes(desc.format)) != chanStride) {
                log::error("GVDB: loadVBX(): channel %d stride %d does not match type %d", chan, chanStride, chanType);
                return nvrhi::FE_INVALID_ARGS;
            }
            desc.samplerDesc.addressU = gp::SamplerAddressMode::Border;
            desc.samplerDesc.addressV = gp::SamplerAddressMode::Border;
            desc.samplerDesc.addressW = gp::SamplerAddressMode::Border;
            int c = vol->addChannel(desc);
            gp::ITexture *tex = c >= 0 ? vol->getChannelTexture(c) : nullptr;
            if (!tex) return nvrhi::FE_GENERIC_ERROR;

            // upload in z-slabs (each one is staged through pinned memory)
            const uint64_t sliceBytes = uint64_t(axisres.x) * axisres.y * chanStride;
            const uint64_t total = sliceBytes * axisres.z;
            if (!r.has(total)) {
                log::error("GVDB: loadVBX(): truncated atlas of channel %d", chan);
                return nvrhi::FE_INVALID_ARGS;
            }
            const uint32_t slabZ = uint32_t(std::max<uint64_t>(1, std::min<uint64_t>(axisres.z, SLAB_BYTES / sliceBytes)));
            for (uint32_t z0 = 0; z0 < uint32_t(axisres.z); z0 += slabZ) {
                const uint32_t dz = std::min(slabZ, uint32_t(axisres.z) - z0);
                gp::SubresourceFootprint fp;
                fp.format = atlasFormatToGP(desc.format);
                fp.width = uint32_t(axisres.x);
                fp.height = uint32_t(axisres.y);
                fp.depth = dz;
                fp.rowPitch = uint32_t(axisres.x) * uint32_t(chanStride);
                nvrhi::FRESULT fr = queue->writeTextureRegion(tex, 0, r.p + sliceBytes * z0, fp, 0, 0, z0, nullptr);
                if (NVRHI_FAILED(fr)) {
                    log::error("GVDB: loadVBX(): atlas upload failed (%d)", int(fr));
                    return fr;
                }
                device->commitQueue(queue);
                device->waitForQueue(queue);   // bounds the pinned staging memory
            }
            r.skip(total);
        }

        vol->invalidateVDBInfo();
        if (outTransform) *outTransform = xf;
        return nvrhi::FS_OK;
    }

    nvrhi::FRESULT saveVBX(nvrhi::IDataBlob **outVbx, const VBXTransform *transform) override {
        if (!outVbx) return nvrhi::FE_INVALID_ARGS;
        *outVbx = nullptr;
        IGVDBVolume *vol = m_volume.Get();
        const int levels = vol->getNumLevels();
        if (levels == 0) {
            log::error("GVDB: saveVBX(): the volume is not configured");
            return nvrhi::FE_INVALID_ARGS;
        }
        gp::IDevice *device = vol->getDevice();
        gp::IDeviceQueue *queue = vol->getQueue();

        const int numGrids = 1;
        const int numChan = vol->getNumChannels();
        const dm::uint3 axiscnt = vol->getAtlasGridDim();
        const dm::uint3 axisres = vol->getAtlasResDim();
        const uint64_t atlasVoxels = uint64_t(axisres.x) * axisres.y * axisres.z;

        // ---- size the blob
        uint64_t size = 2 + 4 * sizeof(dm::float3) + sizeof(int32_t) + numGrids * sizeof(uint64_t);
        size += 256 + 3 + sizeof(dm::float3) + sizeof(int32_t) + sizeof(dm::int3) + 2 * sizeof(int32_t) +
                sizeof(uint64_t) + 1 + sizeof(int32_t) + 1 + 2 * sizeof(dm::int3);
        size += sizeof(int32_t) + sizeof(uint64_t) + uint64_t(levels) * (2 * sizeof(int32_t) + sizeof(dm::int3) + 4 * sizeof(int32_t));
        for (int n = 0; n < levels; ++n) {
            GVDBPoolView p0 = vol->getNodePool(uint8_t(n));
            GVDBPoolView p1 = vol->getChildListPool(uint8_t(n));
            size += p0.count * p0.stride + p1.count * p1.stride;
        }
        for (int c = 0; c < numChan; ++c)
            size += 2 * sizeof(int32_t) + atlasVoxels * atlasFormatBytes(vol->getChannelDesc(c).format);

        nvrhi::AutoPtr<nvrhi::IDataBlob> blob;
        nvrhi::FRESULT fr = nvrhi::CreateBlob(size_t(size), &blob);
        if (NVRHI_FAILED(fr)) return fr;
        Writer w(blob->GetDataPtr(), blob->GetSize());

        // ---- file header (1.11: transform, no bitmasks)
        w.write(VBX_MAJOR);
        w.write(VBX_MINOR);
        VBXTransform xf;
        if (transform) xf = *transform;
        w.write(xf.pretrans);
        w.write(xf.angles);
        w.write(xf.scale);
        w.write(xf.trans);
        w.write(int32_t(numGrids));
        const size_t gridTable = w.offset();
        for (int g = 0; g < numGrids; ++g) w.write(uint64_t(0));   // patched below

        // ---- grid header
        w.writeAt(gridTable, uint64_t(w.offset()));
        w.bytes(nullptr, 256);                           // grid name
        w.write(uint8_t('f'));                           // data type
        w.write(uint8_t(1));                             // components
        w.write(uint8_t(0));                             // compression
        w.write(dm::float3(1.f));                        // voxel size (deprecated)
        w.write(int32_t(vol->getNumNodes(0)));           // brick count (allocated)
        w.write(dm::int3(vol->getBrickDim()));           // brick dimensions
        w.write(int32_t(vol->getAtlasConfig().apron));   // apron
        w.write(int32_t(numChan));
        uint64_t atlasSz = numChan ? atlasVoxels * atlasFormatBytes(vol->getChannelDesc(0).format) : 0;
        w.write(atlasSz);                                // atlas size of channel 0
        w.write(uint8_t(2));                             // topology type: gvdb
        w.write(int32_t(0));                             // topology reuse
        w.write(uint8_t(0));                             // layout: atlas
        w.write(dm::int3(axiscnt));
        w.write(dm::int3(axisres));

        // ---- topology section
        w.write(int32_t(levels));
        w.write(vol->getRootId().raw());
        for (int n = 0; n < levels; ++n) {
            GVDBPoolView p0 = vol->getNodePool(uint8_t(n));
            GVDBPoolView p1 = vol->getChildListPool(uint8_t(n));
            w.write(int32_t(vol->getLogDim(uint8_t(n))));
            w.write(int32_t(vol->getResDim(uint8_t(n))));
            w.write(dm::int3(int(vol->getRange(int8_t(n)))));
            w.write(int32_t(p0.count));
            w.write(int32_t(p0.stride));
            w.write(int32_t(p1.count));
            w.write(int32_t(p1.stride));
        }
        for (int n = 0; n < levels; ++n) {
            GVDBPoolView p0 = vol->getNodePool(uint8_t(n));
            // null parent / child list go out as the reference's ID_UNDEFL so that
            // the reference library can read the file
            for (uint64_t i = 0; i < p0.count; ++i) {
                Node node;
                memcpy(&node, (const uint8_t *)p0.data + i * p0.stride, sizeof(Node));
                if (node.parent == NodeId::null()) node.parent = NodeId{VBX_ID_UNDEFL};
                if (node.childList == NodeId::null()) node.childList = NodeId{VBX_ID_UNDEFL};
                w.bytes(&node, sizeof(Node));
                if (p0.stride > sizeof(Node)) w.bytes((const uint8_t *)p0.data + i * p0.stride + sizeof(Node), p0.stride - sizeof(Node));
            }
        }
        for (int n = 0; n < levels; ++n) {
            GVDBPoolView p1 = vol->getChildListPool(uint8_t(n));
            w.bytes(p1.data, p1.count * p1.stride);
        }

        // ---- atlas section: read the channel textures back in z-slabs
        for (int c = 0; c < numChan; ++c) {
            const AtlasFormat format = vol->getChannelDesc(c).format;
            const uint32_t stride = atlasFormatBytes(format);
            w.write(int32_t(atlasFormatToVBXType(format)));
            w.write(int32_t(stride));

            gp::ITexture *tex = vol->getChannelTexture(c);
            const uint64_t sliceBytes = uint64_t(axisres.x) * axisres.y * stride;
            if (!tex) {
                log::warning("GVDB: saveVBX(): channel %d has no atlas texture, writing zeros", c);
                w.bytes(nullptr, sliceBytes * axisres.z);
                continue;
            }
            const uint32_t slabZ = uint32_t(std::max<uint64_t>(1, std::min<uint64_t>(axisres.z, SLAB_BYTES / sliceBytes)));
            for (uint32_t z0 = 0; z0 < axisres.z; z0 += slabZ) {
                const uint32_t dz = std::min(slabZ, axisres.z - z0);
                const uint64_t bytes = sliceBytes * dz;

                gp::BufferDesc bdesc;
                bdesc.byteSize = bytes;
                bdesc.isStaging = true;
                nvrhi::AutoPtr<gp::IBuffer> staging;
                if (NVRHI_FAILED(fr = device->createBuffer(bdesc, &staging))) return fr;

                gp::TextureCopyLocation dst = {}, src = {};
                dst.resource = staging.Get();
                dst.type = gp::TextureCopyType::PlacedFootprint;
                dst.placeFootprint.offset = 0;
                dst.placeFootprint.format = atlasFormatToGP(format);
                dst.placeFootprint.width = axisres.x;
                dst.placeFootprint.height = axisres.y;
                dst.placeFootprint.depth = dz;
                dst.placeFootprint.rowPitch = axisres.x * stride;
                src.resource = tex;
                src.type = gp::TextureCopyType::SubresourceIndex;
                src.subresourceIndex = 0;
                gp::GPBox box = {0, 0, z0, axisres.x, axisres.y, z0 + dz};
                if (NVRHI_FAILED(fr = queue->copyTextureRegion(dst, 0, 0, 0, src, &box))) {
                    log::error("GVDB: saveVBX(): atlas readback failed (%d)", int(fr));
                    return fr;
                }
                device->commitQueue(queue);
                if (NVRHI_FAILED(fr = device->waitForQueue(queue))) return fr;

                void *mapped = nullptr;
                if (NVRHI_FAILED(fr = device->mapBuffer(staging, &mapped))) return fr;
                w.bytes(mapped, bytes);
                device->unmapBuffer(staging);
            }
        }
        NVRHI_ASSERT(w.offset() == blob->GetSize());

        *outVbx = blob.Detach();
        return nvrhi::FS_OK;
    }

 private:
    // Bitmask files: all bricks become active (they carried no flags) and the
    // packed child lists are spread to their bit positions.
    void convertBitmaskToNonBitmask(int levels, const int32_t *cnt0, const int32_t *width0,
                                    const uint8_t *const *nodeData) {
        IGVDBVolume *vol = m_volume.Get();
        for (int n = 0; n < cnt0[0]; ++n) vol->getNode(0, uint64_t(n))->flags = 1;

        for (int lv = 1; lv < levels; ++lv) {
            const uint32_t numBits = uint32_t(vol->getVoxelCnt(uint8_t(lv)));
            const uint64_t cmax = numBits;
            // mask words available in a record (the header holds the first one)
            const uint32_t availBits = uint32_t((uint64_t(width0[lv]) - offsetof(Node, mask)) * 8);
            if (availBits < numBits) {
                log::error("GVDB: loadVBX(): bitmask of level %d is shorter than its child count", lv);
                continue;
            }
            for (int n = 0; n < cnt0[lv]; ++n) {
                Node *node = vol->getNode(uint8_t(lv), uint64_t(n));
                NodeId *clist = vol->getChildList(node);
                if (!clist) {
                    log::error("GVDB: loadVBX(): bitmask node %d of level %d has no child list", n, lv);
                    continue;
                }
                const uint64_t *mask = (const uint64_t *)(nodeData[lv] + uint64_t(n) * width0[lv] + offsetof(Node, mask));
                const uint64_t ccnt = countOn(mask, numBits);
                if (ccnt > cmax) continue;

                // pad the remainder with null, then move the packed children to their bit index
                for (uint64_t j = ccnt; j < cmax; ++j) clist[j] = NodeId::null();
                for (int64_t j = int64_t(ccnt) - 1; j >= 0; --j) {
                    const uint32_t ndx = countToIndex(mask, numBits, uint64_t(j));
                    if (ndx >= numBits || countOn(mask, ndx) != uint64_t(j)) {
                        log::error("GVDB: loadVBX(): bitmask of node %d, level %d is inconsistent", n, lv);
                        break;
                    }
                    if (ndx == uint32_t(j)) continue;   // already in place
                    clist[ndx] = clist[j];
                    clist[j] = NodeId::null();
                }
            }
        }
        // the mask word of the header is meaningless for indexed child lists
        for (int lv = 0; lv < levels; ++lv)
            for (int n = 0; n < cnt0[lv]; ++n) vol->getNode(uint8_t(lv), uint64_t(n))->mask = 0;
    }

    nvrhi::AutoPtr<IGVDBVolume> m_volume;
};

}  // namespace

nvrhi::FRESULT createGVDBSerializer(IGVDBVolume *volume, IGVDBSerializer **serializer) {
    if (!volume || !serializer) return nvrhi::FE_INVALID_ARGS;
    *serializer = MAKE_RC_OBJ(GVDBSerializer, volume);
    return *serializer ? nvrhi::FS_OK : nvrhi::FE_OUT_OF_MEMORY;
}

}  // namespace gvdb
