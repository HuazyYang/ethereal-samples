// IGVDBVoxelOps: operators on the channel data of one IGVDBVolume (port of
// VolumeGVDB::FillChannel / ClearChannel / Compute / ComputeKernel /
// UpdateApron / Resample / DownsampleCPU / Reduction and Allocator::AtlasFill).
// Only the public IGVDBVolume interface is used; the kernels come from
// shaders/GVDBOperators.slang through IGVDBVolume::getKernel().
#include <gvdb/GVDB.h>
#include "GVDBPrimitives.h"
#include <donut/core/log.h>
#include <cstring>

namespace gvdb {

using namespace donut;

namespace {

inline dm::int3 toInt3(dm::uint3 v) { return dm::int3{int(v.x), int(v.y), int(v.z)}; }

// Name of the operator kernel of a ComputeOp (all have the arguments
// (VDBInfo*, int3 atlasRes, uint8_t channel, float p1, float p2, float p3)).
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
    void computeKernel(IComputeKernel *kernel, int channel, bool updateApron, bool skipOverAprons) override;
    void updateApron() override;
    void updateApron(int channel, float boundValue) override;
    void resample(int channel, const dm::float4x4 &xform, dm::int3 srcRes, nvrhi::IBuffer *src, dm::float3 inRange,
                  dm::float3 outRange) override;
    void downsample(const dm::float4x4 &xform, dm::int3 srcRes, nvrhi::IBuffer *src, dm::int3 dstRes,
                    dm::float3 dstMax, dm::float3 inRange, dm::float3 outRange,
                    std::vector<float> &outValues) override;
    float reduction(int channel) override;

 private:
    IComputeContext *context() const { return m_volume->getContext(); }
    nvrhi::ICommandList *commandList() const { return m_volume->getContext()->getCommandList(); }

    // A channel index that has an atlas texture (false logs a warning).
    bool checkChannel(int channel, const char *what) const;
    // Uploads the 16 floats of xform (dm::float4x4 memory layout, row-vector
    // convention: the kernels compute p.x*m[0] + p.y*m[4] + p.z*m[8] + m[12]).
    void uploadMatrix(const dm::float4x4 &xform);
    // Launches an operator kernel `k(VDBInfo*, int3 atlasRes, uint8_t channel, float, float, float)`
    // over the atlas (8x8x8 thread groups; the packed resolution when skipOverAprons).
    void launchOperator(IComputeKernel *kernel, int channel, dm::float3 params, bool skipOverAprons,
                        bool withParams);

    nvrhi::AutoPtr<IGVDBVolume> m_volume;
    nvrhi::BufferHandle m_matrixBuffer;        // 16 floats
    nvrhi::BufferHandle m_reductionBuffer;     // packres.x * packres.y floats
    nvrhi::BufferHandle m_downsampleBuffer;    // dstRes.x * y * z floats
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

void GVDBVoxelOps::uploadMatrix(const dm::float4x4 &xform) {
    static_assert(sizeof(dm::float4x4) == 16 * sizeof(float), "float4x4 must be 16 contiguous floats");
    m_matrixBuffer = GVDBPrimitives::ensureBuffer(context(), m_matrixBuffer, 16 * sizeof(float), "GVDB operator matrix");
    commandList()->writeBuffer(m_matrixBuffer, &xform, 16 * sizeof(float), 0);
}

void GVDBVoxelOps::launchOperator(IComputeKernel *kernel, int channel, dm::float3 params, bool skipOverAprons,
                                  bool withParams) {
    if (kernel == nullptr) return;
    m_volume->prepareVDB();

    const dm::int3 atlasRes = toInt3(m_volume->getAtlasResDim());
    const dm::int3 threads = skipOverAprons ? toInt3(m_volume->getAtlasPackedResDim()) : atlasRes;
    if (threads.x <= 0 || threads.y <= 0 || threads.z <= 0) return;
    const dm::uint3 groups = groupsFor(kernel, dm::uint3(threads));

    const uint64_t vdbInfo = deviceAddress(m_volume->getVDBInfoGPU());
    const uint8_t chan = uint8_t(channel);
    if (withParams)
        launchKernel(commandList(), kernel, groups, vdbInfo, atlasRes, chan, params.x, params.y, params.z);
    else
        launchKernel(commandList(), kernel, groups, vdbInfo, atlasRes, chan);
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
    // gvdbClearAtlas*(VDBInfo*, int3 atlasRes, uint8_t channel) over the whole atlas.
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
    IComputeKernel *kernel = name ? m_volume->getKernel(name) : nullptr;
    if (kernel == nullptr) {
        log::warning("GVDBVoxelOps::compute: operator kernel %s not found", name ? name : "(unknown op)");
        return;
    }

    for (int n = 0; n < numIterations; ++n) {
        launchOperator(kernel, channel, params, skipOverAprons, true);
        if (bUpdateApron) updateApron(channel, boundValue);
    }
}

void GVDBVoxelOps::computeKernel(IComputeKernel *kernel, int channel, bool bUpdateApron, bool skipOverAprons) {
    if (kernel == nullptr) {
        log::warning("GVDBVoxelOps::computeKernel: no kernel");
        return;
    }
    if (!checkChannel(channel, "computeKernel")) return;

    // k(VDBInfo*, int3 atlasRes, uint8_t channel): the first three operator arguments.
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
    IComputeKernel *kernel = m_volume->getKernel(name);
    if (kernel == nullptr) return;
    if (apron != 1) {
        log::warning("GVDBVoxelOps::updateApron: the apron kernels assume an apron of 1 (it is %d)", apron);
        return;
    }

    // Dimensions
    const int bricks = (int)m_volume->getNumNodes(0);          // all level-0 nodes (the kernel skips inactive ones)
    const int brickres = (int)m_volume->getBrickDimWithApron().x;  // brick side including the apron
    const int brickwid = (int)m_volume->getBrickDim().x;           // brick side without the apron
    if (bricks == 0) return;

    m_volume->prepareVDB();

    // One thread group column per brick (6 x 8 x 8 threads); each yz plane of a group fills one of the 6 faces.
    const dm::uint3 groups(uint32_t(bricks), dm::div_ceil(uint32_t(brickres), 8u), dm::div_ceil(uint32_t(brickres), 8u));
    launchKernel(commandList(), kernel, groups, deviceAddress(m_volume->getVDBInfoGPU()), uint8_t(channel), bricks,
                 brickres, brickwid, boundValue);
}

// ---------------------------------------------------------------------------
// resample / downsample / reduction
// ---------------------------------------------------------------------------

void GVDBVoxelOps::resample(int channel, const dm::float4x4 &xform, dm::int3 srcRes, nvrhi::IBuffer *src,
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
    IComputeKernel *kernel = m_volume->getKernel("gvdbResample");
    if (kernel == nullptr) return;

    m_volume->prepareVDB();
    uploadMatrix(xform);

    // Grid over the whole atlas (GVDB_VOXUNPACKED)
    const dm::int3 res = toInt3(m_volume->getAtlasResDim());
    if (res.x <= 0 || res.y <= 0 || res.z <= 0) return;
    launchKernel(commandList(), kernel, groupsFor(kernel, dm::uint3(res)), deviceAddress(m_volume->getVDBInfoGPU()), res,
                 uint8_t(channel), srcRes, deviceAddress(src), deviceAddress(m_matrixBuffer), inRange, outRange);
}

void GVDBVoxelOps::downsample(const dm::float4x4 &xform, dm::int3 srcRes, nvrhi::IBuffer *src, dm::int3 dstRes,
                              dm::float3 dstMax, dm::float3 inRange, dm::float3 outRange,
                              std::vector<float> &outValues) {
    outValues.clear();
    if (src == nullptr || srcRes.x <= 0 || srcRes.y <= 0 || srcRes.z <= 0) {
        log::warning("GVDBVoxelOps::downsample: no source volume");
        return;
    }
    if (dstRes.x <= 0 || dstRes.y <= 0 || dstRes.z <= 0) return;
    IComputeKernel *kernel = m_volume->getKernel("gvdbDownsample");
    if (kernel == nullptr) return;

    const size_t count = size_t(dstRes.x) * size_t(dstRes.y) * size_t(dstRes.z);
    const size_t bytes = count * sizeof(float);
    m_downsampleBuffer = GVDBPrimitives::ensureBuffer(context(), m_downsampleBuffer, bytes, "GVDB downsample");
    uploadMatrix(xform);

    launchKernel(commandList(), kernel, groupsFor(kernel, dm::uint3(dstRes)), srcRes, deviceAddress(src), dstRes, dstMax,
                 deviceAddress(m_downsampleBuffer), deviceAddress(m_matrixBuffer), inRange, outRange);

    // Retrieve data back to the host
    outValues.resize(count);
    context()->readBuffer(m_downsampleBuffer, 0, outValues.data(), bytes);
}

float GVDBVoxelOps::reduction(int channel) {
    if (!checkChannel(channel, "reduction")) return 0.f;
    if (m_volume->getChannelDesc(channel).format != ATLAS_FORMAT_R32_FLOAT) {
        log::warning("GVDBVoxelOps::reduction: channel %d is not a float channel", channel);
        return 0.f;
    }
    IComputeKernel *kernel = m_volume->getKernel("gvdbReduction");
    if (kernel == nullptr) return 0.f;

    m_volume->prepareVDB();

    // 2D grid over the x/y plane of the packed atlas (apron voxels excluded);
    // the kernel integrates along z.
    const dm::int3 packres = toInt3(m_volume->getAtlasPackedResDim());
    const dm::int3 res = toInt3(m_volume->getAtlasResDim());
    if (packres.x <= 0 || packres.y <= 0 || packres.z <= 0) return 0.f;

    const size_t count = size_t(packres.x) * size_t(packres.y);
    const size_t bytes = count * sizeof(float);
    m_reductionBuffer = GVDBPrimitives::ensureBuffer(context(), m_reductionBuffer, bytes, "GVDB reduction");

    launchKernel(commandList(), kernel, groupsFor(kernel, dm::uint3(uint32_t(packres.x), uint32_t(packres.y), 1u)),
                 deviceAddress(m_volume->getVDBInfoGPU()), res, uint8_t(channel), packres, deviceAddress(m_reductionBuffer));

    std::vector<float> sums(count);
    context()->readBuffer(m_reductionBuffer, 0, sums.data(), bytes);

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
