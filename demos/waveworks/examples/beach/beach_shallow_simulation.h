#pragma once

#include <cstdint>
#include <vector>

// Conservative depth-averaged Saint-Venant equations, in metres and seconds.
// CPU implementation is also the runtime solver: tests exercise the exact
// numerical path rendered by D3D11, not a disconnected reference model.
struct BeachShallowCell {
    float depth = 0, momentumX = 0, momentumZ = 0, foamMass = 0;
};

struct BeachShallowSettings {
    uint32_t width = 384, height = 160;
    float originX = -140.f, originZ = -66.f;
    float cellSizeX = 0.8f, cellSizeZ = 0.5f;
    float seaLevel = 0.f;
    float gravity = 9.81f, cfl = 0.42f;
    float dryDepth = 0.001f, manning = 0.022f;
    bool openBoundary = true;
    float swellAmplitude = 0.18f, swellPeriod = 8.5f;
    float foamHalfLife = 3.5f, dryingTime = 35.f;
};

struct BeachShallowDiagnostics {
    double time = 0, waterVolume = 0, boundaryVolume = 0, roundoffVolume = 0;
    float maximumSpeed = 0, maximumCfl = 0, minimumDepth = 0;
    uint64_t steps = 0;
};

class BeachShallowSimulation {
 public:
    bool Initialize(const BeachShallowSettings& settings, const std::vector<float>& bed);
    bool Reset(float seaLevel);
    // Bounded caller workload: returns the seconds actually integrated. Any
    // unconsumed time remains the caller's debt, never an oversized unstable dt.
    double Advance(double seconds, bool wavemaker, uint32_t maximumSteps = 64);
    bool SetInitialState(const std::vector<BeachShallowCell>& cells);
    const std::vector<BeachShallowCell>& Cells() const { return m_cells; }
    const std::vector<float>& Bed() const { return m_bed; }
    const std::vector<float>& Wetness() const { return m_wetness; }
    const BeachShallowSettings& Settings() const { return m_settings; }
    const BeachShallowDiagnostics& Diagnostics() const { return m_diagnostics; }

 private:
    struct Face {
        BeachShallowCell flux;
        float pressureLeft = 0, pressureRight = 0;
    };
    Face Flux(BeachShallowCell left, BeachShallowCell right, float leftBed, float rightBed,
              bool zAxis) const;
    BeachShallowCell Ghost(BeachShallowCell cell, float bed, bool zAxis, bool lowerEdge,
                           bool wavemaker, double time) const;
    void Step(float dt, bool wavemaker);
    void Measure();
    BeachShallowSettings m_settings;
    BeachShallowDiagnostics m_diagnostics;
    std::vector<float> m_bed, m_wetness;
    std::vector<BeachShallowCell> m_cells, m_next;
    std::vector<Face> m_xFaces, m_zFaces;
};
