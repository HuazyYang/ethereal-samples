// SPH solver: the operation half of the fluids5.0 Particles class (Run,
// InsertParticles, PrefixScanParticles, CountingSort, ComputePressure,
// ComputeForce, Advance; the GPU path). Works on an ISPHParticles through its
// public interface only.
#include "SPHFluid.h"
#include <nvrhi/core/foundation.h>
#include <donut/core/log.h>

namespace sph {

using namespace donut;

class SPHSolver : public nvrhi::ObjectImpl<ISPHSolver> {
 public:
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(SPHSolver)
    NVRHI_IMPLEMENTS_INTERFACE(ISPHSolver)
    NVRHI_END_INTERFACE_TABLE()

    explicit SPHSolver(ISPHParticles *particles) : m_particles(particles), m_context(particles->getContext()) {}

    // Initialize / LoadKernel of the reference.
    nvrhi::FRESULT initialize();

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
        m_particles->setTime(0.f);
    }

 private:
    // Brings the tables of device pointers the kernels index up to date with
    // the particle set (when buffers were reallocated). Returns false when
    // there is nothing to run.
    bool prepare();
    nvrhi::ICommandList *commandList() { return m_context->getCommandList(); }
    // Thread groups covering the capacity (the kernels return for indices past their count argument), as the
    // reference's numBlocks x numThreads.
    dm::uint3 particleGroups(gvdb::IComputeKernel *kernel) {
        return gvdb::groupsFor(kernel, uint64_t(m_particles->getParams().pnum));
    }

    nvrhi::AutoPtr<ISPHParticles> m_particles;
    gvdb::IComputeContext *m_context;   // the particle set's (kept alive by it)
    gvdb::IComputeKernel *m_kInsert = nullptr, *m_kCountingSort = nullptr, *m_kPressure = nullptr,
                         *m_kForce = nullptr, *m_kAdvance = nullptr, *m_kPrefixSum = nullptr,
                         *m_kPrefixFixup = nullptr;
    SPHBufferTable m_points = {}, m_temp = {}, m_accel = {};   // FPnts, FPntTmp, FAccel
    uint64_t m_layoutRevision = 0;   // revision of the particle set the tables were built for
    uint32_t m_frame = 0;
};

nvrhi::FRESULT SPHSolver::initialize() {
    struct {
        const char *name;
        gvdb::IComputeKernel **kernel;
    } kernels[] = {
        {"insertParticles", &m_kInsert}, {"countingSortFull", &m_kCountingSort}, {"computePressure", &m_kPressure},
        {"computeForce", &m_kForce},     {"advanceParticles", &m_kAdvance},      {"prefixSum", &m_kPrefixSum},
        {"prefixFixup", &m_kPrefixFixup},
    };
    for (auto &k : kernels) {
        *k.kernel = m_context->getKernel(SPH_KERNELS, k.name);
        if (!*k.kernel) return nvrhi::FE_NOT_FOUND;   // getKernel() reported why
    }
    return nvrhi::FS_OK;
}

bool SPHSolver::prepare() {
    if (m_particles->getNumParticles() == 0 || m_particles->getMaxParticles() == 0) return false;
    if (!m_particles->getGridBuffer(GridBuffer::Grid) || !m_particles->getBuffer(ParticleBuffer::Position)) return false;

    if (m_layoutRevision != m_particles->getLayoutRevision()) {
        // UpdateGPUAccess: the tables of device pointers the kernels index.
        m_points = {};
        m_temp = {};
        m_accel = {};
        for (int n = 0; n < SPH_PARTICLE_BUFFERS; ++n) {
            m_points.buf[n] = gvdb::deviceAddress(m_particles->getBuffer(ParticleBuffer(n)));
            m_temp.buf[n] = gvdb::deviceAddress(m_particles->getTempBuffer(ParticleBuffer(n)));
        }
        for (int n = 0; n < SPH_GRID_BUFFERS; ++n)
            m_accel.buf[n] = gvdb::deviceAddress(m_particles->getGridBuffer(GridBuffer(n)));
        m_layoutRevision = m_particles->getLayoutRevision();
    }
    return true;
}

void SPHSolver::insertParticles() {
    if (!prepare()) return;
    // Reset all grid cells to empty
    nvrhi::ICommandList *cmd = commandList();
    cmd->clearBufferUInt(m_particles->getGridBuffer(GridBuffer::GridCount), 0u);
    cmd->clearBufferUInt(m_particles->getGridBuffer(GridBuffer::GridOffset), 0u);
    cmd->clearBufferUInt(m_particles->getBuffer(ParticleBuffer::GridCell), 0u);
    cmd->clearBufferUInt(m_particles->getBuffer(ParticleBuffer::GridIndex), 0u);

    gvdb::launchKernel(cmd, m_kInsert, particleGroups(m_kInsert), m_particles->getParams(), m_points, m_accel,
                       int(m_particles->getNumParticles()));
}

void SPHSolver::prefixScanParticles() {
    if (!prepare()) return;

    // Prefix Sum - determine grid offsets
    const int blockSize = SPH_SCAN_BLOCKSIZE << 1;
    const int numElem1 = m_particles->getParams().gridTotal;
    const int numElem2 = int(numElem1 / blockSize) + 1;
    const int numElem3 = int(numElem2 / blockSize) + 1;
    const int zero_offsets = 1;
    const int zon = 1;
    const uint64_t nullPtr = 0;   // a null uint* argument
    auto groups = [](int n) { return dm::uint3(uint32_t(n), 1u, 1u); };

    const uint64_t array1 = m_accel.buf[SPH_AGRIDCNT];   // input
    const uint64_t scan1 = m_accel.buf[SPH_AGRIDOFF];    // output
    const uint64_t array2 = m_accel.buf[SPH_AAUXARRAY1];
    const uint64_t scan2 = m_accel.buf[SPH_AAUXSCAN1];
    const uint64_t array3 = m_accel.buf[SPH_AAUXARRAY2];
    const uint64_t scan3 = m_accel.buf[SPH_AAUXSCAN2];

    nvrhi::ICommandList *cmd = commandList();
    // sum array1. output -> scan1, array2
    gvdb::launchKernel(cmd, m_kPrefixSum, groups(numElem2), array1, scan1, array2, numElem1, zero_offsets);

    // sum array2. output -> scan2, array3
    gvdb::launchKernel(cmd, m_kPrefixSum, groups(numElem3), array2, scan2, array3, numElem2, zon);

    if (numElem3 > 1) {
        // sum array3. output -> scan3
        gvdb::launchKernel(cmd, m_kPrefixSum, groups(1), array3, scan3, nullPtr, numElem3, zon);

        // merge scan3 into scan2. output -> scan2
        gvdb::launchKernel(cmd, m_kPrefixFixup, groups(numElem3), scan2, scan3, numElem2);
    }

    // merge scan2 into scan1. output -> scan1
    gvdb::launchKernel(cmd, m_kPrefixFixup, groups(numElem2), scan1, scan2, numElem1);
}

void SPHSolver::countingSort() {
    if (!prepare()) return;
    // Transfer particle data to the temp buffers
    // (required by the algorithm; device-to-device copy, no sync needed)
    nvrhi::ICommandList *cmd = commandList();
    const uint64_t count = m_particles->getNumParticles();
    for (int n = 0; n < SPH_PARTICLE_BUFFERS; ++n) {
        const ParticleBuffer slot = ParticleBuffer(n);
        cmd->copyBuffer(m_particles->getTempBuffer(slot), 0, m_particles->getBuffer(slot), 0,
                        count * particleBufferStride(slot));
    }

    gvdb::launchKernel(cmd, m_kCountingSort, particleGroups(m_kCountingSort), m_particles->getParams(), m_points, m_temp,
                       m_accel, int(m_particles->getNumParticles()));
}

void SPHSolver::computePressure() {
    if (!prepare()) return;
    gvdb::launchKernel(commandList(), m_kPressure, particleGroups(m_kPressure), m_particles->getParams(), m_points,
                       m_accel, int(m_particles->getNumParticles()));
}

void SPHSolver::computeForce() {
    if (!prepare()) return;
    gvdb::launchKernel(commandList(), m_kForce, particleGroups(m_kForce), m_particles->getParams(), m_points, m_accel,
                       int(m_particles->getNumParticles()));
}

void SPHSolver::advance() {
    if (!prepare()) return;
    const SPHFluidParams &p = m_particles->getParams();
    // As the reference: the last argument is the capacity, not the count.
    gvdb::launchKernel(commandList(), m_kAdvance, particleGroups(m_kAdvance), p, m_points, p.time, p.dt, p.sim_scale,
                       p.pnum);
}

// Run + AdvanceTime (without the cuCtxSynchronize of the reference: the caller
// decides when to wait).
void SPHSolver::step() {
    insertParticles();
    prefixScanParticles();
    countingSort();
    computePressure();
    computeForce();
    advance();

    const SPHFluidParams &p = m_particles->getParams();
    m_particles->setTime(p.time + p.dt);
    m_frame++;
}

nvrhi::FRESULT createSPHSolver(ISPHParticles *particles, ISPHSolver **outSolver) {
    if (particles == nullptr || outSolver == nullptr || !particles->getContext()) return nvrhi::FE_INVALID_ARGS;
    *outSolver = nullptr;
    nvrhi::AutoPtr<SPHSolver> solver = MAKE_RC_OBJ_PTR(SPHSolver, particles);
    if (!solver) return nvrhi::FE_OUT_OF_MEMORY;
    nvrhi::FRESULT fr = solver->initialize();
    if (NVRHI_FAILED(fr)) return fr;
    *outSolver = solver.Detach();
    return nvrhi::FS_OK;
}

}  // namespace sph
