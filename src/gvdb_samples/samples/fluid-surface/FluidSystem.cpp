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
// grid, buffers, kernel launches. The CPU simulation paths, neighbour
// tables, recording / playback and the GL drawing of the reference are not
// ported.
#include "FluidSystem.h"
#include <sample-utils/SampleTypes.h>
#include <donut/core/log.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

using namespace donut;
using namespace SampleUtils;

namespace {

constexpr float PI_F = 3.141592f;
constexpr const char *c_Kernels = "shaders/gvdb/FluidSystemKernels.nvsc";

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

FluidSystem::FluidSystem(gvdb::IComputeContext *context) : m_context(context) {}

FluidSystem::~FluidSystem() {
    // the kernels reach the buffers through device addresses
    if (m_context) m_context->finish();
}

nvrhi::BufferHandle FluidSystem::createBuffer(size_t bytes, const char *name) {
    nvrhi::BufferDesc desc;
    desc.byteSize = std::max<size_t>(bytes, 16);
    desc.debugName = name;
    nvrhi::BufferHandle buffer;
    if (NVRHI_FAILED(m_context->getDevice()->createBuffer(desc, &buffer)))
        log::error("FluidSystem: cannot allocate a buffer of %llu bytes", (unsigned long long)bytes);
    return buffer;
}

// ---------------------------------------------------------------------------
// Initialize: kernels
// ---------------------------------------------------------------------------

bool FluidSystem::initialize() {
    struct {
        const char *name;
        gvdb::IComputeKernel **kernel;
    } kernels[] = {
        {"insertParticles", &m_kInsert}, {"countingSortFull", &m_kCountingSort}, {"computePressure", &m_kPressure},
        {"computeForce", &m_kForce},     {"advanceParticles", &m_kAdvance},      {"prefixSum", &m_kPrefixSum},
        {"prefixFixup", &m_kPrefixFixup},
    };
    for (auto &k : kernels) {
        *k.kernel = m_context->getKernel(c_Kernels, k.name);
        if (!*k.kernel) return false;   // getKernel() reported why
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
    // A restart replaces buffers that recorded work may still use.
    for (int n = 0; n < FLUID_NUM_BUF; ++n) {
        m_context->releaseAfterUse(m_buf[n]);
        m_context->releaseAfterUse(m_temp[n]);
        m_buf[n] = nullptr;
        m_temp[n] = nullptr;
    }
    nvrhi::ICommandList *cmd = commandList();
    for (auto &b : bufs) {
        m_buf[b.id] = createBuffer(b.stride * f.szPnts, "FluidSystem particles");
        if (!m_buf[b.id]) return false;
        cmd->clearBufferUInt(m_buf[b.id], 0);
        if (b.dual) {
            m_temp[b.id] = createBuffer(b.stride * f.szPnts, "FluidSystem particles (unsorted)");
            if (!m_temp[b.id]) return false;
            cmd->clearBufferUInt(m_temp[b.id], 0);
        }
    }
    m_buf[FGRIDCNT] = createBuffer(sizeof(uint32_t) * f.szGrid, "FluidSystem grid counts");
    if (!m_buf[FGRIDCNT]) return false;
    m_buf[FGRIDOFF] = createBuffer(sizeof(uint32_t) * f.szGrid, "FluidSystem grid offsets");
    if (!m_buf[FGRIDOFF]) return false;
    cmd->clearBufferUInt(m_buf[FGRIDCNT], 0);
    cmd->clearBufferUInt(m_buf[FGRIDOFF], 0);

    // auxiliary arrays of the three-level prefix sum
    const int blockSize = FLUID_SCAN_BLOCKSIZE << 1;
    const int numElem1 = m_gridTotal;
    const int numElem2 = numElem1 / blockSize + 1;
    const int numElem3 = numElem2 / blockSize + 1;
    m_buf[FAUXARRAY1] = createBuffer(sizeof(uint32_t) * numElem2, "FluidSystem scan");
    if (!m_buf[FAUXARRAY1]) return false;
    m_buf[FAUXSCAN1] = createBuffer(sizeof(uint32_t) * numElem2, "FluidSystem scan");
    if (!m_buf[FAUXSCAN1]) return false;
    m_buf[FAUXARRAY2] = createBuffer(sizeof(uint32_t) * numElem3, "FluidSystem scan");
    if (!m_buf[FAUXARRAY2]) return false;
    m_buf[FAUXSCAN2] = createBuffer(sizeof(uint32_t) * numElem3, "FluidSystem scan");
    if (!m_buf[FAUXSCAN2]) return false;
    return true;
}

void FluidSystem::updateBufferTables() {
    for (int n = 0; n < FLUID_NUM_BUF; ++n) {
        m_fbuf.ptr[n] = gvdb::deviceAddress(m_buf[n]);
        m_ftemp.ptr[n] = m_temp[n] ? gvdb::deviceAddress(m_temp[n]) : m_fbuf.ptr[n];
    }
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
    if (!m_kInsert) {
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
    commandList()->writeBuffer(m_buf[FPOS], m_hostPos.data(), m_hostPos.size() * sizeof(dm::float3), 0);
    commandList()->writeBuffer(m_buf[FCLR], m_hostClr.data(), m_hostClr.size() * sizeof(uint32_t), 0);
    updateBufferTables();
    return true;
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
    commandList()->clearBufferUInt(m_buf[FGRIDCNT], 0);
    commandList()->clearBufferUInt(m_buf[FGRIDOFF], 0);
    gvdb::launchKernel(commandList(), m_kInsert, gvdb::groupsFor(m_kInsert, uint64_t(m_numPoints)), m_params, m_fbuf,
                       m_numPoints);
}

void FluidSystem::prefixSumCells() {
    const int blockSize = FLUID_SCAN_BLOCKSIZE << 1;
    const int numElem1 = m_gridTotal;
    const int numElem2 = numElem1 / blockSize + 1;
    const int numElem3 = numElem2 / blockSize + 1;
    const int zon = 1;
    const int zeroOffsets = 1;
    const uint64_t nullPtr = 0;
    auto groups = [](int n) { return dm::uint3(uint32_t(n), 1u, 1u); };

    const uint64_t array1 = gvdb::deviceAddress(m_buf[FGRIDCNT]);   // input
    const uint64_t scan1 = gvdb::deviceAddress(m_buf[FGRIDOFF]);    // output
    const uint64_t array2 = gvdb::deviceAddress(m_buf[FAUXARRAY1]);
    const uint64_t scan2 = gvdb::deviceAddress(m_buf[FAUXSCAN1]);
    const uint64_t array3 = gvdb::deviceAddress(m_buf[FAUXARRAY2]);
    const uint64_t scan3 = gvdb::deviceAddress(m_buf[FAUXSCAN2]);

    nvrhi::ICommandList *cmd = commandList();
    // sum array1 -> scan1, array2
    gvdb::launchKernel(cmd, m_kPrefixSum, groups(numElem2), array1, scan1, array2, numElem1, zeroOffsets);
    // sum array2 -> scan2, array3
    gvdb::launchKernel(cmd, m_kPrefixSum, groups(numElem3), array2, scan2, array3, numElem2, zon);
    if (numElem3 > 1) {
        // sum array3 -> scan3
        gvdb::launchKernel(cmd, m_kPrefixSum, groups(1), array3, scan3, nullPtr, numElem3, zon);
        // merge scan3 into scan2
        gvdb::launchKernel(cmd, m_kPrefixFixup, groups(numElem3), scan2, scan3, numElem2);
    }
    // merge scan2 into scan1
    gvdb::launchKernel(cmd, m_kPrefixFixup, groups(numElem2), scan1, scan2, numElem1);
}

void FluidSystem::countingSortFull() {
    // particle data to the temp buffers (device to device)
    struct {
        FluidBuf id;
        size_t stride;
    } copies[] = {{FPOS, 12}, {FVEL, 12}, {FVEVAL, 12}, {FFORCE, 12}, {FPRESS, 4},
                  {FDENSITY, 4}, {FCLR, 4}, {FGCELL, 4}, {FGNDX, 4}};
    for (auto &c : copies)
        commandList()->copyBuffer(m_temp[c.id], 0, m_buf[c.id], 0, c.stride * size_t(m_numPoints));

    gvdb::launchKernel(commandList(), m_kCountingSort, gvdb::groupsFor(m_kCountingSort, uint64_t(m_numPoints)),
                       m_params, m_fbuf, m_ftemp, m_numPoints);
}

void FluidSystem::computePressure() {
    gvdb::launchKernel(commandList(), m_kPressure, gvdb::groupsFor(m_kPressure, uint64_t(m_numPoints)), m_params,
                       m_fbuf, m_numPoints);
}

void FluidSystem::computeForce() {
    gvdb::launchKernel(commandList(), m_kForce, gvdb::groupsFor(m_kForce, uint64_t(m_params.pnum)), m_params, m_fbuf,
                       m_params.pnum);
}

void FluidSystem::advance() {
    gvdb::launchKernel(commandList(), m_kAdvance, gvdb::groupsFor(m_kAdvance, uint64_t(m_params.pnum)), m_params,
                       m_fbuf, m_time, m_dt, m_p.simScale, m_params.pnum);
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
    m_context->readBuffer(m_buf[FPOS], 0, pos.data(), bytes3);
    m_context->readBuffer(m_buf[FVEL], 0, vel.data(), bytes3);
    m_context->readBuffer(m_buf[FCLR], 0, clr.data(), bytes1);
}
