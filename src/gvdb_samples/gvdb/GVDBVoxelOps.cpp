// IGVDBVoxelOps: operators on the channel data of one IGVDBVolume (port of
// VolumeGVDB::FillChannel / ClearChannel / Compute / ComputeKernel /
// UpdateApron / Resample / DownsampleCPU / Reduction and Allocator::AtlasFill).
// Only the public IGVDBVolume interface is used; the kernels come from the
// GVDBOperators PTX module through IGVDBVolume::getKernel().
#include <gvdb/GVDB.h>
#include <donut/core/log.h>
#include <iterator>
#include <cstring>

namespace gvdb {

using namespace donut;

#define GVDB_V_GP(expr)                                                        \
    do {                                                                       \
        auto rc_ = (expr);                                                     \
        if (NVRHI_FAILED(rc_)) {                                               \
            donut::log::error("GVDBVoxelOps: %s failed with error %d", #expr, \
                              (int)rc_);                                       \
            NVRHI_ASSERT(0);                                                   \
        }                                                                      \
    } while (0)

namespace {

constexpr int OP_BLOCK = 8;   // operator kernels run with 8x8x8 blocks

inline int divCeil(int a, int b) { return (a + b - 1) / b; }

inline dm::int3 toInt3(dm::uint3 v) { return dm::int3{int(v.x), int(v.y), int(v.z)}; }

// Name of the operator kernel of a ComputeOp (all have the signature
// (VDBInfo*, int3 atlasRes, uchar channel, float p1, float p2, float p3)).
const char *computeOpKernelName(ComputeOp op) {
    switch (op) {
        case ComputeOp::Smooth:    return "gvdbOpSmooth";
        case ComputeOp::Noise:     return "gvdbOpNoise";
        case ComputeOp::Grow:      return "gvdbOpGrow";
        case ComputeOp::Cut:       return "gvdbOpCut";
        case ComputeOp::ClrExpand: return "gvdbOpClrExpand";
        case ComputeOp::ExpandC:   return "gvdbOpExpandC";
    }
    return nullptr;
}

// Kernels per atlas format; nullptr where the format has no kernel (RGB32_FLOAT:
// CUDA surfaces cannot store / write float3).
const char *fillKernelName(AtlasFormat format) {
    switch (format) {
        case ATLAS_FORMAT_R8_UINT:      return "gvdbOpFillC";
        case ATLAS_FORMAT_RGBA8_UINT:   return "gvdbOpFillC4";
        case ATLAS_FORMAT_R32_FLOAT:    return "gvdbOpFillF";
        case ATLAS_FORMAT_RGBA32_FLOAT: return "gvdbOpFillF4";
        case ATLAS_FORMAT_RGB32_FLOAT:  return nullptr;
    }
    return nullptr;
}

const char *clearKernelName(AtlasFormat format) {
    switch (format) {
        case ATLAS_FORMAT_R8_UINT:      return "gvdbClearAtlasC";
        case ATLAS_FORMAT_RGBA8_UINT:   return "gvdbClearAtlasC4";
        case ATLAS_FORMAT_R32_FLOAT:    return "gvdbClearAtlasF";
        case ATLAS_FORMAT_RGBA32_FLOAT: return "gvdbClearAtlasF4";
        case ATLAS_FORMAT_RGB32_FLOAT:  return nullptr;
    }
    return nullptr;
}

const char *apronKernelName(AtlasFormat format) {
    switch (format) {
        case ATLAS_FORMAT_R8_UINT:      return "gvdbUpdateApronC";
        case ATLAS_FORMAT_RGBA8_UINT:   return "gvdbUpdateApronC4";
        case ATLAS_FORMAT_R32_FLOAT:    return "gvdbUpdateApronF";
        case ATLAS_FORMAT_RGBA32_FLOAT: return "gvdbUpdateApronF4";
        case ATLAS_FORMAT_RGB32_FLOAT:  return nullptr;
    }
    return nullptr;
}

}  // namespace

class GVDBVoxelOps : public nvrhi::ObjectImpl<IGVDBVoxelOps> {
 public:
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(GVDBVoxelOps)
    NVRHI_IMPLEMENTS_INTERFACE(IGVDBVoxelOps)
    NVRHI_END_INTERFACE_TABLE()

    explicit GVDBVoxelOps(IGVDBVolume *volume) : m_volume{volume} {}

    IGVDBVolume *getVolume() const override { return m_volume.Get(); }

    void fillChannel(int channel, dm::float4 value) override;
    void clearChannel(int channel) override;
    void clearAllChannels() override;
    void compute(ComputeOp op, int channel, int numIterations, dm::float3 params, bool updateApron,
                 bool skipOverAprons, float boundValue) override;
    void computeKernel(gp::IKernel *kernel, int channel, bool updateApron, bool skipOverAprons) override;
    void updateApron() override;
    void updateApron(int channel, float boundValue) override;
    void resample(int channel, const dm::float4x4 &xform, dm::int3 srcRes, gp::IBuffer *src, dm::float3 inRange,
                  dm::float3 outRange) override;
    void downsample(const dm::float4x4 &xform, dm::int3 srcRes, gp::IBuffer *src, dm::int3 dstRes,
                    dm::float3 dstMax, dm::float3 inRange, dm::float3 outRange,
                    std::vector<float> &outValues) override;
    float reduction(int channel) override;

 private:
    gp::IDevice *device() const { return m_volume->getDevice(); }
    gp::IDeviceQueue *queue() const { return m_volume->getQueue(); }

    // A channel index that has an atlas texture (false logs a warning).
    bool checkChannel(int channel, const char *what) const;
    // (Re)allocates a scratch buffer when it is too small.
    void ensureBuffer(nvrhi::AutoPtr<gp::IBuffer> &buffer, size_t bytes, bool staging);
    // Uploads the 16 floats of xform (dm::float4x4 memory layout, row-vector
    // convention: the kernels compute p.x*m[0] + p.y*m[4] + p.z*m[8] + m[12]).
    void uploadMatrix(const dm::float4x4 &xform);
    // Copies a device buffer to the host through a staging buffer (waits for the queue).
    void readBack(gp::IBuffer *src, size_t bytes, nvrhi::AutoPtr<gp::IBuffer> &staging, void *dst);
    // Launches an operator kernel `k(VDBInfo*, int3 atlasRes, uchar channel, float, float, float)`
    // over the atlas (8x8x8 blocks; the packed resolution when skipOverAprons).
    void launchOperator(gp::IKernel *kernel, int channel, dm::float3 params, bool skipOverAprons,
                        bool withParams);

    nvrhi::AutoPtr<IGVDBVolume> m_volume;
    nvrhi::AutoPtr<gp::IBuffer> m_matrixBuffer;        // 16 floats
    nvrhi::AutoPtr<gp::IBuffer> m_reductionBuffer;     // packres.x * packres.y floats
    nvrhi::AutoPtr<gp::IBuffer> m_reductionStaging;
    nvrhi::AutoPtr<gp::IBuffer> m_downsampleBuffer;    // dstRes.x * y * z floats
    nvrhi::AutoPtr<gp::IBuffer> m_downsampleStaging;
};

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

bool GVDBVoxelOps::checkChannel(int channel, const char *what) const {
    if (channel < 0 || channel >= m_volume->getNumChannels()) {
        log::warning("GVDBVoxelOps::%s: channel %d does not exist", what, channel);
        return false;
    }
    if (m_volume->getChannelTexture(channel) == nullptr) {
        log::warning("GVDBVoxelOps::%s: channel %d has no atlas yet (call updateAtlas() first)", what, channel);
        return false;
    }
    return true;
}

void GVDBVoxelOps::ensureBuffer(nvrhi::AutoPtr<gp::IBuffer> &buffer, size_t bytes, bool staging) {
    if (bytes == 0) bytes = sizeof(float);
    if (buffer && buffer->getDesc()->byteSize >= bytes) return;
    buffer = nullptr;
    gp::BufferDesc desc;
    desc.byteSize = bytes;
    desc.isStaging = staging;
    GVDB_V_GP(device()->createBuffer(desc, &buffer));
}

void GVDBVoxelOps::uploadMatrix(const dm::float4x4 &xform) {
    static_assert(sizeof(dm::float4x4) == 16 * sizeof(float), "float4x4 must be 16 contiguous floats");
    ensureBuffer(m_matrixBuffer, 16 * sizeof(float), false);
    GVDB_V_GP(queue()->writeBuffer(m_matrixBuffer, &xform, 16 * sizeof(float), 0));
}

void GVDBVoxelOps::readBack(gp::IBuffer *src, size_t bytes, nvrhi::AutoPtr<gp::IBuffer> &staging, void *dst) {
    ensureBuffer(staging, bytes, true);
    GVDB_V_GP(queue()->copyBufferRegion(staging, 0, src, 0, bytes));
    device()->commitQueue(queue());
    GVDB_V_GP(device()->waitForQueue(queue()));
    void *mapped = nullptr;
    GVDB_V_GP(device()->mapBuffer(staging, &mapped));
    if (mapped) memcpy(dst, mapped, bytes);
    device()->unmapBuffer(staging);
}

void GVDBVoxelOps::launchOperator(gp::IKernel *kernel, int channel, dm::float3 params, bool skipOverAprons,
                                  bool withParams) {
    if (kernel == nullptr) return;
    m_volume->prepareVDB();

    const dm::int3 atlasRes = toInt3(m_volume->getAtlasResDim());
    const dm::int3 threads = skipOverAprons ? toInt3(m_volume->getAtlasPackedResDim()) : atlasRes;
    if (threads.x <= 0 || threads.y <= 0 || threads.z <= 0) return;

    const gp::dim3 block{OP_BLOCK, OP_BLOCK, OP_BLOCK};
    const gp::dim3 grid{divCeil(threads.x, block.x), divCeil(threads.y, block.y), divCeil(threads.z, block.z)};

    const uchar chan = (uchar)channel;
    gp::KernelArg args[] = {
        gp::KernelArg::Buffer(m_volume->getVDBInfoGPU()),
        gp::KernelArg::Scalar(atlasRes),
        gp::KernelArg::Scalar(chan),
        gp::KernelArg::Scalar(params.x),
        gp::KernelArg::Scalar(params.y),
        gp::KernelArg::Scalar(params.z),
    };
    GVDB_V_GP(queue()->launch(kernel, grid, block, args, withParams ? std::size(args) : 3));
}

// ---------------------------------------------------------------------------
// fill / clear
// ---------------------------------------------------------------------------

void GVDBVoxelOps::fillChannel(int channel, dm::float4 value) {
    if (value.x == 0.f && value.y == 0.f && value.z == 0.f && value.w == 0.f) {
        clearChannel(channel);
        return;
    }
    if (!checkChannel(channel, "fillChannel")) return;

    const AtlasFormat format = m_volume->getChannelDesc(channel).format;
    const char *name = fillKernelName(format);
    if (name == nullptr) {
        log::warning("GVDBVoxelOps::fillChannel: channel %d has format RGB32_FLOAT, which has no fill kernel "
                     "(no surface writes for float3)", channel);
        return;
    }
    launchOperator(m_volume->getKernel(name), channel, dm::float3{value.x, value.y, value.z}, false, true);
}

void GVDBVoxelOps::clearChannel(int channel) {
    if (!checkChannel(channel, "clearChannel")) return;

    const AtlasFormat format = m_volume->getChannelDesc(channel).format;
    const char *name = clearKernelName(format);
    if (name == nullptr) {
        log::warning("GVDBVoxelOps::clearChannel: channel %d has format RGB32_FLOAT, which has no clear kernel "
                     "(no surface writes for float3)", channel);
        return;
    }
    // gvdbClearAtlas*(VDBInfo*, int3 atlasRes, uchar channel) over the whole atlas.
    launchOperator(m_volume->getKernel(name), channel, dm::float3::zero(), false, false);
}

void GVDBVoxelOps::clearAllChannels() {
    const int n = m_volume->getNumChannels();
    for (int i = 0; i < n; ++i) clearChannel(i);
}

// ---------------------------------------------------------------------------
// compute
// ---------------------------------------------------------------------------

void GVDBVoxelOps::compute(ComputeOp op, int channel, int numIterations, dm::float3 params, bool bUpdateApron,
                           bool skipOverAprons, float boundValue) {
    if (!checkChannel(channel, "compute")) return;
    const char *name = computeOpKernelName(op);
    gp::IKernel *kernel = name ? m_volume->getKernel(name) : nullptr;
    if (kernel == nullptr) {
        log::warning("GVDBVoxelOps::compute: operator kernel %s not found", name ? name : "(unknown op)");
        return;
    }

    for (int n = 0; n < numIterations; ++n) {
        launchOperator(kernel, channel, params, skipOverAprons, true);
        if (bUpdateApron) updateApron(channel, boundValue);
    }
}

void GVDBVoxelOps::computeKernel(gp::IKernel *kernel, int channel, bool bUpdateApron, bool skipOverAprons) {
    if (kernel == nullptr) {
        log::warning("GVDBVoxelOps::computeKernel: no kernel");
        return;
    }
    if (!checkChannel(channel, "computeKernel")) return;

    // k(VDBInfo*, int3 atlasRes, uchar channel): the first three operator arguments.
    launchOperator(kernel, channel, dm::float3::zero(), skipOverAprons, false);

    // As the reference: a user kernel may touch any channel, so all aprons are updated.
    if (bUpdateApron) updateApron();
}

// ---------------------------------------------------------------------------
// apron
// ---------------------------------------------------------------------------

void GVDBVoxelOps::updateApron() {
    const int n = m_volume->getNumChannels();
    for (int i = 0; i < n; ++i) updateApron(i, m_volume->getChannelDesc(i).voidValue.x);
}

void GVDBVoxelOps::updateApron(int channel, float boundValue) {
    const int apron = (int)m_volume->getAtlasConfig().apron;
    if (apron == 0) return;
    if (!checkChannel(channel, "updateApron")) return;

    const AtlasFormat format = m_volume->getChannelDesc(channel).format;
    const char *name = apronKernelName(format);
    if (name == nullptr) {
        log::warning("GVDBVoxelOps::updateApron: channel %d has format RGB32_FLOAT, which has no apron kernel "
                     "(no surface writes for float3)", channel);
        return;
    }
    gp::IKernel *kernel = m_volume->getKernel(name);
    if (kernel == nullptr) return;

    // Dimensions
    const int bricks = (int)m_volume->getNumNodes(0);          // all level-0 nodes (the kernel skips inactive ones)
    const int brickres = (int)m_volume->getBrickDimWithApron().x;  // brick side including the apron
    const int brickwid = (int)m_volume->getBrickDim().x;           // brick side without the apron
    if (bricks == 0) return;

    m_volume->prepareVDB();

    // One block column per brick; each yz plane of a block fills one of the 6 faces.
    // Note: the kernels assume an apron of 1.
    const gp::dim3 block{6 * apron, 8, 8};
    const gp::dim3 grid{bricks, divCeil(brickres, block.y), divCeil(brickres, block.z)};

    const uchar chan = (uchar)channel;
    gp::KernelArg args[] = {
        gp::KernelArg::Buffer(m_volume->getVDBInfoGPU()),
        gp::KernelArg::Scalar(chan),
        gp::KernelArg::Scalar(bricks),
        gp::KernelArg::Scalar(brickres),
        gp::KernelArg::Scalar(brickwid),
        gp::KernelArg::Scalar(boundValue),
    };
    GVDB_V_GP(queue()->launch(kernel, grid, block, args, std::size(args)));
}

// ---------------------------------------------------------------------------
// resample / downsample / reduction
// ---------------------------------------------------------------------------

void GVDBVoxelOps::resample(int channel, const dm::float4x4 &xform, dm::int3 srcRes, gp::IBuffer *src,
                            dm::float3 inRange, dm::float3 outRange) {
    if (src == nullptr || srcRes.x <= 0 || srcRes.y <= 0 || srcRes.z <= 0) {
        log::warning("GVDBVoxelOps::resample: no source volume");
        return;
    }
    if (!checkChannel(channel, "resample")) return;
    if (m_volume->getChannelDesc(channel).format != ATLAS_FORMAT_R32_FLOAT) {
        log::warning("GVDBVoxelOps::resample: channel %d is not a float channel", channel);
        return;
    }
    gp::IKernel *kernel = m_volume->getKernel("gvdbResample");
    if (kernel == nullptr) return;

    m_volume->prepareVDB();
    uploadMatrix(xform);

    // Grid over the whole atlas (GVDB_VOXUNPACKED)
    const dm::int3 res = toInt3(m_volume->getAtlasResDim());
    if (res.x <= 0 || res.y <= 0 || res.z <= 0) return;
    const gp::dim3 block{OP_BLOCK, OP_BLOCK, OP_BLOCK};
    const gp::dim3 grid{divCeil(res.x, block.x), divCeil(res.y, block.y), divCeil(res.z, block.z)};

    const uchar chan = (uchar)channel;
    gp::KernelArg args[] = {
        gp::KernelArg::Buffer(m_volume->getVDBInfoGPU()),
        gp::KernelArg::Scalar(res),
        gp::KernelArg::Scalar(chan),
        gp::KernelArg::Scalar(srcRes),
        gp::KernelArg::Buffer(src),
        gp::KernelArg::Buffer(m_matrixBuffer),
        gp::KernelArg::Scalar(inRange),
        gp::KernelArg::Scalar(outRange),
    };
    GVDB_V_GP(queue()->launch(kernel, grid, block, args, std::size(args)));
}

void GVDBVoxelOps::downsample(const dm::float4x4 &xform, dm::int3 srcRes, gp::IBuffer *src, dm::int3 dstRes,
                              dm::float3 dstMax, dm::float3 inRange, dm::float3 outRange,
                              std::vector<float> &outValues) {
    outValues.clear();
    if (src == nullptr || srcRes.x <= 0 || srcRes.y <= 0 || srcRes.z <= 0) {
        log::warning("GVDBVoxelOps::downsample: no source volume");
        return;
    }
    if (dstRes.x <= 0 || dstRes.y <= 0 || dstRes.z <= 0) return;
    gp::IKernel *kernel = m_volume->getKernel("gvdbDownsample");
    if (kernel == nullptr) return;

    const size_t count = size_t(dstRes.x) * size_t(dstRes.y) * size_t(dstRes.z);
    const size_t bytes = count * sizeof(float);
    ensureBuffer(m_downsampleBuffer, bytes, false);
    uploadMatrix(xform);

    // Grid as the reference: (res / 8) + 1 blocks per axis
    const gp::dim3 block{OP_BLOCK, OP_BLOCK, OP_BLOCK};
    const gp::dim3 grid{dstRes.x / block.x + 1, dstRes.y / block.y + 1, dstRes.z / block.z + 1};

    gp::KernelArg args[] = {
        gp::KernelArg::Scalar(srcRes),
        gp::KernelArg::Buffer(src),
        gp::KernelArg::Scalar(dstRes),
        gp::KernelArg::Scalar(dstMax),
        gp::KernelArg::Buffer(m_downsampleBuffer),
        gp::KernelArg::Buffer(m_matrixBuffer),
        gp::KernelArg::Scalar(inRange),
        gp::KernelArg::Scalar(outRange),
    };
    GVDB_V_GP(queue()->launch(kernel, grid, block, args, std::size(args)));

    // Retrieve data back to the host
    outValues.resize(count);
    readBack(m_downsampleBuffer, bytes, m_downsampleStaging, outValues.data());
}

float GVDBVoxelOps::reduction(int channel) {
    if (!checkChannel(channel, "reduction")) return 0.f;
    if (m_volume->getChannelDesc(channel).format != ATLAS_FORMAT_R32_FLOAT) {
        log::warning("GVDBVoxelOps::reduction: channel %d is not a float channel", channel);
        return 0.f;
    }
    gp::IKernel *kernel = m_volume->getKernel("gvdbReduction");
    if (kernel == nullptr) return 0.f;

    m_volume->prepareVDB();

    // 2D grid over the x/y plane of the packed atlas (apron voxels excluded);
    // the kernel integrates along z.
    const dm::int3 packres = toInt3(m_volume->getAtlasPackedResDim());
    const dm::int3 res = toInt3(m_volume->getAtlasResDim());
    if (packres.x <= 0 || packres.y <= 0 || packres.z <= 0) return 0.f;
    const gp::dim3 block{OP_BLOCK, OP_BLOCK, 1};
    const gp::dim3 grid{packres.x / block.x + 1, packres.y / block.y + 1, 1};

    const size_t count = size_t(packres.x) * size_t(packres.y);
    const size_t bytes = count * sizeof(float);
    ensureBuffer(m_reductionBuffer, bytes, false);

    const uchar chan = (uchar)channel;
    gp::KernelArg args[] = {
        gp::KernelArg::Buffer(m_volume->getVDBInfoGPU()),
        gp::KernelArg::Scalar(res),
        gp::KernelArg::Scalar(chan),
        gp::KernelArg::Scalar(packres),
        gp::KernelArg::Buffer(m_reductionBuffer),
    };
    GVDB_V_GP(queue()->launch(kernel, grid, block, args, std::size(args)));

    std::vector<float> sums(count);
    readBack(m_reductionBuffer, bytes, m_reductionStaging, sums.data());

    double sum = 0.0;
    for (float v : sums) sum += v;
    return (float)sum;
}

// ---------------------------------------------------------------------------
// factory
// ---------------------------------------------------------------------------

nvrhi::FRESULT createGVDBVoxelOps(IGVDBVolume *volume, IGVDBVoxelOps **ops) {
    if (volume == nullptr || ops == nullptr) return nvrhi::FE_INVALID_ARGS;
    *ops = MAKE_RC_OBJ(GVDBVoxelOps, volume);
    return *ops ? nvrhi::FS_OK : nvrhi::FE_OUT_OF_MEMORY;
}

}  // namespace gvdb
