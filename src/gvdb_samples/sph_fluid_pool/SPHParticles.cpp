// SPH particle set: the data half of the fluids5.0 Particles class
// (m_Points / m_PointsTemp / m_Accel / m_Params, ReallocateParticles,
// RebuildAccelGrid, AddPointsInVolume, UpdateParams, CommitAll, Retrieve).
#include "SPHFluid.h"
#include <sample-utils/SampleTypes.h>
#include <donut/core/log.h>
#include <nvrhi/utils.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>
#include <vector>

namespace sph {

using namespace donut;

#define SPH_CHECK_GP(expr)                                                              \
    do {                                                                                \
        nvrhi::FRESULT rc_ = (expr);                                                    \
        if (NVRHI_FAILED(rc_)) {                                                        \
            donut::log::error("SPHParticles: %s failed with error %d", #expr, (int)rc_); \
            return rc_;                                                                 \
        }                                                                               \
    } while (0)

namespace {

constexpr int kNumParticleBuffers = SPH_PARTICLE_BUFFERS;
constexpr int kNumGridBuffers = SPH_GRID_BUFFERS;

const char *const kParticleBufferNames[kNumParticleBuffers] = {"pos",   "vel",   "clr",   "veval",
                                                               "force", "press", "gcell", "gndx"};

int iDivUp(int a, int b) { return (a % b != 0) ? (a / b + 1) : (a / b); }

void computeNumBlocks(int numPnts, int minThreads, int &numBlocks, int &numThreads) {
    numThreads = std::min(minThreads, numPnts);
    numBlocks = (numThreads == 0) ? 1 : iDivUp(numPnts, numThreads);
}

// Slots the rasterizer reads: the CUDA / OpenGL interop buffers of the reference.
bool isSharedSlot(int slot) { return slot == SPH_FPOS || slot == SPH_FCLR || slot == SPH_FVEL; }

// UpdateParams of the reference: everything derived from the primary values.
void updateDerivedParams(SPHFluidParams &p) {
    p.gravity = p.grav_dir * p.grav_amt;
    p.AL2 = p.AL * p.AL;
    p.VL2 = p.VL * p.VL;

    const float sr = p.psmoothradius;
    p.r2 = sr * sr;
    p.pdist = powf(p.pmass / p.prest_dens, 1 / 3.0f);
    p.poly6kern = 315.0f / (64.0f * 3.141592f * powf(sr, 9.0f));
    p.spikykern = -45.0f / (3.141592f * powf(sr, 6.0f));
    p.lapkern = 45.0f / (3.141592f * powf(sr, 6.0f));
    p.gausskern = 1.0f / powf(3.141592f * 2.0f * sr * sr, 3.0f / 2.0f);

    p.d2 = p.sim_scale * p.sim_scale;
    p.rd2 = p.r2 / p.d2;
    p.vterm = p.lapkern * p.pvisc;
}

}  // namespace

uint32_t particleBufferStride(ParticleBuffer buffer) {
    switch (buffer) {
        case ParticleBuffer::Position:
        case ParticleBuffer::Velocity:
        case ParticleBuffer::VelocityEval:
        case ParticleBuffer::Force: return 12;
        default: return 4;
    }
}

// ---------------------------------------------------------------------------
// Parameters (SetupDefaultParams / SetupExampleParams)
// ---------------------------------------------------------------------------

void setupDefaultParams(SPHFluidParams &params) {
    //  Range = +/- 10.0 * 0.006 (r) =     0.12          m (= 120 mm = 4.7 inch)
    //  Container Volume (Vc) =            0.001728      m^3
    //  Rest Density (D) =              1000.0           kg / m^3
    //  Particle Mass (Pm) =               0.00020543    kg                      (mass = vol * density)
    //  Number of Particles (N) =       4000.0
    //  Water Mass (M) =                   0.821         kg (= 821 grams)
    //  Water Volume (V) =                 0.000821      m^3 (= 3.4 cups, .21 gals)
    //  Smoothing Radius (R) =             0.02          m (= 20 mm = ~3/4 inch)
    //  Particle Radius (Pr) =             0.00366       m (= 4 mm  = ~1/8 inch)
    //  Particle Volume (Pv) =             2.054e-7      m^3 (= .268 milliliters)
    //  Rest Distance (Pd) =               0.0059        m
    //
    // Ideal grid cell size (gs) = 2 * smoothing radius = 0.02*2 = 0.04
    // Ideal domain size = k*gs/d = k*0.02*2/0.005 = k*8 = {8, 16, 24, 32, 40, 48, ..}
    //    (k = number of cells, gs = cell size, d = simulation scale)
    params = SPHFluidParams{};

    // Set by Particles::Initialize in the reference.
    params.example = int(SPHExample::WavePool);
    params.grid_density = 2.0f;
    params.pnum = 65536;

    params.time = 0;
    params.dt = 0.004f;
    params.sim_scale = 0.006f;       // unit size
    params.pvisc = 0.10f;            // pascal-second (Pa.s) = 1 kg m^-1 s^-1
    params.prest_dens = 200.0f;      // kg / m^3
    params.pspacing = 0.0f;          // 0 = derived from the rest density
    params.pmass = 0.00020543f;      // kg
    params.pradius = 0.015f;         // m
    params.pdist = 0.0059f;          // m
    params.psmoothradius = 0.015f;   // m
    params.pintstiff = 1.5f;
    params.bound_stiff = 50000.0f;   // boundary stiffness
    params.bound_damp = 150.0f;
    params.AL = 150.0f;              // accel limit, m / s^2
    params.VL = 50.0f;               // vel limit, m / s

    params.bound_slope = 0.0f;       // ground slope
    params.bound_friction = 0.0f;    // ground friction
    params.bound_wall_force = 0.0f;
    params.grav_amt = 1.0f;

    params.grav_pos = sph_f3(0.f, 0.f, 0.f);
    params.grav_dir = sph_f3(0.f, -4.8f, 0.f);

    // Default sim config
    params.gridSize = sph_f3(params.psmoothradius * 2);
}

void setupExampleParams(SPHFluidParams &params, SPHExample example) {
    params.example = int(example);
    switch (example) {
        case SPHExample::Regression: {   // Regression test. N x N x N static grid
            int k = int(ceilf(powf(float(params.pnum), 1.0f / 3.0f)));
            params.bound_min = sph_f3(0.f, 0.f, 0.f);
            params.bound_max = sph_f3(2.0f + (k / 2), 2.0f + (k / 2), 2.0f + (k / 2));
            params.init_min = sph_f3(1.0f, 1.0f, 1.0f);
            params.init_max = sph_f3(1.0f + (k / 2), 1.0f + (k / 2), 1.0f + (k / 2));

            params.grav_amt = 0.0f;
            params.grav_dir = sph_f3(0.f, 0.f, 0.f);
            params.pspacing = 0.5f;                      // fixed spacing
            params.psmoothradius = params.pspacing;      // search radius
        } break;
        case SPHExample::Tower:
            params.bound_min = sph_f3(0.f, 0.f, 0.f);
            params.bound_max = sph_f3(256.f, 128.f, 256.f);
            params.init_min = sph_f3(5.f, 5.f, 5.f);
            params.init_max = sph_f3(256 * 0.3f, 128 * 0.9f, 256 * 0.3f);
            break;
        case SPHExample::WavePool:   // large beach front
            params.bound_min = sph_f3(0.f, 0.f, 0.f);
            params.bound_max = sph_f3(500.f, 200.f, 500.f);
            params.init_min = sph_f3(120.f, 60.f, 0.f);
            params.init_max = sph_f3(500.f, 195.f, 500.f);
            params.bound_wall_force = 100.0f;
            params.bound_wall_freq = 2.0f;
            params.bound_slope = 0.12f;
            break;
        case SPHExample::DamBreak:   // small dam break
            params.bound_min = sph_f3(-40.f, 0.f, -40.f);
            params.bound_max = sph_f3(40.f, 60.f, 40.f);
            params.init_min = sph_f3(0.f, 8.f, -35.f);
            params.init_max = sph_f3(35.f, 55.f, 35.f);
            params.bound_wall_force = 0.0f;
            params.bound_wall_freq = 0.0f;
            break;
    }
}

// ---------------------------------------------------------------------------
// SPHParticles
// ---------------------------------------------------------------------------

class SPHParticles : public nvrhi::ObjectImpl<ISPHParticles> {
 public:
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(SPHParticles)
    NVRHI_IMPLEMENTS_INTERFACE(ISPHParticles)
    NVRHI_END_INTERFACE_TABLE()

    explicit SPHParticles(gp::IDeviceQueue *queue) : m_queue(queue), m_device(queue->getDevice()) {
        setupDefaultParams(m_params);
        updateDerivedParams(m_params);
    }

    ~SPHParticles() {
        waitForQueue();
        if (m_desc.interop && m_desc.interop->getNVRHIDevice()) m_desc.interop->getNVRHIDevice()->waitForIdle();
        releaseParticleBuffers();
    }

    // ---- configuration ----
    nvrhi::FRESULT configure(const SPHParticlesDesc &desc) override;
    const SPHParticlesDesc &getDesc() const override { return m_desc; }

    // ---- parameters ----
    const SPHFluidParams &getParams() const override { return m_params; }
    void setParams(const SPHFluidParams &params) override {
        m_params = params;
        applyParticleDims();
        updateDerivedParams(m_params);
        m_paramsRevision++;
    }
    void setTime(float time) override {
        m_params.time = time;
        m_paramsRevision++;
    }
    uint64_t getParamsRevision() const override { return m_paramsRevision; }

    // ---- acceleration grid ----
    nvrhi::FRESULT rebuildAccelGrid() override;

    // ---- particles ----
    uint32_t getNumParticles() const override { return m_numParticles; }
    uint32_t getMaxParticles() const override { return m_maxParticles; }
    void clear() override {
        m_numParticles = 0;
        m_hostPos.clear();
        m_hostVel.clear();
        m_hostVelEval.clear();
        m_hostForce.clear();
        m_hostPress.clear();
        m_hostClr.clear();
        m_rng.seed(std::mt19937::default_seed);   // the same fill after every reset
    }
    uint32_t addPointsInVolume(dm::float3 boxMin, dm::float3 boxMax) override;
    nvrhi::FRESULT commit() override;
    nvrhi::FRESULT retrieve(ParticleBuffer buffer, uint32_t first, uint32_t count, void *dst) override;

    // ---- device buffers ----
    gp::IBuffer *getBuffer(ParticleBuffer buffer) override {
        return int(buffer) < kNumParticleBuffers ? m_points[int(buffer)].Get() : nullptr;
    }
    gp::IBuffer *getTempBuffer(ParticleBuffer buffer) override {
        return int(buffer) < kNumParticleBuffers ? m_pointsTemp[int(buffer)].Get() : nullptr;
    }
    gp::IBuffer *getGridBuffer(GridBuffer buffer) override {
        return int(buffer) < kNumGridBuffers ? m_accel[int(buffer)].Get() : nullptr;
    }
    nvrhi::IBuffer *getRenderBuffer(ParticleBuffer buffer) override {
        return int(buffer) < kNumParticleBuffers ? m_render[int(buffer)].Get() : nullptr;
    }
    bool areRenderBuffersShared() const override { return m_renderShared; }
    nvrhi::FRESULT updateRenderBuffers(nvrhi::ICommandList *commandList) override;
    uint64_t getLayoutRevision() const override { return m_layoutRevision; }

    dm::box3 getBounds() const override { return dm::box3(m_params.bound_min, m_params.bound_max); }

    gp::IDevice *getDevice() override { return m_device; }
    gp::IDeviceQueue *getQueue() override { return m_queue; }

 private:
    // Host-side wait for everything enqueued so far: buffers are freed
    // immediately, kernels still in the queue must not touch them afterwards.
    void waitForQueue() {
        if (!m_device || !m_queue) return;
        m_device->commitQueue(m_queue);
        m_device->waitForQueue(m_queue);
    }
    void applyParticleDims() {
        m_params.pnum = int(m_maxParticles);
        const int threadsPerBlock = 512;
        computeNumBlocks(m_params.pnum, threadsPerBlock, m_params.numBlocks, m_params.numThreads);
    }
    nvrhi::FRESULT createPlainBuffer(nvrhi::AutoPtr<gp::IBuffer> &buffer, uint64_t bytes) {
        gp::BufferDesc desc;
        desc.byteSize = size_t(bytes);
        buffer = nullptr;
        nvrhi::FRESULT fr = m_device->createBuffer(desc, &buffer);
        if (NVRHI_FAILED(fr) || !buffer || buffer->getNativeHandle() == 0) {
            log::error("SPHParticles: cannot allocate a device buffer of %llu bytes", (unsigned long long)bytes);
            buffer = nullptr;
            return NVRHI_FAILED(fr) ? fr : nvrhi::FE_OUT_OF_MEMORY;
        }
        return nvrhi::FS_OK;
    }
    void releaseParticleBuffers() {
        for (int n = 0; n < kNumParticleBuffers; ++n) {
            m_points[n] = nullptr;       // the gp view before the nvrhi buffer it maps
            m_pointsTemp[n] = nullptr;
            m_render[n] = nullptr;
            m_mirrorStaging[n] = nullptr;
        }
        m_renderShared = false;
    }
    nvrhi::BufferDesc makeRenderBufferDesc(int slot, nvrhi::SharedResourceFlags sharedFlags) const {
        nvrhi::BufferDesc bufferDesc;
        bufferDesc.byteSize = uint64_t(m_maxParticles) * particleBufferStride(ParticleBuffer(slot));
        bufferDesc.isVertexBuffer = true;
        bufferDesc.initialState = nvrhi::ResourceStates::VertexBuffer;
        bufferDesc.keepInitialState = true;
        bufferDesc.sharedResourceFlags = sharedFlags;
        bufferDesc.debugName = std::string("SPH ") + kParticleBufferNames[slot];
        return bufferDesc;
    }
    // Position / Color / Velocity as nvrhi vertex buffers imported into the
    // compute device. All three or none.
    bool createSharedRenderBuffers(nvrhi::IDevice *nvrhiDevice);

    nvrhi::AutoPtr<gp::IDeviceQueue> m_queue;
    nvrhi::AutoPtr<gp::IDevice> m_device;
    SPHParticlesDesc m_desc;
    nvrhi::AutoPtr<IGPAndNVRHIInteropDevice> m_interop;   // keeps desc.interop alive
    SPHFluidParams m_params;
    uint64_t m_paramsRevision = 1;
    uint64_t m_layoutRevision = 1;

    uint32_t m_numParticles = 0;
    uint32_t m_maxParticles = 0;
    int m_gridTotalAllocated = 0;
    uint32_t m_gridParticlesAllocated = 0;

    // host arrays (the DT_CPU side of the reference, only what the fill writes)
    std::vector<dm::float3> m_hostPos, m_hostVel, m_hostVelEval, m_hostForce;
    std::vector<float> m_hostPress;
    std::vector<uint32_t> m_hostClr;
    std::mt19937 m_rng;

    nvrhi::AutoPtr<gp::IBuffer> m_points[kNumParticleBuffers];
    nvrhi::AutoPtr<gp::IBuffer> m_pointsTemp[kNumParticleBuffers];
    nvrhi::AutoPtr<gp::IBuffer> m_accel[kNumGridBuffers];
    nvrhi::BufferHandle m_render[kNumParticleBuffers];
    bool m_renderShared = false;
    nvrhi::AutoPtr<gp::IBuffer> m_mirrorStaging[kNumParticleBuffers];   // host-visible copies (mirror mode)
};

namespace {
// Errors logged while a shared buffer is probed are not fatal: there is a
// fallback. Donut's default log callback breaks into the debugger on every
// error, so they are reported as warnings for the duration of the probe.
class ScopedSoftErrors {
 public:
    ScopedSoftErrors() : m_previous(log::GetCallback()) {
        log::Callback previous = m_previous;
        log::SetCallback([previous](log::Severity severity, const char *message) {
            if (severity == log::Severity::Error) severity = log::Severity::Warning;
            if (previous) previous(severity, message);
        });
    }
    ~ScopedSoftErrors() { log::SetCallback(m_previous); }

 private:
    log::Callback m_previous;
};
}  // namespace

bool SPHParticles::createSharedRenderBuffers(nvrhi::IDevice *nvrhiDevice) {
    ScopedSoftErrors softErrors;
    bool ok = true;
    for (int n = 0; n < kNumParticleBuffers && ok; ++n) {
        if (!isSharedSlot(n)) continue;
        const uint64_t bytes = uint64_t(m_maxParticles) * particleBufferStride(ParticleBuffer(n));
        nvrhiDevice->createBuffer(makeRenderBufferDesc(n, SampleUtils::interopSharedFlags(nvrhiDevice)), &m_render[n]);
        if (!m_render[n]) {
            ok = false;
            break;
        }
        nvrhi::FRESULT fr = m_desc.interop->createGPBuffer(m_render[n], &m_points[n]);
        ok = !NVRHI_FAILED(fr) && m_points[n] && m_points[n]->getNativeHandle() != 0 &&
             m_points[n]->getDesc()->byteSize >= bytes;
    }
    if (!ok) {
        for (int n = 0; n < kNumParticleBuffers; ++n) {
            if (!isSharedSlot(n)) continue;
            m_points[n] = nullptr;
            m_render[n] = nullptr;
        }
    }
    return ok;
}

nvrhi::FRESULT SPHParticles::updateRenderBuffers(nvrhi::ICommandList *commandList) {
    if (m_renderShared || !m_render[SPH_FPOS]) return nvrhi::FS_OK;   // zero copy, or nothing to draw from
    if (!commandList) return nvrhi::FE_INVALID_ARGS;
    if (m_numParticles == 0) return nvrhi::FS_OK;

    for (int n = 0; n < kNumParticleBuffers; ++n) {
        if (!isSharedSlot(n)) continue;
        const uint64_t bytes = uint64_t(m_numParticles) * particleBufferStride(ParticleBuffer(n));
        if (!m_mirrorStaging[n]) {
            gp::BufferDesc desc;
            desc.byteSize = size_t(uint64_t(m_maxParticles) * particleBufferStride(ParticleBuffer(n)));
            desc.isStaging = true;
            SPH_CHECK_GP(m_device->createBuffer(desc, &m_mirrorStaging[n]));
            if (!m_mirrorStaging[n]) return nvrhi::FE_OUT_OF_MEMORY;
        }
        SPH_CHECK_GP(m_queue->copyBufferRegion(m_mirrorStaging[n], 0, m_points[n], 0, bytes));
    }
    waitForQueue();
    for (int n = 0; n < kNumParticleBuffers; ++n) {
        if (!isSharedSlot(n)) continue;
        const uint64_t bytes = uint64_t(m_numParticles) * particleBufferStride(ParticleBuffer(n));
        void *data = nullptr;
        SPH_CHECK_GP(m_device->mapBuffer(m_mirrorStaging[n], &data));
        if (!data) return nvrhi::FE_GENERIC_ERROR;
        commandList->writeBuffer(m_render[n], data, size_t(bytes));
        m_device->unmapBuffer(m_mirrorStaging[n]);
    }
    return nvrhi::FS_OK;
}

// ReallocateParticles
nvrhi::FRESULT SPHParticles::configure(const SPHParticlesDesc &desc) {
    if (desc.maxParticles == 0 || desc.maxParticles > 0x7fffffffu / 12u) return nvrhi::FE_INVALID_ARGS;

    waitForQueue();
    if (m_desc.interop && m_desc.interop->getNVRHIDevice()) m_desc.interop->getNVRHIDevice()->waitForIdle();
    releaseParticleBuffers();
    clear();

    m_desc = desc;
    m_interop = desc.interop;
    m_maxParticles = desc.maxParticles;
    applyParticleDims();
    m_paramsRevision++;
    m_layoutRevision++;

    // The rasterizer's buffers: shared with the compute device when the import
    // works (zero copy), otherwise separate vertex buffers that
    // updateRenderBuffers() refreshes through the host.
    nvrhi::IDevice *nvrhiDevice = desc.interop ? desc.interop->getNVRHIDevice() : nullptr;
    m_renderShared = false;
    if (nvrhiDevice) {
        m_renderShared = createSharedRenderBuffers(nvrhiDevice);
        if (!m_renderShared) {
            log::warning("SPHParticles: %s vertex buffers cannot be imported into the compute device; the Position / "
                         "Color / Velocity buffers are copied through the host every frame instead",
                         nvrhi::utils::GraphicsAPIToString(nvrhiDevice->getGraphicsAPI()));
            for (int n = 0; n < kNumParticleBuffers; ++n) {
                if (!isSharedSlot(n)) continue;
                nvrhiDevice->createBuffer(makeRenderBufferDesc(n, nvrhi::SharedResourceFlags::None), &m_render[n]);
                if (!m_render[n]) {
                    log::error("SPHParticles: cannot create the vertex buffer '%s'", kParticleBufferNames[n]);
                    releaseParticleBuffers();
                    return nvrhi::FE_OUT_OF_MEMORY;
                }
            }
        }
    }
    for (int n = 0; n < kNumParticleBuffers; ++n) {
        const uint64_t bytes = uint64_t(m_maxParticles) * particleBufferStride(ParticleBuffer(n));
        if (!m_points[n]) {
            nvrhi::FRESULT fr = createPlainBuffer(m_points[n], bytes);
            if (NVRHI_FAILED(fr)) {
                releaseParticleBuffers();
                return fr;
            }
            // Entries past the particle count are still visited by the advance
            // kernel (it runs over pnum as in the reference): keep them defined.
            // (The shared buffers are cleared by commit(), under the caller's
            // keyed mutexes on D3D11.)
            SPH_CHECK_GP(m_queue->clearBufferUint(m_points[n], 0u));
        }
        // MatchAllBuffers: the copy the counting sort reads from
        nvrhi::FRESULT fr = createPlainBuffer(m_pointsTemp[n], bytes);
        if (NVRHI_FAILED(fr)) {
            releaseParticleBuffers();
            return fr;
        }
        SPH_CHECK_GP(m_queue->clearBufferUint(m_pointsTemp[n], 0u));
    }
    log::info("SPHParticles: %d particles max, t:%dx%d=%d%s", m_params.pnum, m_params.numBlocks, m_params.numThreads,
              m_params.numBlocks * m_params.numThreads,
              !nvrhiDevice ? "" : m_renderShared ? ", pos / clr / vel shared with nvrhi" : ", pos / clr / vel mirrored to nvrhi");
    return nvrhi::FS_OK;
}

// RebuildAccelGrid
//
// Ideal grid cell size (gs) = 2 * smoothing radius = 0.02*2 = 0.04
// Ideal domain size = k * gs / d = k*0.02*2/0.005 = k*8 = {8, 16, 24, 32, 40, 48, ..}
//    (k = number of cells, gs = cell size, d = simulation scale)
nvrhi::FRESULT SPHParticles::rebuildAccelGrid() {
    if (m_maxParticles == 0) {
        log::error("SPHParticles: rebuildAccelGrid() needs configure() first");
        return nvrhi::FE_INVALID_ARGS;
    }
    SPHFluidParams p = m_params;

    // Grid size - cell spacing in SPH units
    p.grid_size = 1.5f * p.psmoothradius / p.grid_density;

    // Grid bounds - one cell beyond the fluid domain
    p.gridMin = p.bound_min - float(2.0 * (p.grid_size / p.sim_scale));
    p.gridMax = p.bound_max + float(2.0 * (p.grid_size / p.sim_scale));
    p.gridSize = p.gridMax - p.gridMin;

    const float world_cellsize = p.grid_size / p.sim_scale;   // cell spacing in world units
    const float sim_scale = p.sim_scale;

    // Grid res - grid volume uniformly sub-divided by grid size
    p.gridRes.x = int(ceilf(p.gridSize.x / world_cellsize));
    p.gridRes.y = int(ceilf(p.gridSize.y / world_cellsize));
    p.gridRes.z = int(ceilf(p.gridSize.z / world_cellsize));
    p.gridSize.x = p.gridRes.x * world_cellsize;   // adjust grid size to a multiple of the cell size
    p.gridSize.y = p.gridRes.y * world_cellsize;
    p.gridSize.z = p.gridRes.z * world_cellsize;
    p.gridDelta = dm::float3(p.gridRes) / p.gridSize;   // delta = translate from world space to cell #

    // Grid total - total number of grid cells
    const int64_t total = int64_t(p.gridRes.x) * p.gridRes.y * p.gridRes.z;
    if (p.gridRes.x <= 0 || p.gridRes.y <= 0 || p.gridRes.z <= 0 || total > 0x7fffffff / 4) {
        log::error("SPHParticles: invalid acceleration grid %d x %d x %d", p.gridRes.x, p.gridRes.y, p.gridRes.z);
        return nvrhi::FE_INVALID_ARGS;
    }
    p.gridTotal = int(total);

    // Number of cells to search:
    // n = (2r / w) + 1, where n = 1D cell search count, r = search radius, w = world cell width
    p.gridSrch = int(floorf(2.0f * (p.psmoothradius / sim_scale) / world_cellsize) + 1.0f);
    if (p.gridSrch < 2) p.gridSrch = 2;
    p.gridAdjCnt = p.gridSrch * p.gridSrch * p.gridSrch;
    p.gridScanMax = p.gridRes - dm::int3(p.gridSrch, p.gridSrch, p.gridSrch);

    // The reference exits here; gridAdj holds a search width of 6 at most.
    if (p.gridSrch > 6 || p.gridAdjCnt > SPH_MAX_GRID_ADJ) {
        log::error("SPHParticles: neighbor search is n > 6 (n = %d); increase the grid cell size", p.gridSrch);
        return nvrhi::FE_INVALID_ARGS;
    }
    // The prefix scan has three levels of SPH_SCAN_BLOCKSIZE * 2 elements.
    const int blockSize = SPH_SCAN_BLOCKSIZE << 1;
    if (int64_t(p.gridTotal) > int64_t(blockSize) * blockSize * blockSize) {
        log::error("SPHParticles: %d grid cells exceed the prefix sum maximum", p.gridTotal);
        return nvrhi::FE_INVALID_ARGS;
    }

    // Grid adjacency lookup - stride to access the neighboring cells
    int cell = 0;
    for (int y = 0; y < p.gridSrch; y++)
        for (int z = 0; z < p.gridSrch; z++)
            for (int x = 0; x < p.gridSrch; x++) p.gridAdj[cell++] = (y * p.gridRes.z + z) * p.gridRes.x + x;

    // Allocate acceleration (kept when the sizes did not change)
    if (p.gridTotal != m_gridTotalAllocated || m_maxParticles != m_gridParticlesAllocated || !m_accel[SPH_AGRID]) {
        waitForQueue();
        for (auto &buffer : m_accel) buffer = nullptr;
        m_gridTotalAllocated = 0;
        m_layoutRevision++;

        // Auxiliary buffers - prefix sum sizes
        const int numElem1 = p.gridTotal;
        const int numElem2 = int(numElem1 / blockSize) + 1;
        const int numElem3 = int(numElem2 / blockSize) + 1;
        const uint64_t u = sizeof(uint32_t);
        const uint64_t sizes[kNumGridBuffers] = {u * m_maxParticles, u * numElem1, u * numElem1, u * numElem2,
                                                 u * numElem2,       u * numElem3, u * numElem3};
        for (int n = 0; n < kNumGridBuffers; ++n) {
            nvrhi::FRESULT fr = createPlainBuffer(m_accel[n], sizes[n]);
            if (NVRHI_FAILED(fr)) {
                for (auto &buffer : m_accel) buffer = nullptr;
                return fr;
            }
            SPH_CHECK_GP(m_queue->clearBufferUint(m_accel[n], 0u));
        }
        m_gridTotalAllocated = p.gridTotal;
        m_gridParticlesAllocated = m_maxParticles;
    }

    m_params = p;
    updateDerivedParams(m_params);
    m_paramsRevision++;

    log::info("SPHParticles: accel grid %d cells, res %dx%dx%d, search %d", p.gridTotal, p.gridRes.x, p.gridRes.y,
              p.gridRes.z, p.gridSrch);
    return nvrhi::FS_OK;
}

// AddPointsInVolume (+ AddParticle)
uint32_t SPHParticles::addPointsInVolume(dm::float3 min, dm::float3 max) {
    SPHFluidParams &p = m_params;

    // Determine particle density / spacing
    if (p.pspacing == 0) {
        // Determine spacing from density
        p.pdist = powf(p.pmass / p.prest_dens, 1 / 3.0f);
        p.pspacing = p.pdist * 0.87f / p.sim_scale;
    } else {
        // Determine density from spacing
        p.pdist = p.pspacing * p.sim_scale / 0.87f;
        p.prest_dens = p.pmass / powf(p.pdist, 3.0f);
    }
    log::info("SPHParticles: AddPointsInVolume. Density: %f, Spacing: %f, PDist: %f", p.prest_dens, p.pspacing, p.pdist);

    // Distribute points at rest spacing
    const uint32_t before = m_numParticles;
    const float spacing = p.pspacing;
    const float offs = 0;
    if (!(spacing > 0.f)) return 0;
    const int cntx = int(ceilf((max.x - min.x - offs) / spacing));
    const int cntz = int(ceilf((max.z - min.z - offs) / spacing));
    const int cnt = cntx * cntz;

    min += offs;
    max -= offs;
    const float dx = max.x - min.x;
    const float dz = max.z - min.z;

    auto rnd = [&]() { return float(m_rng() >> 8) * (1.0f / 16777216.0f) * spacing; };   // [0, spacing)

    dm::float3 pos;
    bool full = m_numParticles >= m_maxParticles;
    for (pos.y = min.y; pos.y <= max.y && !full && cnt > 0; pos.y += spacing) {
        for (int xz = 0; xz < cnt; xz++) {
            if (m_numParticles >= m_maxParticles) {   // AddParticle() == -1
                full = true;
                break;
            }
            pos.x = min.x + (xz % cntx) * spacing;
            pos.z = min.z + (xz / cntx) * spacing;

            dm::float3 jitter;
            jitter.x = rnd();
            jitter.y = rnd();
            jitter.z = rnd();
            m_hostPos.push_back(pos + jitter);

            // particle color
            dm::float3 pnt((pos.x - min.x) / dx, 0.f, (pos.z - min.z) / dz);
            dm::float3 clr(0.f, pnt.x, (pnt.x + pnt.z) * 0.5f);
            clr *= 0.7f;
            clr += 0.2f;
            clr = dm::clamp(clr, dm::float3(0.f), dm::float3(1.f));
            m_hostClr.push_back(SampleUtils::packColorA(clr.x, clr.y, clr.z, 1.f));
            m_numParticles++;
        }
    }
    // AddParticle: the other attributes start at zero
    m_hostVel.resize(m_numParticles, dm::float3(0.f));
    m_hostVelEval.resize(m_numParticles, dm::float3(0.f));
    m_hostForce.resize(m_numParticles, dm::float3(0.f));
    m_hostPress.resize(m_numParticles, 0.f);

    updateDerivedParams(m_params);
    m_paramsRevision++;
    return m_numParticles - before;
}

// CommitAll
nvrhi::FRESULT SPHParticles::commit() {
    if (m_maxParticles == 0 || !m_points[SPH_FPOS]) return nvrhi::FE_INVALID_ARGS;

    // Everything past the particle count (and the buffers without a host
    // array) is zero, as in the freshly allocated buffers of the reference.
    for (int n = 0; n < kNumParticleBuffers; ++n) SPH_CHECK_GP(m_queue->clearBufferUint(m_points[n], 0u));
    if (m_numParticles == 0) return nvrhi::FS_OK;

    const uint64_t n3 = uint64_t(m_numParticles) * sizeof(dm::float3);
    const uint64_t n1 = uint64_t(m_numParticles) * sizeof(uint32_t);
    SPH_CHECK_GP(m_queue->writeBuffer(m_points[SPH_FPOS], m_hostPos.data(), n3, 0));
    SPH_CHECK_GP(m_queue->writeBuffer(m_points[SPH_FVEL], m_hostVel.data(), n3, 0));
    SPH_CHECK_GP(m_queue->writeBuffer(m_points[SPH_FCLR], m_hostClr.data(), n1, 0));
    SPH_CHECK_GP(m_queue->writeBuffer(m_points[SPH_FVEVAL], m_hostVelEval.data(), n3, 0));
    SPH_CHECK_GP(m_queue->writeBuffer(m_points[SPH_FFORCE], m_hostForce.data(), n3, 0));
    SPH_CHECK_GP(m_queue->writeBuffer(m_points[SPH_FPRESS], m_hostPress.data(), n1, 0));
    return nvrhi::FS_OK;
}

// Retrieve
nvrhi::FRESULT SPHParticles::retrieve(ParticleBuffer buffer, uint32_t first, uint32_t count, void *dst) {
    if (int(buffer) >= kNumParticleBuffers || !dst || !m_points[int(buffer)]) return nvrhi::FE_INVALID_ARGS;
    if (uint64_t(first) + count > m_maxParticles) return nvrhi::FE_INVALID_ARGS;
    if (count == 0) return nvrhi::FS_OK;

    const uint64_t stride = particleBufferStride(buffer);
    gp::BufferDesc desc;
    desc.byteSize = size_t(stride * count);
    desc.isStaging = true;
    nvrhi::AutoPtr<gp::IBuffer> staging;
    SPH_CHECK_GP(m_device->createBuffer(desc, &staging));
    if (!staging) return nvrhi::FE_OUT_OF_MEMORY;
    SPH_CHECK_GP(m_queue->copyBufferRegion(staging, 0, m_points[int(buffer)], stride * first, stride * count));
    waitForQueue();
    void *data = nullptr;
    SPH_CHECK_GP(m_device->mapBuffer(staging, &data));
    if (!data) return nvrhi::FE_GENERIC_ERROR;
    memcpy(dst, data, size_t(stride * count));
    m_device->unmapBuffer(staging);
    return nvrhi::FS_OK;
}

nvrhi::FRESULT createSPHParticles(gp::IDeviceQueue *queue, ISPHParticles **outParticles) {
    if (queue == nullptr || outParticles == nullptr) return nvrhi::FE_INVALID_ARGS;
    *outParticles = MAKE_RC_OBJ(SPHParticles, queue);
    return *outParticles ? nvrhi::FS_OK : nvrhi::FE_OUT_OF_MEMORY;
}

}  // namespace sph
