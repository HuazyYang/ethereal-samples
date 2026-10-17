#include "beach_shallow_simulation.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
float Velocity(float momentum, float depth, float dry) {
    return depth > dry ? momentum / depth : 0.f;
}
}  // namespace

bool BeachShallowSimulation::Initialize(const BeachShallowSettings& s,
                                        const std::vector<float>& bed) {
    if (s.width < 2 || s.height < 2 || s.width > 2048 || s.height > 2048 ||
        bed.size() != size_t(s.width) * s.height || !std::isfinite(s.cellSizeX) ||
        !std::isfinite(s.cellSizeZ) || !std::isfinite(s.originX) ||
        !std::isfinite(s.originZ) || !std::isfinite(s.seaLevel) ||
        !std::isfinite(s.gravity) || !std::isfinite(s.cfl) || !std::isfinite(s.dryDepth) ||
        !std::isfinite(s.manning) || !std::isfinite(s.swellAmplitude) ||
        !std::isfinite(s.swellPeriod) || !std::isfinite(s.foamHalfLife) ||
        !std::isfinite(s.dryingTime) || s.cellSizeX <= 0 || s.cellSizeZ <= 0 ||
        s.gravity <= 0 || s.cfl <= 0 || s.cfl > 0.45f || s.dryDepth <= 0 || s.manning < 0 ||
        s.swellAmplitude < 0 || s.swellPeriod <= 0 || s.foamHalfLife <= 0 ||
        s.dryingTime <= 0)
        return false;
    for (float value : bed)
        if (!std::isfinite(value))
            return false;
    m_settings = s;
    m_bed = bed;
    m_cells.resize(bed.size());
    m_next.resize(bed.size());
    m_wetness.resize(bed.size());
    m_xFaces.resize(size_t(s.width + 1) * s.height);
    m_zFaces.resize(size_t(s.height + 1) * s.width);
    return Reset(s.seaLevel);
}

bool BeachShallowSimulation::Reset(float seaLevel) {
    if (m_bed.empty() || !std::isfinite(seaLevel))
        return false;
    m_settings.seaLevel = seaLevel;
    for (size_t i = 0; i < m_cells.size(); ++i) {
        m_cells[i] = {std::max(seaLevel - m_bed[i], 0.f), 0, 0, 0};
        m_wetness[i] = m_cells[i].depth > m_settings.dryDepth ? 1.f : 0.f;
    }
    m_diagnostics = {};
    Measure();
    return true;
}

bool BeachShallowSimulation::SetInitialState(const std::vector<BeachShallowCell>& cells) {
    if (m_bed.empty() || cells.size() != m_bed.size())
        return false;
    for (const auto& q : cells)
        if (!std::isfinite(q.depth) || !std::isfinite(q.momentumX) ||
            !std::isfinite(q.momentumZ) || !std::isfinite(q.foamMass) || q.depth < 0 ||
            q.foamMass < 0 || q.foamMass > q.depth ||
            (q.depth <= m_settings.dryDepth && (q.momentumX != 0 || q.momentumZ != 0)))
            return false;
    m_cells = cells;
    for (size_t i = 0; i < cells.size(); ++i)
        m_wetness[i] = cells[i].depth > m_settings.dryDepth ? 1.f : 0.f;
    m_diagnostics = {};
    Measure();
    return true;
}

BeachShallowSimulation::Face BeachShallowSimulation::Flux(BeachShallowCell l,
                                                          BeachShallowCell r, float bl,
                                                          float br, bool zAxis) const {
    const float g = m_settings.gravity, dry = m_settings.dryDepth;
    if (l.depth == 0 && r.depth == 0)
        return {};
    float ul = Velocity(zAxis ? l.momentumZ : l.momentumX, l.depth, dry);
    float ur = Velocity(zAxis ? r.momentumZ : r.momentumX, r.depth, dry);
    float vl = Velocity(zAxis ? l.momentumX : l.momentumZ, l.depth, dry);
    float vr = Velocity(zAxis ? r.momentumX : r.momentumZ, r.depth, dry);
    // Audusse et al. (2004), hydrostatic reconstruction. The nonconservative
    // pressure corrections balance bed slope, while mass uses ONE shared flux.
    float faceBed = std::max(bl, br);
    float hl = std::max(0.f, l.depth + bl - faceBed);
    float hr = std::max(0.f, r.depth + br - faceBed);
    float cl = std::sqrt(g * hl), cr = std::sqrt(g * hr);
    float sl = std::min({0.f, ul - cl, ur - cr});
    float sr = std::max({0.f, ul + cl, ur + cr});
    // Einfeldt dry-bed wave speeds; a wet/dry rarefaction travels at u +/- 2c.
    if (hr <= dry)
        sr = std::max(sr, ul + 2.f * cl);
    if (hl <= dry)
        sl = std::min(sl, ur - 2.f * cr);
    float inverse = sr > sl ? 1.f / (sr - sl) : 0.f;
    float mass = (sr * hl * ul - sl * hr * ur + sl * sr * (hr - hl)) * inverse;
    float normal =
        (sr * (hl * ul * ul + 0.5f * g * hl * hl) -
         sl * (hr * ur * ur + 0.5f * g * hr * hr) + sl * sr * (hr * ur - hl * ul)) *
        inverse;
    float tangent =
        (sr * hl * ul * vl - sl * hr * ur * vr + sl * sr * (hr * vr - hl * vl)) * inverse;
    float concentration =
        mass >= 0 ? Velocity(l.foamMass, l.depth, dry) : Velocity(r.foamMass, r.depth, dry);
    return {{mass, zAxis ? tangent : normal, zAxis ? normal : tangent,
             mass * std::clamp(concentration, 0.f, 1.f)},
            0.5f * g * (l.depth * l.depth - hl * hl),
            0.5f * g * (r.depth * r.depth - hr * hr)};
}

BeachShallowCell BeachShallowSimulation::Ghost(BeachShallowCell q, float bed, bool zAxis,
                                               bool lower, bool wavemaker,
                                               double time) const {
    const auto& s = m_settings;
    if (!s.openBoundary) {
        if (zAxis)
            q.momentumZ = -q.momentumZ;
        else
            q.momentumX = -q.momentumX;
        return q;
    }
    // Side boundaries are transmissive. On the seaward edge prescribe only
    // the incoming characteristic; retain the outgoing one to let backwash out.
    if (!zAxis || !lower || bed >= s.seaLevel - s.dryDepth)
        return q;
    float h0 = s.seaLevel - bed;
    float ramp = float(std::min(time / (2.0 * s.swellPeriod), 1.0));
    ramp = 0.5f - 0.5f * std::cos(3.141592654f * ramp);
    float eta = wavemaker ? s.swellAmplitude * ramp *
                                float(std::sin(6.28318530718 * time / s.swellPeriod))
                          : 0.f;
    float c0 = std::sqrt(s.gravity * h0);
    float incoming = 2.f * c0 + 2.f * eta * std::sqrt(s.gravity / h0);
    float outgoing =
        Velocity(q.momentumZ, q.depth, s.dryDepth) - 2.f * std::sqrt(s.gravity * q.depth);
    float c = std::max(0.f, 0.25f * (incoming - outgoing));
    float h = c * c / s.gravity;
    float u = 0.5f * (incoming + outgoing);
    return {h, h * Velocity(q.momentumX, q.depth, s.dryDepth), h * u,
            h * Velocity(q.foamMass, q.depth, s.dryDepth)};
}

double BeachShallowSimulation::Advance(double seconds, bool wavemaker,
                                       uint32_t maximumSteps) {
    if (m_cells.empty() || !std::isfinite(seconds) || seconds <= 0)
        return 0;
    const auto& s = m_settings;
    double advanced = 0;
    for (uint32_t step = 0; step < maximumSteps && advanced < seconds; ++step) {
        float maximumX = 0, maximumZ = 0;
        for (const auto& q : m_cells) {
            float c = std::sqrt(s.gravity * q.depth);
            // The factor 2 also bounds wet/dry Riemann fans and the incoming
            // boundary wave. This unsplit 2D CFL is stricter than a 1D bound.
            maximumX = std::max(
                maximumX, std::abs(Velocity(q.momentumX, q.depth, s.dryDepth)) + 2 * c);
            maximumZ = std::max(
                maximumZ, std::abs(Velocity(q.momentumZ, q.depth, s.dryDepth)) + 2 * c);
        }
        if (s.openBoundary) {
            for (uint32_t x = 0; x < s.width; ++x) {
                auto q =
                    Ghost(m_cells[x], m_bed[x], true, true, wavemaker, m_diagnostics.time);
                float c = std::sqrt(s.gravity * q.depth);
                maximumX = std::max(
                    maximumX, std::abs(Velocity(q.momentumX, q.depth, s.dryDepth)) + 2 * c);
                maximumZ = std::max(
                    maximumZ, std::abs(Velocity(q.momentumZ, q.depth, s.dryDepth)) + 2 * c);
            }
        }
        float maximumRate = maximumX / s.cellSizeX + maximumZ / s.cellSizeZ;
        float dt = float(
            std::min(seconds - advanced, double(s.cfl / std::max(maximumRate, 1e-4f))));
        if (dt <= 0)
            break;
        m_diagnostics.maximumCfl = std::max(m_diagnostics.maximumCfl, dt * maximumRate);
        Step(dt, wavemaker);
        m_diagnostics.time += dt;
        ++m_diagnostics.steps;
        advanced += dt;
    }
    Measure();
    return advanced;
}

void BeachShallowSimulation::Step(float dt, bool wavemaker) {
    const auto& s = m_settings;
    const uint32_t w = s.width, h = s.height;
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x <= w; ++x) {
            size_t li = size_t(y) * w + (x ? x - 1 : 0);
            size_t ri = size_t(y) * w + std::min(x, w - 1);
            auto l = m_cells[li], r = m_cells[ri];
            if (!x)
                l = Ghost(r, m_bed[ri], false, true, wavemaker, m_diagnostics.time);
            if (x == w)
                r = Ghost(l, m_bed[li], false, false, wavemaker, m_diagnostics.time);
            m_xFaces[size_t(y) * (w + 1) + x] = Flux(l, r, m_bed[li], m_bed[ri], false);
        }
    for (uint32_t y = 0; y <= h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            size_t li = size_t(y ? y - 1 : 0) * w + x;
            size_t ri = size_t(std::min(y, h - 1)) * w + x;
            auto l = m_cells[li], r = m_cells[ri];
            if (!y)
                l = Ghost(r, m_bed[ri], true, true, wavemaker, m_diagnostics.time);
            if (y == h)
                r = Ghost(l, m_bed[li], true, false, wavemaker, m_diagnostics.time);
            m_zFaces[size_t(y) * w + x] = Flux(l, r, m_bed[li], m_bed[ri], true);
        }
    double boundary = 0;
    for (uint32_t y = 0; y < h; ++y)
        boundary += (m_xFaces[size_t(y) * (w + 1)].flux.depth -
                     m_xFaces[size_t(y) * (w + 1) + w].flux.depth) *
                    dt * s.cellSizeZ;
    for (uint32_t x = 0; x < w; ++x)
        boundary += (m_zFaces[x].flux.depth - m_zFaces[size_t(h) * w + x].flux.depth) * dt *
                    s.cellSizeX;
    m_diagnostics.boundaryVolume += boundary;
    float rx = dt / s.cellSizeX, rz = dt / s.cellSizeZ;
    float foamDecay = std::exp(-0.69314718f * dt / s.foamHalfLife);
    float wetDecay = std::exp(-dt / s.dryingTime);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            size_t i = size_t(y) * w + x;
            const Face& l = m_xFaces[size_t(y) * (w + 1) + x];
            const Face& r = m_xFaces[size_t(y) * (w + 1) + x + 1];
            const Face& b = m_zFaces[i];
            const Face& t = m_zFaces[i + w];
            const auto& old = m_cells[i];
            BeachShallowCell q;
            q.depth = old.depth - rx * (r.flux.depth - l.flux.depth) -
                      rz * (t.flux.depth - b.flux.depth);
            q.momentumX = old.momentumX -
                          rx * ((r.flux.momentumX + r.pressureLeft) -
                                (l.flux.momentumX + l.pressureRight)) -
                          rz * (t.flux.momentumX - b.flux.momentumX);
            q.momentumZ = old.momentumZ -
                          rz * ((t.flux.momentumZ + t.pressureLeft) -
                                (b.flux.momentumZ + b.pressureRight)) -
                          rx * (r.flux.momentumZ - l.flux.momentumZ);
            q.foamMass = old.foamMass - rx * (r.flux.foamMass - l.flux.foamMass) -
                         rz * (t.flux.foamMass - b.flux.foamMass);
            // Only roundoff is projected. Retain sub-dry-threshold water mass;
            // discarding it each step would make moving shores nonconservative.
            if (q.depth < 0) {
                m_diagnostics.roundoffVolume -= double(q.depth) * s.cellSizeX * s.cellSizeZ;
                q.depth = 0;
            }
            if (q.depth > s.dryDepth) {
                float speed = std::hypot(q.momentumX, q.momentumZ) / q.depth;
                float drag = 1.f + dt * s.gravity * s.manning * s.manning * speed /
                                       std::pow(q.depth, 4.f / 3.f);
                q.momentumX /= drag;
                q.momentumZ /= drag;
                // Advected whitewater concentration with a compressive bore
                // source. This is a bounded empirical aeration closure, not
                // another height/depth shading band or a full multiphase fluid.
                float froude = speed / std::sqrt(s.gravity * q.depth);
                float compression =
                    std::max(0.f, (q.depth - old.depth) / (dt * std::max(q.depth, 0.02f)));
                float breakingFraction =
                    std::max(0.f, 1.f - 1.f / std::max(froude * froude, 1.f));
                float source = compression * breakingFraction;
                q.foamMass = q.depth - (q.depth - std::clamp(q.foamMass, 0.f, q.depth)) *
                                           std::exp(-source * dt);
            } else
                q.momentumX = q.momentumZ = 0;
            q.foamMass = std::clamp(q.foamMass * foamDecay, 0.f, q.depth);
            m_wetness[i] = q.depth > 0.008f ? 1.f : m_wetness[i] * wetDecay;
            m_next[i] = q;
        }
    m_cells.swap(m_next);
}

void BeachShallowSimulation::Measure() {
    m_diagnostics.waterVolume = 0;
    m_diagnostics.maximumSpeed = 0;
    m_diagnostics.minimumDepth = std::numeric_limits<float>::max();
    for (const auto& q : m_cells) {
        m_diagnostics.waterVolume +=
            double(q.depth) * m_settings.cellSizeX * m_settings.cellSizeZ;
        m_diagnostics.maximumSpeed = std::max(
            m_diagnostics.maximumSpeed,
            Velocity(std::hypot(q.momentumX, q.momentumZ), q.depth, m_settings.dryDepth));
        m_diagnostics.minimumDepth = std::min(m_diagnostics.minimumDepth, q.depth);
    }
}
