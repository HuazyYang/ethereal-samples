#pragma once

#include "beach_scene.h"
#include "beach_assets.h"
#include "beach_shallow_simulation.h"

// Metres, Y up. Geometry parameters, not shading controls. The original survey
// remains an independent input; this reference composition never edits it.
struct BeachCoveSettings {
    uint32_t width = 1537, height = 1025;
    float minimumX = -140.f, minimumZ = -260.f;
    float extentX = 1000.f, extentZ = 600.f;
    float meanSeaLevel = 0.f;
    const BeachGroundTextures* ground = nullptr;
};

struct BeachCoastPoint {
    float x, z;
};

// Open, oriented shoreline extending beyond both Z edges of the terrain.
// Positive signed distance is land. Unlike z=f(x), this supports hooked spits.
const std::vector<BeachCoastPoint>& BeachCoveCoastline();
float BeachCoveSignedDistance(float x, float z);
float BeachCoveElevation(float x, float z);
// Bounded, nonperiodic metre-scale relief; zero on the submerged shoreface.
float BeachCoveDetailElevation(float x, float z, const BeachGroundTextures& ground);
bool BuildBeachCove(const BeachCoveSettings& settings, std::vector<BeachVertex>& vertices,
                    std::vector<uint32_t>& indices);
void ClassifyBeachCoveGround(float seaLevel, const BeachGroundTextures& ground,
                             std::vector<BeachTerrainRenderVertex>& vertices);
BeachShallowSettings BeachCoveShallowSettings(float seaLevel = 0.f);
