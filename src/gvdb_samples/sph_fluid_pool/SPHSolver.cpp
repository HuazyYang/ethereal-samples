// SPH solver: the operation half of the fluids5.0 Particles class (Run,
// InsertParticles, PrefixScanParticles, CountingSort, ComputePressure,
// ComputeForce, Advance; the GPU path). Works on an ISPHParticles through its
// public interface only.
#include "SPHFluid.h"
#include <donut/core/log.h>
#include <nvrhi/core/datablob.h>

namespace sph {

using namespace donut;

#define SPH_V_GP(expr)                                                               \
    do {                                                                             \
        nvrhi::FRESULT rc_ = (expr);                                                 \
        if (NVRHI_FAILED(rc_)) {                                                     \
            donut::log::error("SPHSolver: %s failed with error %d", #expr, (int)rc_); \
            NVRHI_ASSERT(0);                                                         \
        }                                                                            \
    } while (0)

class SPHSolver : public nvrhi::ObjectImpl<ISPHSolver> {
 public:
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(SPHSolver)
    NVRHI_IMPLEMENTS_INTERFACE(ISPHSolver)
    NVRHI_END_INTERFACE_TABLE()

    explicit SPHSolver(ISPHParticles *particles)
        : m_particles(particles), m_device(particles->getDevice()), m_queue(particles->getQueue()) {}

    ~SPHSolver() {
        // Kernels of this module may still be queued.
        if (m_device && m_queue) {
            m_device->commitQueue(m_queue);
            m_device->waitForQueue(m_queue);
        }
    }

    // Initialize / LoadKernel of the reference.
    nvrhi::FRESULT initialize(vfs::IFileSystem *vfs);

    ISPHParticles *getParticles() override { return m_particles; }

    void insertParticles() override;
    void prefixScanParticles() override;
    void countingSort() override;
    void computePressure() override;
    void computeForce() override;
    void advance() override;
    void step() override;

    float getTime() const override { return m_particles->getParams().time; }
    uint32_t getFrame() const override { return m_frame; }
    void reset() override {
        m_frame = 0;
        setTime(0.f);
    }

 private:
    // Brings the module's __constant__ data up to date with the particle set:
    // the three pointer tables when buffers were reallocated, the parameter
    // block when parameters changed. Returns false when there is nothing to run.
    bool prepare();
    // Sets the simulation time without causing a parameter upload: the kernels
    // take the time as an argument and never read FParams.time.
    void setTime(float time) {
        const uint64_t before = m_particles->getParamsRevision();
        m_particles->setTime(time);
        if (m_paramsRevision == before) m_paramsRevision = m_particles->getParamsRevision();
    }
    void launch(gp::IKernel *kernel, int blocks, int threads, const gp::KernelArg *args, size_t argc) {
        SPH_V_GP(m_queue->launch(kernel, gp::dim3{blocks, 1, 1}, gp::dim3{threads, 1, 1}, args, argc));
    }
    // A particle kernel over numBlocks x numThreads (covers the capacity; the
    // kernels return for indices past their count argument).
    void launchParticles(gp::IKernel *kernel, const gp::KernelArg *args, size_t argc) {
        const SPHFluidParams &p = m_particles->getParams();
        launch(kernel, p.numBlocks, p.numThreads, args, argc);
    }

    nvrhi::AutoPtr<ISPHParticles> m_particles;
    nvrhi::AutoPtr<gp::IDevice> m_device;
    nvrhi::AutoPtr<gp::IDeviceQueue> m_queue;
    nvrhi::AutoPtr<gp::IModule> m_module;
    nvrhi::AutoPtr<gp::IKernel> m_kInsert, m_kCountingSort, m_kPressure, m_kForce, m_kAdvance, m_kPrefixSum,
        m_kPrefixFixup;
    uint64_t m_layoutRevision = 0;   // revisions of the particle set the constants were uploaded for
    uint64_t m_paramsRevision = 0;
    uint32_t m_frame = 0;
};

nvrhi::FRESULT SPHSolver::initialize(vfs::IFileSystem *vfs) {
    nvrhi::AutoPtr<nvrhi::IDataBlob> ptx;
    if (!vfs || NVRHI_FAILED(vfs->readFile(SPH_PTX_KERNELS, &ptx)) || !ptx) {
        log::error("SPHSolver: cannot read %s", SPH_PTX_KERNELS);
        return nvrhi::FE_NOT_FOUND;
    }
    // The PTX text must be zero terminated.
    size_t len = ptx->GetSize();
    ptx->Resize(len + 1);
    static_cast<char *>(ptx->GetDataPtr())[len] = 0;
    if (NVRHI_FAILED(m_device->createModule({}, ptx->GetDataPtr(), len + 1, &m_module)) || !m_module) {
        log::error("SPHSolver: cannot create the kernel module from %s", SPH_PTX_KERNELS);
        return nvrhi::FE_GENERIC_ERROR;
    }
    struct {
        const char *name;
        nvrhi::AutoPtr<gp::IKernel> *kernel;
    } kernels[] = {
        {"insertParticles", &m_kInsert}, {"countingSortFull", &m_kCountingSort}, {"computePressure", &m_kPressure},
        {"computeForce", &m_kForce},     {"advanceParticles", &m_kAdvance},      {"prefixSum", &m_kPrefixSum},
        {"prefixFixup", &m_kPrefixFixup},
    };
    for (auto &k : kernels) {
        if (NVRHI_FAILED(m_module->getKernel(k.name, &(*k.kernel))) || !*k.kernel) {
            log::error("SPHSolver: kernel %s not found in %s", k.name, SPH_PTX_KERNELS);
            return nvrhi::FE_NOT_FOUND;
        }
    }
    return nvrhi::FS_OK;
}

bool SPHSolver::prepare() {
    if (m_particles->getNumParticles() == 0 || m_particles->getMaxParticles() == 0) return false;
    if (!m_particles->getGridBuffer(GridBuffer::Grid) || !m_particles->getBuffer(ParticleBuffer::Position)) return false;

    if (m_layoutRevision != m_particles->getLayoutRevision()) {
        // UpdateGPUAccess: the tables of device pointers the kernels index.
        SPHBufferTable points = {}, temp = {}, accel = {};
        for (int n = 0; n < SPH_PARTICLE_BUFFERS; ++n) {
            gp::IBuffer *buffer = m_particles->getBuffer(ParticleBuffer(n));
            gp::IBuffer *tempBuffer = m_particles->getTempBuffer(ParticleBuffer(n));
            points.buf[n] = buffer ? (unsigned long long)buffer->getNativeHandle() : 0ull;
            temp.buf[n] = tempBuffer ? (unsigned long long)tempBuffer->getNativeHandle() : 0ull;
        }
        for (int n = 0; n < SPH_GRID_BUFFERS; ++n) {
            gp::IBuffer *buffer = m_particles->getGridBuffer(GridBuffer(n));
            accel.buf[n] = buffer ? (unsigned long long)buffer->getNativeHandle() : 0ull;
        }
        // The symbols belong to the module; any of its kernels addresses them.
        SPH_V_GP(m_queue->setConstantBuffer(m_kInsert, "FPnts", &points, sizeof(points)));
        SPH_V_GP(m_queue->setConstantBuffer(m_kInsert, "FPntTmp", &temp, sizeof(temp)));
        SPH_V_GP(m_queue->setConstantBuffer(m_kInsert, "FAccel", &accel, sizeof(accel)));
        m_layoutRevision = m_particles->getLayoutRevision();
    }
    if (m_paramsRevision != m_particles->getParamsRevision()) {
        SPHFluidParams params = m_particles->getParams();
        SPH_V_GP(m_queue->setConstantBuffer(m_kInsert, "FParams", &params, sizeof(params)));
        m_paramsRevision = m_particles->getParamsRevision();
    }
    return true;
}

void SPHSolver::insertParticles() {
    if (!prepare()) return;
    // Reset all grid cells to empty
    SPH_V_GP(m_queue->clearBufferUint(m_particles->getGridBuffer(GridBuffer::GridCount), 0u));
    SPH_V_GP(m_queue->clearBufferUint(m_particles->getGridBuffer(GridBuffer::GridOffset), 0u));
    SPH_V_GP(m_queue->clearBufferUint(m_particles->getBuffer(ParticleBuffer::GridCell), 0u));
    SPH_V_GP(m_queue->clearBufferUint(m_particles->getBuffer(ParticleBuffer::GridIndex), 0u));

    gp::KernelArg args[] = {gp::KernelArg::Scalar(int(m_particles->getNumParticles()))};
    launchParticles(m_kInsert, args, 1);
}

void SPHSolver::prefixScanParticles() {
    if (!prepare()) return;
    using KA = gp::KernelArg;

    // Prefix Sum - determine grid offsets
    const int blockSize = SPH_SCAN_BLOCKSIZE << 1;
    const int numElem1 = m_particles->getParams().gridTotal;
    const int numElem2 = int(numElem1 / blockSize) + 1;
    const int numElem3 = int(numElem2 / blockSize) + 1;
    const int threads = SPH_SCAN_BLOCKSIZE;
    const int zero_offsets = 1;
    const int zon = 1;
    const KA nullPtr = KA::Scalar(uint64_t(0));   // a null uint* argument

    gp::IBuffer *array1 = m_particles->getGridBuffer(GridBuffer::GridCount);    // input
    gp::IBuffer *scan1 = m_particles->getGridBuffer(GridBuffer::GridOffset);    // output
    gp::IBuffer *array2 = m_particles->getGridBuffer(GridBuffer::AuxArray1);
    gp::IBuffer *scan2 = m_particles->getGridBuffer(GridBuffer::AuxScan1);
    gp::IBuffer *array3 = m_particles->getGridBuffer(GridBuffer::AuxArray2);
    gp::IBuffer *scan3 = m_particles->getGridBuffer(GridBuffer::AuxScan2);

    // sum array1. output -> scan1, array2
    KA argsA[] = {KA::Buffer(array1), KA::Buffer(scan1), KA::Buffer(array2), KA::Scalar(numElem1), KA::Scalar(zero_offsets)};
    launch(m_kPrefixSum, numElem2, threads, argsA, 5);

    // sum array2. output -> scan2, array3
    KA argsB[] = {KA::Buffer(array2), KA::Buffer(scan2), KA::Buffer(array3), KA::Scalar(numElem2), KA::Scalar(zon)};
    launch(m_kPrefixSum, numElem3, threads, argsB, 5);

    if (numElem3 > 1) {
        // sum array3. output -> scan3
        KA argsC[] = {KA::Buffer(array3), KA::Buffer(scan3), nullPtr, KA::Scalar(numElem3), KA::Scalar(zon)};
        launch(m_kPrefixSum, 1, threads, argsC, 5);

        // merge scan3 into scan2. output -> scan2
        KA argsD[] = {KA::Buffer(scan2), KA::Buffer(scan3), KA::Scalar(numElem2)};
        launch(m_kPrefixFixup, numElem3, threads, argsD, 3);
    }

    // merge scan2 into scan1. output -> scan1
    KA argsE[] = {KA::Buffer(scan1), KA::Buffer(scan2), KA::Scalar(numElem1)};
    launch(m_kPrefixFixup, numElem2, threads, argsE, 3);
}

void SPHSolver::countingSort() {
    if (!prepare()) return;
    // Transfer particle data to the temp buffers
    // (required by the algorithm; device-to-device copy, no sync needed)
    const uint64_t count = m_particles->getNumParticles();
    for (int n = 0; n < SPH_PARTICLE_BUFFERS; ++n) {
        const ParticleBuffer slot = ParticleBuffer(n);
        SPH_V_GP(m_queue->copyBufferRegion(m_particles->getTempBuffer(slot), 0, m_particles->getBuffer(slot), 0,
                                           count * particleBufferStride(slot)));
    }

    gp::KernelArg args[] = {gp::KernelArg::Scalar(int(m_particles->getNumParticles()))};
    launchParticles(m_kCountingSort, args, 1);
}

void SPHSolver::computePressure() {
    if (!prepare()) return;
    gp::KernelArg args[] = {gp::KernelArg::Scalar(int(m_particles->getNumParticles()))};
    launchParticles(m_kPressure, args, 1);
}

void SPHSolver::computeForce() {
    if (!prepare()) return;
    gp::KernelArg args[] = {gp::KernelArg::Scalar(int(m_particles->getNumParticles()))};
    launchParticles(m_kForce, args, 1);
}

void SPHSolver::advance() {
    if (!prepare()) return;
    using KA = gp::KernelArg;
    const SPHFluidParams &p = m_particles->getParams();
    // As the reference: the last argument is the capacity, not the count.
    KA args[] = {KA::Scalar(p.time), KA::Scalar(p.dt), KA::Scalar(p.sim_scale), KA::Scalar(p.pnum)};
    launchParticles(m_kAdvance, args, 4);
}

// Run + AdvanceTime (without the cuCtxSynchronize of the reference: the caller
// decides when to wait for the queue).
void SPHSolver::step() {
    insertParticles();
    prefixScanParticles();
    countingSort();
    computePressure();
    computeForce();
    advance();

    const SPHFluidParams &p = m_particles->getParams();
    setTime(p.time + p.dt);
    m_frame++;
}

nvrhi::FRESULT createSPHSolver(ISPHParticles *particles, vfs::IFileSystem *vfs, ISPHSolver **outSolver) {
    if (particles == nullptr || vfs == nullptr || outSolver == nullptr) return nvrhi::FE_INVALID_ARGS;
    if (!particles->getDevice() || !particles->getQueue()) return nvrhi::FE_INVALID_ARGS;
    *outSolver = nullptr;
    nvrhi::AutoPtr<SPHSolver> solver = MAKE_RC_OBJ_PTR(SPHSolver, particles);
    if (!solver) return nvrhi::FE_OUT_OF_MEMORY;
    nvrhi::FRESULT fr = solver->initialize(vfs);
    if (NVRHI_FAILED(fr)) return fr;
    *outSolver = solver.Detach();
    return nvrhi::FS_OK;
}

}  // namespace sph
