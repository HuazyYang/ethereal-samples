#include "beach_shallow_water.h"
#include "beach_cove.h"
#include "asset_utils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

BeachShallowWater::BeachShallowWater(nvrhi::IDevice* device, MainState* state)
    : m_device(device),
      m_state(state) {}

bool BeachShallowWater::Init(const BeachBathymetry& bathymetry) {
    if (!bathymetry.IsValid())
        return false;
    BeachShallowSettings s = m_state->bBeachCoveTerrain
        ? BeachCoveShallowSettings() : BeachShallowSettings{};
    s.seaLevel = m_state->OceanQuadtreeParameters.mean_sea_level;
    // Fit inside the unchanged source survey. The PDE owns only the shelf;
    // deep water keeps the dispersive FFT, rather than shortening timesteps
    // for a 260 m trench where the shallow-water approximation is inappropriate.
    std::vector<float> bed;
    if (!BuildBeachShallowBed(bathymetry, s, bed))
        return false;
    if (!m_simulation.Initialize(s, bed))
        return false;
    const double initialVolume = m_simulation.Diagnostics().waterVolume;
    if (ReadProcessEnvironmentVariable("WAVEWORKS_BEACH_NO_PDE") == "1")
        m_state->bBeachShallowSimulation = false;
    m_device->createCommandList(nvrhi::CommandListParameters(), &m_commands);
    nvrhi::TextureDesc td;
    td.width = s.width;
    td.height = s.height;
    td.format = nvrhi::Format::RGBA32_FLOAT;
    td.initialState = nvrhi::ResourceStates::ShaderResource;
    td.keepInitialState = true;
    td.debugName = "Saint-Venant elevation depth aeration wetness";
    m_device->createTexture(td, &m_state->hBeachShallowSurface);
    td.debugName = "Saint-Venant surface slopes and depth-averaged velocity";
    m_device->createTexture(td, &m_state->hBeachShallowFlow);
    nvrhi::BufferDesc cb;
    cb.byteSize = 32;
    cb.isConstantBuffer = true;
    cb.initialState = nvrhi::ResourceStates::ConstantBuffer;
    cb.keepInitialState = true;
    cb.debugName = "Beach shallow-water domain";
    m_device->createBuffer(cb, &m_state->hBeachShallowConstants);
    if (!m_commands || !m_state->hBeachShallowSurface || !m_state->hBeachShallowFlow ||
        !m_state->hBeachShallowConstants)
        return false;
    m_surface.resize(bed.size());
    m_flow.resize(bed.size());
    // Deterministic physical spin-up; no prepainted wave/foam initial texture.
    if (m_state->bBeachShallowSimulation && m_state->bUpdateLocalWaves) {
        auto start = std::chrono::steady_clock::now();
        const double spinup = m_state->bBeachCoveTerrain ? 40.0 : 28.0;
        double remaining = spinup;
        while (remaining > 1e-6)
            remaining -= m_simulation.Advance(remaining, m_state->bBeachSurfForcing, 256);
        std::printf("Beach SWE: %ux%u HLL/hydrostatic, %.0f s spin-up in %.3f s\n", s.width,
                    s.height, spinup,
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
                        .count());
        float maximumFoam = 0, maximumElevation = 0;
        uint32_t runupCells = 0;
        for (size_t i = 0; i < bed.size(); ++i) {
            const auto& q = m_simulation.Cells()[i];
            if (q.depth > s.dryDepth) {
                maximumFoam = std::max(maximumFoam, q.foamMass / q.depth);
                maximumElevation =
                    std::max(maximumElevation, q.depth + bed[i] - s.seaLevel);
                runupCells += bed[i] > s.seaLevel;
            }
        }
        const auto& d = m_simulation.Diagnostics();
        double massError = std::abs(d.waterVolume - initialVolume - d.boundaryVolume) /
                           std::max(initialVolume, 1.0);
        if (!std::isfinite(d.waterVolume) || d.minimumDepth < 0 || massError > 2e-5 ||
            d.roundoffVolume > 1e-5 || d.maximumCfl > s.cfl + 1e-5) {
            std::fprintf(stderr, "FAIL: beach SWE spin-up conservation/positivity/CFL\n");
            return false;
        }
        std::printf("PASS: beach SWE spin-up, relative mass residual %.9f\n", massError);
        std::printf(
            "Beach SWE: peak eta %.4f m, foam %.4f, wet run-up cells %u, CFL %.3f, mass "
            "correction %.9f m3\n",
            maximumElevation, maximumFoam, runupCells,
            m_simulation.Diagnostics().maximumCfl,
            m_simulation.Diagnostics().roundoffVolume);
    }
    if (ReadProcessEnvironmentVariable("WAVEWORKS_VALIDATE_SHALLOW") == "1" &&
        !ValidateLifecycle())
        return false;
    Upload();
    return true;
}

bool BeachShallowWater::ValidateLifecycle() {
    // Exercise the actual application update guard, with no WaveWorks calls
    // or GPU writes. Restore the complete solver afterwards, including foam,
    // wetness and flux history, so enabling validation cannot alter a capture.
    auto savedSimulation = m_simulation;
    const bool savedLocal = m_state->bUpdateLocalWaves;
    const bool savedSimulate = m_state->bSimulateWater;
    const bool savedEnabled = m_state->bBeachShallowSimulation;
    const bool savedReset = m_state->bBeachResetShallow;
    const double savedDebt = m_timeDebt;
    const bool savedDirty = m_dirty;
    bool passed = true;
    auto retained = [&]() {
        const auto& before = savedSimulation.Cells();
        const auto& after = m_simulation.Cells();
        return m_simulation.Diagnostics().time == savedSimulation.Diagnostics().time &&
               std::memcmp(before.data(), after.data(),
                           before.size() * sizeof(before[0])) == 0 &&
               m_simulation.Wetness() == savedSimulation.Wetness();
    };
    m_state->bBeachShallowSimulation = true;
    m_state->bSimulateWater = true;
    m_state->bUpdateLocalWaves = false;
    m_state->bBeachResetShallow = true;
    Animate(1.f / 60.f);
    passed = passed && retained() && !m_state->bBeachResetShallow;
    m_state->bUpdateLocalWaves = true;
    m_state->bSimulateWater = false;
    Animate(1.f / 60.f);
    passed = passed && retained();
    m_state->bSimulateWater = true;
    m_state->bBeachShallowSimulation = false;
    Animate(1.f / 60.f);
    passed = passed && retained();
    m_state->bBeachShallowSimulation = true;
    Animate(1.f / 60.f);
    passed = passed && m_simulation.Diagnostics().time > savedSimulation.Diagnostics().time;
    m_state->bBeachResetShallow = true;
    m_state->bSimulateWater = false;
    Animate(1.f / 60.f);
    passed = passed && m_simulation.Diagnostics().steps == 0 &&
             m_simulation.Diagnostics().time == 0 && !m_state->bBeachResetShallow &&
             m_timeDebt == 0;
    for (const auto& q : m_simulation.Cells())
        passed = passed && q.momentumX == 0 && q.momentumZ == 0 && q.foamMass == 0;
    m_simulation = std::move(savedSimulation);
    m_state->bUpdateLocalWaves = savedLocal;
    m_state->bSimulateWater = savedSimulate;
    m_state->bBeachShallowSimulation = savedEnabled;
    m_state->bBeachResetShallow = savedReset;
    m_timeDebt = savedDebt;
    m_dirty = savedDirty;
    m_state->fBeachShallowMilliseconds = 0;
    m_state->fBeachShallowDebt = float(m_timeDebt);
    m_state->fBeachShallowTime = float(m_simulation.Diagnostics().time);
    m_state->fBeachShallowCfl = m_simulation.Diagnostics().maximumCfl;
    std::printf("%s: beach SWE local-disable / pause / disable / resume / reset guards\n",
                passed ? "PASS" : "FAIL");
    return passed;
}

void BeachShallowWater::Animate(float elapsed) {
    if (!m_state->bUpdateLocalWaves) {
        m_state->bBeachResetShallow = false;
        return;
    }
    bool reset =
        m_state->bBeachResetShallow ||
        m_simulation.Settings().seaLevel != m_state->OceanQuadtreeParameters.mean_sea_level;
    if (reset) {
        m_simulation.Reset(m_state->OceanQuadtreeParameters.mean_sea_level);
        m_state->bBeachResetShallow = false;
        m_timeDebt = 0;
        m_dirty = true;
    }
    if (!m_state->bSimulateWater || !m_state->bBeachShallowSimulation ||
        !std::isfinite(elapsed) || elapsed <= 0)
        return;
    auto start = std::chrono::steady_clock::now();
    m_timeDebt += elapsed;
    double advanced = m_simulation.Advance(m_timeDebt, m_state->bBeachSurfForcing, 24);
    m_timeDebt = std::max(0.0, m_timeDebt - advanced);
    m_dirty = advanced > 0 || m_dirty;
    m_state->fBeachShallowMilliseconds = float(
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
            .count());
    m_state->fBeachShallowDebt = float(m_timeDebt);
    m_state->fBeachShallowTime = float(m_simulation.Diagnostics().time);
    m_state->fBeachShallowCfl = m_simulation.Diagnostics().maximumCfl;
}

void BeachShallowWater::Upload() {
    const auto& s = m_simulation.Settings();
    struct Constants {
        float originX, originZ, inverseX, inverseZ, dx, dz, enabled, sea;
    };
    Constants cb = {s.originX,
                    s.originZ,
                    1.f / (s.width * s.cellSizeX),
                    1.f / (s.height * s.cellSizeZ),
                    s.cellSizeX,
                    s.cellSizeZ,
                    m_state->bBeachShallowSimulation ? 1.f : 0.f,
                    s.seaLevel};
    m_commands->open();
    m_commands->writeBuffer(m_state->hBeachShallowConstants, &cb, sizeof(cb));
    if (m_dirty) {
        const auto& cells = m_simulation.Cells();
        const auto& bed = m_simulation.Bed();
        const auto& wetness = m_simulation.Wetness();
        for (uint32_t y = 0; y < s.height; ++y)
            for (uint32_t x = 0; x < s.width; ++x) {
                size_t i = size_t(y) * s.width + x;
                const auto& q = cells[i];
                // Extend the wet-cell elevation into dry neighbours for a
                // surface gradient: never interpret an exposed cliff as a wave.
                float eta = bed[i] + q.depth;
                if (q.depth <= s.dryDepth) {
                    float weightedEta = 0, weight = 0;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            int px = int(x) + dx, py = int(y) + dy;
                            if (px < 0 || py < 0 || px >= int(s.width) ||
                                py >= int(s.height))
                                continue;
                            size_t k = size_t(py) * s.width + px;
                            if (cells[k].depth > s.dryDepth) {
                                weightedEta += (bed[k] + cells[k].depth) * cells[k].depth;
                                weight += cells[k].depth;
                            }
                        }
                    // Eta is undefined on dry land. Extend the neighbouring
                    // water surface for texture filtering, not the cliff top.
                    eta = weight > 0 ? weightedEta / weight : s.seaLevel;
                }
                auto elevation = [&](uint32_t px, uint32_t py) {
                    size_t k = size_t(py) * s.width + px;
                    return cells[k].depth > s.dryDepth ? bed[k] + cells[k].depth : eta;
                };
                uint32_t l = x ? x - 1 : x, r = std::min(x + 1, s.width - 1);
                uint32_t b = y ? y - 1 : y, t = std::min(y + 1, s.height - 1);
                float sx = -(elevation(r, y) - elevation(l, y)) / ((r - l) * s.cellSizeX);
                float sz = -(elevation(x, t) - elevation(x, b)) / ((t - b) * s.cellSizeZ);
                if (q.depth <= s.dryDepth)
                    sx = sz = 0;
                float inverseH = q.depth > s.dryDepth ? 1.f / q.depth : 0.f;
                m_surface[i] = {eta, q.depth, q.foamMass * inverseH, wetness[i]};
                m_flow[i] = {sx, sz, q.momentumX * inverseH, q.momentumZ * inverseH};
            }
        nvrhi::ITexture* textures[] = {m_state->hBeachShallowSurface,
                                       m_state->hBeachShallowFlow};
        const Pixel* data[] = {m_surface.data(), m_flow.data()};
        for (uint32_t i = 0; i < 2; ++i) {
            m_commands->beginTrackingTextureState(
                textures[i], nvrhi::AllSubresources,
                m_uploaded ? nvrhi::ResourceStates::ShaderResource
                           : nvrhi::ResourceStates::Common);
            m_commands->writeTexture(textures[i], 0, 0, data[i], s.width * sizeof(Pixel));
            m_commands->setTextureState(textures[i], nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
            m_commands->commitBarriers();
        }
        m_uploaded = true;
        m_dirty = false;
    }
    m_commands->close();
    m_device->executeCommandList(m_commands);
}

void BeachShallowWater::Shutdown() {
    const auto& d = m_simulation.Diagnostics();
    std::printf(
        "Beach SWE: t=%.3f steps=%llu volume=%.6f boundary=%.6f positivityCorrection=%.9f "
        "CFL=%.3f speed=%.3f debt=%.3f\n",
        d.time, static_cast<unsigned long long>(d.steps), d.waterVolume, d.boundaryVolume,
        d.roundoffVolume, d.maximumCfl, d.maximumSpeed, m_timeDebt);
    m_state->hBeachShallowSurface = nullptr;
    m_state->hBeachShallowFlow = nullptr;
    m_state->hBeachShallowConstants = nullptr;
    m_commands = nullptr;
}
