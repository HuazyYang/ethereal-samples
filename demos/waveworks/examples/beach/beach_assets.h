#pragma once

#include "beach_scene.h"
#include <string>

// Physical footprints of the source scans, shared by CPU displacement/placement
// and the renderer's material UV constants.
inline constexpr float BeachGroundScanSize = 20.f;
inline constexpr float BeachSandScanSize = 1.6f;
inline constexpr float BeachVegetationScanSize = 2.f;
inline constexpr float BeachGroundReliefScale = 2.8f;
inline constexpr gfsdk_float4 BeachGroundVisibilityBounds = {-80.f, -105.f, 320.f, 310.f};

// Cosine-weighted sky visibility from the actual triangulated height field.
// Bounds are origin XZ / extent XZ. This does not modify height or base color.
bool BuildBeachGroundVisibility(const BeachBathymetry& bed, gfsdk_float4 bounds,
                                uint32_t width, uint32_t height,
                                std::vector<float>& values);

// Filtered linear scan data, also consumed by the CPU terrain mesh builder.
struct BeachScalarTexture {
    uint32_t width = 0, height = 0;
    std::vector<float> pixels;
    bool Load(const std::string& path);
    bool IsValid() const;
    float Sample(float u, float v) const;
    float SampleStochastic(float u, float v) const;
};
struct BeachGroundTextures {
    BeachScalarTexture groundHeight, sandHeight, cover;
    bool Load(const std::string& folder);
    bool IsValid() const;
};

struct BeachPropMesh {
    std::vector<BeachVertex> vertices;
    std::vector<uint32_t> indices;
    float height = 0;
};
// Original glTF local geometry/UVs, converted offline. No runtime mesh synthesis.
bool LoadBeachPropMeshes(const std::string& path, std::vector<BeachPropMesh>& meshes);
struct BeachPropInstance {
    float x, y, z, scale;
    float cosine, sine, height, wind;
};
using BeachPropInstances = std::vector<std::vector<BeachPropInstance>>;
bool BuildBeachPropInstances(const std::vector<BeachPropMesh>& meshes, uint32_t kind,
                             const BeachBathymetry& bed, const BeachGroundTextures& ground,
                             BeachPropInstances& instances);
