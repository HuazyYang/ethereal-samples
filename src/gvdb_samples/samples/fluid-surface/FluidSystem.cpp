//-----------------------------------------------------------------------------
// FLUIDS v.3 - SPH Fluid Simulator for CPU and GPU
// Copyright (C) 2012-2013. Rama Hoetzlein, http://fluids3.com
//
// NVIDIA (R) GVDB VOXELS
// Copyright 2017 NVIDIA Corporation
// SPDX-License-Identifier: Apache-2.0
//-----------------------------------------------------------------------------
// Host side of the fluid system (fluid_system.cpp of the reference, GPU path
// RUN_GPU_FULL only): parameter setup of the "wave pool" example, uniform
// grid, gp buffers, kernel launches. The CPU simulation paths, neighbour
// tables, recording / playback and the GL drawing of the reference are not
// ported.
#include "FluidSystem.h"
#include <sample-utils/SampleTypes.h>
#include <donut/core/log.h>
#include <nvrhi/core/datablob.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

using namespace donut;
using namespace SampleUtils;

namespace {

constexpr float PI_F = 3.141592f;

FluidFloat3 toF3(dm::float3 v) { return FluidFloat3{v.x, v.y, v.z}; }
FluidInt3 toI3(dm::int3 v) { return FluidInt3{v.x, v.y, v.z}; }

int iDivUp(int a, int b) { return (a % b != 0) ? (a / b + 1) : (a / b); }

void computeNumBlocks(int numPnts, int minThreads, int &numBlocks, int &numThreads) {
    numThreads = std::min(minThreads, numPnts);
    numBlocks = (numThreads == 0) ? 1 : iDivUp(numPnts, numThreads);
}

// Vector3DF::Random of the reference: uniform in [0, hi] per component with rand().
float randomRange(float lo, float hi) { return lo + float(rand()) / float(RAND_MAX) * (hi - lo); }

}  // namespace

FluidSystem::FluidSystem(gp::IDevice *device, gp::IDeviceQueue *queue) : m_device(device), m_queue(queue) {}

FluidSystem::~FluidSystem() {
    if (m_device && m_queue) syncQueue(m_device, m_queue);
}

bool FluidSystem::createBuffer(nvrhi::AutoPtr<gp::IBuffer> &buffer, size_t bytes) {
    buffer = nullptr;
    gp::BufferDesc desc;
    desc.byteSize = std::max<size_t>(bytes, 16);
    if (NVRHI_FAILED(m_device->createBuffer(desc, &buffer))) {
        log::error("FluidSystem: cannot allocate a buffer of %llu bytes", (unsigned long long)bytes);
        return false;
    }
    return true;
}

void FluidSystem::launch(gp::IKernel *kernel, int blocks, int threads, const gp::KernelArg *args, size_t argc) {
    UT_V_GP(m_queue->launch(kernel, gp::dim3{blocks, 1, 1}, gp::dim3{threads, 1, 1}, args, argc));
}

// ---------------------------------------------------------------------------
// Initialize: kernels
// ---------------------------------------------------------------------------

bool FluidSystem::initialize(vfs::IFileSystem *vfs) {
    nvrhi::AutoPtr<nvrhi::IDataBlob> ptx;
    if (!vfs || NVRHI_FAILED(vfs->readFile("ptx/FluidSystemKernels.ptx", &ptx)) || !ptx) {
        log::error("FluidSystem: cannot read ptx/FluidSystemKernels.ptx");
        return false;
    }
    size_t len = ptx->GetSize();
    ptx->Resize(len + 1);
    static_cast<char *>(ptx->GetDataPtr())[len] = 0;
    if (NVRHI_FAILED(m_device->createModule({}, ptx->GetDataPtr(), len + 1, &m_module))) {
        log::error("FluidSystem: cannot create the kernel module");
        return false;
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
        if (NVRHI_FAILED(m_module->getKernel(k.name, &(*k.kernel)))) {
            log::error("FluidSystem: kernel %s not found", k.name);
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Parameters (SetupDefaultParams / SetupExampleParams / SetupSpacing / SetupGrid)
// ---------------------------------------------------------------------------

void FluidSystem::setupDefaultParams() {
    m_time = 0.0f;
    m_dt = 0.003f;
    m_p.simScale = 0.005f;      // unit size
    m_p.visc = 0.50f;           // pascal-second (Pa.s)
    m_p.restDensity = 400.0f;   // kg / m^3
    m_p.spacing = 0.0f;         // 0 = computed from the density
    m_p.mass = 0.00020543f;     // kg
    m_p.radius = 0.015f;        // m
    m_p.dist = 0.0059f;         // m
    m_p.smoothRadius = 0.015f;  // m
    m_p.intStiff = 1.0f;
    m_p.extStiff = 50000.0f;
    m_p.extDamp = 100.0f;
    m_p.accelLimit = 150.0f;    // m / s^2
    m_p.velLimit = 3.0f;        // m / s
    m_p.grav = 1.0f;
    m_p.groundSlope = 0.0f;
    m_p.forceMin = 0.0f;
    m_p.forceMax = 0.0f;
    m_p.forceFreq = 16.0f;
    m_p.gridDensity = 2.0f;
    m_p.planeGravDir = {0.f, -9.8f, 0.f};
    m_p.gridSize = m_p.smoothRadius * 2.f;
}

// Example 2 of the reference: wave pool.
void FluidSystem::setupExampleParams() {
    m_p.volMin = {0.f, 0.f, 0.f};
    m_p.volMax = {400.f, 200.f, 400.f};
    m_p.initMin = {100.f, 80.f, 100.f};
    m_p.initMax = {300.f, 190.f, 300.f};
    m_p.forceMin = 100.0f;
    m_p.forceFreq = 6.0f;
    m_p.groundSlope = 0.10f;
}

void FluidSystem::setupSpacing() {
    if (m_p.spacing == 0.f) {
        // spacing from the density
        m_p.dist = powf(m_p.mass / m_p.restDensity, 1.f / 3.0f);
        m_p.spacing = m_p.dist * 0.87f / m_p.simScale;
    } else {
        // density from the spacing
        m_p.dist = m_p.spacing * m_p.simScale / 0.87f;
        m_p.restDensity = m_p.mass / powf(m_p.dist, 3.0f);
    }
    log::info("FluidSystem: Add Particles. Density: %f, Spacing: %f, PDist: %f", m_p.restDensity, m_p.spacing, m_p.dist);

    // particle boundaries
    m_p.boundMin = m_p.volMin + 2.0f * (m_p.gridSize / m_p.simScale);
    m_p.boundMax = m_p.volMax - 2.0f * (m_p.gridSize / m_p.simScale);
}

void FluidSystem::setupGrid(dm::float3 vmin, dm::float3 vmax, float simScale, float cellSize) {
    float worldCellSize = cellSize / simScale;

    m_gridMin = vmin;
    m_gridMax = vmax;
    m_gridSize = m_gridMax - m_gridMin;
    m_gridRes.x = int(ceilf(m_gridSize.x / worldCellSize));   // grid resolution
    m_gridRes.y = int(ceilf(m_gridSize.y / worldCellSize));
    m_gridRes.z = int(ceilf(m_gridSize.z / worldCellSize));
    m_gridSize.x = float(m_gridRes.x) * cellSize / simScale;   // grid size as a multiple of the cell size
    m_gridSize.y = float(m_gridRes.y) * cellSize / simScale;
    m_gridSize.z = float(m_gridRes.z) * cellSize / simScale;
    m_gridDelta = dm::float3(m_gridRes) / m_gridSize;         // world space -> cell number
    m_gridTotal = m_gridRes.x * m_gridRes.y * m_gridRes.z;

    // cells to search: n = (2r / w) + 1 (r = search radius, w = world cell width)
    m_gridSrch = int(floorf(2.0f * (m_p.smoothRadius / simScale) / worldCellSize) + 1.0f);
    if (m_gridSrch < 2) m_gridSrch = 2;
    m_gridAdjCnt = m_gridSrch * m_gridSrch * m_gridSrch;   // 2x2x2 = 8, 3x3x3 = 27, 4x4x4 = 64
    if (m_gridSrch > 4) {
        log::error("FluidSystem: neighbour search is n > 4 (%d); the adjacency table holds 64 cells.", m_gridSrch);
        m_gridSrch = 4;
        m_gridAdjCnt = 64;
    }
}

// FluidSetupCUDA + FluidParamCUDA of the reference: the FluidParams block.
void FluidSystem::setupFluidParams() {
    FluidParams &f = m_params;
    memset(&f, 0, sizeof(f));
    f.pnum = m_maxPoints;
    f.gridRes = toI3(m_gridRes);
    f.gridSize = toF3(m_gridSize);
    f.gridDelta = toF3(m_gridDelta);
    f.gridMin = toF3(m_gridMin);
    f.gridMax = toF3(m_gridMax);
    f.gridTotal = m_gridTotal;
    f.gridSrch = m_gridSrch;
    f.gridAdjCnt = m_gridSrch * m_gridSrch * m_gridSrch;
    f.gridScanMax = toI3(m_gridRes - dm::int3(m_gridSrch));

    // adjacency lookup
    int cell = 0;
    for (int y = 0; y < m_gridSrch; y++)
        for (int z = 0; z < m_gridSrch; z++)
            for (int x = 0; x < m_gridSrch; x++) f.gridAdj[cell++] = (y * m_gridRes.z + z) * m_gridRes.x + x;

    // launch configuration
    computeNumBlocks(f.pnum, FLUID_THREADS, f.numBlocks, f.numThreads);           // particles
    computeNumBlocks(f.gridTotal, FLUID_THREADS, f.gridBlocks, f.gridThreads);    // grid cells
    f.szPnts = f.numBlocks * f.numThreads;
    f.szGrid = f.gridBlocks * f.gridThreads;
    log::info("FluidSystem: CUDA Config:");
    log::info("  Pnts: %d, t:%dx%d=%d, Size:%d", f.pnum, f.numBlocks, f.numThreads, f.numBlocks * f.numThreads, f.szPnts);
    log::info("  Grid: %d, t:%dx%d=%d, bufGrid:%d, Res: %dx%dx%d", f.gridTotal, f.gridBlocks, f.gridThreads,
              f.gridBlocks * f.gridThreads, f.szGrid, m_gridRes.x, m_gridRes.y, m_gridRes.z);

    // simulation parameters (UpdateParams -> FluidParamCUDA)
    const float sr = m_p.smoothRadius;
    dm::float3 grav = m_p.planeGravDir * m_p.grav;
    f.psimscale = m_p.simScale;
    f.psmoothradius = sr;
    f.pradius = m_p.radius;
    f.r2 = sr * sr;
    f.pmass = m_p.mass;
    f.prest_dens = m_p.restDensity;
    f.pboundmin = toF3(m_p.boundMin);
    f.pboundmax = toF3(m_p.boundMax);
    f.pextstiff = m_p.extStiff;
    f.pintstiff = m_p.intStiff;
    f.pvisc = m_p.visc;
    f.pdamp = m_p.extDamp;
    f.pforce_min = m_p.forceMin;
    f.pforce_max = m_p.forceMax;
    f.pforce_freq = m_p.forceFreq;
    f.pground_slope = m_p.groundSlope;
    f.pgravity = toF3(grav);
    f.AL = m_p.accelLimit;
    f.AL2 = f.AL * f.AL;
    f.VL = m_p.velLimit;
    f.VL2 = f.VL * f.VL;
    f.pvel_limit = m_p.velLimit;
    f.paccel_limit = m_p.accelLimit;

    f.pdist = powf(f.pmass / f.prest_dens, 1.f / 3.0f);
    f.poly6kern = 315.0f / (64.0f * PI_F * powf(sr, 9.0f));   // Wpoly6 kernel (denominator part) - 2003 Muller, p.4
    f.spikykern = -45.0f / (PI_F * powf(sr, 6.0f));            // Laplacian of viscosity (denominator): PI h^6
    f.lapkern = 45.0f / (PI_F * powf(sr, 6.0f));
    f.gausskern = 1.0f / powf(PI_F * 2.0f * sr * sr, 3.0f / 2.0f);

    f.d2 = f.psimscale * f.psimscale;
    f.rd2 = f.r2 / f.d2;
    f.vterm = f.lapkern * f.pvisc;
}

// ---------------------------------------------------------------------------
// Buffers (AllocateParticles / AllocateGrid)
// ---------------------------------------------------------------------------

bool FluidSystem::allocateBuffers() {
    const FluidParams &f = m_params;
    struct {
        FluidBuf id;
        size_t stride;
        bool dual;   // fbuf + ftemp
    } bufs[] = {
        {FPOS, sizeof(float) * 3, true},  {FVEL, sizeof(float) * 3, true},     {FVEVAL, sizeof(float) * 3, true},
        {FFORCE, sizeof(float) * 3, true}, {FPRESS, sizeof(float), true},       {FDENSITY, sizeof(float), true},
        {FCLR, sizeof(uint32_t), true},   {FGCELL, sizeof(uint32_t), true},    {FGNDX, sizeof(uint32_t), true},
        {FGRID, sizeof(uint32_t), false},
    };
    for (auto &b : bufs) {
        if (!createBuffer(m_buf[b.id], b.stride * f.szPnts)) return false;
        UT_V_GP(m_queue->clearBufferUint(m_buf[b.id], 0));
        if (b.dual) {
            if (!createBuffer(m_temp[b.id], b.stride * f.szPnts)) return false;
            UT_V_GP(m_queue->clearBufferUint(m_temp[b.id], 0));
        } else {
            m_temp[b.id] = nullptr;
        }
    }
    if (!createBuffer(m_buf[FGRIDCNT], sizeof(uint32_t) * f.szGrid)) return false;
    if (!createBuffer(m_buf[FGRIDOFF], sizeof(uint32_t) * f.szGrid)) return false;
    UT_V_GP(m_queue->clearBufferUint(m_buf[FGRIDCNT], 0));
    UT_V_GP(m_queue->clearBufferUint(m_buf[FGRIDOFF], 0));

    // auxiliary arrays of the three-level prefix sum
    const int blockSize = FLUID_SCAN_BLOCKSIZE << 1;
    const int numElem1 = m_gridTotal;
    const int numElem2 = numElem1 / blockSize + 1;
    const int numElem3 = numElem2 / blockSize + 1;
    if (!createBuffer(m_buf[FAUXARRAY1], sizeof(uint32_t) * numElem2)) return false;
    if (!createBuffer(m_buf[FAUXSCAN1], sizeof(uint32_t) * numElem2)) return false;
    if (!createBuffer(m_buf[FAUXARRAY2], sizeof(uint32_t) * numElem3)) return false;
    if (!createBuffer(m_buf[FAUXSCAN2], sizeof(uint32_t) * numElem3)) return false;
    m_stagingPos = m_stagingVel = m_stagingClr = nullptr;
    return true;
}

bool FluidSystem::uploadConstants() {
    FluidBufs fb = {}, ft = {};
    for (int n = 0; n < FLUID_NUM_BUF; ++n) {
        fb.ptr[n] = m_buf[n] ? (unsigned long long)m_buf[n]->getNativeHandle() : 0ull;
        ft.ptr[n] = m_temp[n] ? (unsigned long long)m_temp[n]->getNativeHandle() : fb.ptr[n];
    }
    // The symbols belong to the module; any of its kernels addresses them.
    nvrhi::FRESULT fr = m_queue->setConstantBuffer(m_kInsert, "fbuf", &fb, sizeof(fb));
    if (!NVRHI_FAILED(fr)) fr = m_queue->setConstantBuffer(m_kInsert, "ftemp", &ft, sizeof(ft));
    if (!NVRHI_FAILED(fr)) fr = m_queue->setConstantBuffer(m_kInsert, "fparam", &m_params, sizeof(m_params));
    if (NVRHI_FAILED(fr)) {
        log::error("FluidSystem: cannot upload the constant tables (%d)", int(fr));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Initial particles (SetupAddVolume)
// ---------------------------------------------------------------------------

void FluidSystem::setupAddVolume(dm::float3 vmin, dm::float3 vmax, float spacing, float offs, int total) {
    const int cntx = int(ceilf((vmax.x - vmin.x - offs) / spacing));
    const int cntz = int(ceilf((vmax.z - vmin.z - offs) / spacing));
    const int cnt = cntx * cntz;

    vmin += offs;
    vmax -= offs;
    const float dx = vmax.x - vmin.x;
    const float dz = vmax.z - vmin.z;

    m_hostPos.clear();
    m_hostClr.clear();
    m_hostPos.reserve(total);
    m_hostClr.reserve(total);
    dm::float3 pos;
    for (pos.y = vmin.y; pos.y <= vmax.y; pos.y += spacing) {
        for (int xz = 0; xz < cnt; xz++) {
            if (int(m_hostPos.size()) >= total) break;   // AddParticle() == -1
            pos.x = vmin.x + float(xz % cntx) * spacing;
            pos.z = vmin.z + float(xz / cntx) * spacing;
            dm::float3 rnd(randomRange(0.f, spacing), randomRange(0.f, spacing), randomRange(0.f, spacing));
            m_hostPos.push_back(pos + rnd);

            dm::float3 clr((pos.x - vmin.x) / dx, 0.f, (pos.z - vmin.z) / dz);
            clr = dm::clamp(clr * 0.8f + 0.2f, dm::float3(0.f), dm::float3(1.f));
            m_hostClr.push_back(packColorA(clr.x, clr.y, clr.z, 1.f));
        }
    }
    m_numPoints = int(m_hostPos.size());
}

// ---------------------------------------------------------------------------
// Start
// ---------------------------------------------------------------------------

bool FluidSystem::start(int numParticles) {
    if (!m_module) {
        log::error("FluidSystem: initialize() first");
        return false;
    }
    m_time = 0.f;
    m_frame = 0;
    m_numPoints = 0;
    m_maxPoints = numParticles;

    setupDefaultParams();
    setupExampleParams();
    m_p.gridSize = 2.f * m_p.smoothRadius / m_p.gridDensity;
    setupSpacing();
    setupGrid(m_p.volMin, m_p.volMax, m_p.simScale, m_p.gridSize);
    setupFluidParams();

    if (!allocateBuffers()) return false;

    // the initial block of particles (increases m_numPoints)
    setupAddVolume(m_p.initMin, m_p.initMax, m_p.spacing, 0.1f, m_maxPoints);
    log::info("FluidSystem: %d particles", m_numPoints);

    // TransferToCUDA: the other buffers start at zero
    UT_V_GP(m_queue->writeBuffer(m_buf[FPOS], m_hostPos.data(), m_hostPos.size() * sizeof(dm::float3), 0));
    UT_V_GP(m_queue->writeBuffer(m_buf[FCLR], m_hostClr.data(), m_hostClr.size() * sizeof(uint32_t), 0));
    return uploadConstants();
}

// ---------------------------------------------------------------------------
// Simulation step (Run, RUN_GPU_FULL)
// ---------------------------------------------------------------------------

void FluidSystem::run() {
    if (m_numPoints == 0) return;
    insertParticles();      // insert into the grid
    prefixSumCells();       // grid offsets
    countingSortFull();     // deep-copy sort
    computePressure();
    computeForce();
    advance();
    m_time += m_dt;
    m_frame++;
}

void FluidSystem::insertParticles() {
    UT_V_GP(m_queue->clearBufferUint(m_buf[FGRIDCNT], 0));
    UT_V_GP(m_queue->clearBufferUint(m_buf[FGRIDOFF], 0));
    gp::KernelArg args[] = {gp::KernelArg::Scalar(m_numPoints)};
    launch(m_kInsert, m_params.numBlocks, m_params.numThreads, args, 1);
}

void FluidSystem::prefixSumCells() {
    using KA = gp::KernelArg;
    const int blockSize = FLUID_SCAN_BLOCKSIZE << 1;
    const int numElem1 = m_gridTotal;
    const int numElem2 = numElem1 / blockSize + 1;
    const int numElem3 = numElem2 / blockSize + 1;
    const int threads = FLUID_SCAN_BLOCKSIZE;
    const int zon = 1;
    const int zeroOffsets = 1;
    const KA nullPtr = KA::Scalar(uint64_t(0));

    gp::IBuffer *array1 = m_buf[FGRIDCNT];   // input
    gp::IBuffer *scan1 = m_buf[FGRIDOFF];    // output
    gp::IBuffer *array2 = m_buf[FAUXARRAY1];
    gp::IBuffer *scan2 = m_buf[FAUXSCAN1];
    gp::IBuffer *array3 = m_buf[FAUXARRAY2];
    gp::IBuffer *scan3 = m_buf[FAUXSCAN2];

    // sum array1 -> scan1, array2
    KA argsA[] = {KA::Buffer(array1), KA::Buffer(scan1), KA::Buffer(array2), KA::Scalar(numElem1), KA::Scalar(zeroOffsets)};
    launch(m_kPrefixSum, numElem2, threads, argsA, 5);
    // sum array2 -> scan2, array3
    KA argsB[] = {KA::Buffer(array2), KA::Buffer(scan2), KA::Buffer(array3), KA::Scalar(numElem2), KA::Scalar(zon)};
    launch(m_kPrefixSum, numElem3, threads, argsB, 5);
    if (numElem3 > 1) {
        // sum array3 -> scan3
        KA argsC[] = {KA::Buffer(array3), KA::Buffer(scan3), nullPtr, KA::Scalar(numElem3), KA::Scalar(zon)};
        launch(m_kPrefixSum, 1, threads, argsC, 5);
        // merge scan3 into scan2
        KA argsD[] = {KA::Buffer(scan2), KA::Buffer(scan3), KA::Scalar(numElem2)};
        launch(m_kPrefixFixup, numElem3, threads, argsD, 3);
    }
    // merge scan2 into scan1
    KA argsE[] = {KA::Buffer(scan1), KA::Buffer(scan2), KA::Scalar(numElem1)};
    launch(m_kPrefixFixup, numElem2, threads, argsE, 3);
}

void FluidSystem::countingSortFull() {
    // particle data to the temp buffers (device to device)
    struct {
        FluidBuf id;
        size_t stride;
    } copies[] = {{FPOS, 12}, {FVEL, 12}, {FVEVAL, 12}, {FFORCE, 12}, {FPRESS, 4},
                  {FDENSITY, 4}, {FCLR, 4}, {FGCELL, 4}, {FGNDX, 4}};
    for (auto &c : copies)
        UT_V_GP(m_queue->copyBufferRegion(m_temp[c.id], 0, m_buf[c.id], 0, c.stride * size_t(m_numPoints)));

    gp::KernelArg args[] = {gp::KernelArg::Scalar(m_numPoints)};
    launch(m_kCountingSort, m_params.numBlocks, m_params.numThreads, args, 1);
}

void FluidSystem::computePressure() {
    gp::KernelArg args[] = {gp::KernelArg::Scalar(m_numPoints)};
    launch(m_kPressure, m_params.numBlocks, m_params.numThreads, args, 1);
}

void FluidSystem::computeForce() {
    gp::KernelArg args[] = {gp::KernelArg::Scalar(m_params.pnum)};
    launch(m_kForce, m_params.numBlocks, m_params.numThreads, args, 1);
}

void FluidSystem::advance() {
    using KA = gp::KernelArg;
    KA args[] = {KA::Scalar(m_time), KA::Scalar(m_dt), KA::Scalar(m_p.simScale), KA::Scalar(m_params.pnum)};
    launch(m_kAdvance, m_params.numBlocks, m_params.numThreads, args, 4);
}

// ---------------------------------------------------------------------------
// Readback
// ---------------------------------------------------------------------------

void FluidSystem::readback(std::vector<dm::float3> &pos, std::vector<dm::float3> &vel, std::vector<uint32_t> &clr) {
    pos.resize(m_numPoints);
    vel.resize(m_numPoints);
    clr.resize(m_numPoints);
    if (m_numPoints == 0) return;
    const size_t bytes3 = sizeof(dm::float3) * m_numPoints;
    const size_t bytes1 = sizeof(uint32_t) * m_numPoints;
    auto staging = [&](nvrhi::AutoPtr<gp::IBuffer> &buffer, size_t bytes) {
        if (buffer && buffer->getDesc()->byteSize >= bytes) return true;
        gp::BufferDesc desc;
        desc.byteSize = bytes;
        desc.isStaging = true;
        return !NVRHI_FAILED(m_device->createBuffer(desc, &buffer));
    };
    if (!staging(m_stagingPos, bytes3) || !staging(m_stagingVel, bytes3) || !staging(m_stagingClr, bytes1)) return;
    UT_V_GP(m_queue->copyBufferRegion(m_stagingPos, 0, m_buf[FPOS], 0, bytes3));
    UT_V_GP(m_queue->copyBufferRegion(m_stagingVel, 0, m_buf[FVEL], 0, bytes3));
    UT_V_GP(m_queue->copyBufferRegion(m_stagingClr, 0, m_buf[FCLR], 0, bytes1));
    syncQueue(m_device, m_queue);

    auto fetch = [&](gp::IBuffer *buffer, void *dst, size_t bytes) {
        void *data = nullptr;
        UT_V_GP(m_device->mapBuffer(buffer, &data));
        if (data) memcpy(dst, data, bytes);
        m_device->unmapBuffer(buffer);
    };
    fetch(m_stagingPos, pos.data(), bytes3);
    fetch(m_stagingVel, vel.data(), bytes3);
    fetch(m_stagingClr, clr.data(), bytes1);
}
