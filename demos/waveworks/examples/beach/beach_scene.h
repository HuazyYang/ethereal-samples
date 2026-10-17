#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include <waveworks/GFSDK_WaveWorks_FloatTypes.h>

struct MainState;
struct BeachShallowSettings;

inline gfsdk_float3 BeachSunDirectionYUp(float elevationDegrees, float azimuthDegrees) {
    const float elevation = elevationDegrees * 0.01745329252f;
    const float azimuth = azimuthDegrees * 0.01745329252f;
    return {std::cos(elevation) * std::cos(azimuth), std::sin(elevation),
            std::cos(elevation) * std::sin(azimuth)};
}
inline gfsdk_float3 BeachSunShadowRight(float azimuthDegrees) {
    const float azimuth = azimuthDegrees * 0.01745329252f;
    return {-std::sin(azimuth), 0.f, std::cos(azimuth)};
}

struct BeachVertex {
    float x, y, z, nx, ny, nz, u, v;
};

// Runtime-only extension of the version-one, eight-float BeachVertex stored in
// hanalei_cove.bin. The packed channels are sand, vegetation, rock, and water
// coat coverage respectively. The three material weights always add to 255.
struct BeachTerrainRenderVertex {
    float x, y, z, nx, ny, nz, u, v;
    uint32_t packedMaterialWeights;
};

enum class BeachTerrainMaterialChannel : uint32_t {
    Sand = 0,
    Vegetation = 1,
    Rock = 2,
    WaterCoat = 3
};

constexpr uint8_t GetBeachTerrainMaterialWeight(uint32_t packedWeights,
                                                BeachTerrainMaterialChannel channel) {
    return static_cast<uint8_t>((packedWeights >> (8u * static_cast<uint32_t>(channel))) &
                                0xffu);
}

struct BeachTerrainClassification {
    float terrainDatumSeaLevel = 0.f;
    float upperBeachHeightAboveSeaLevel = 2.6f;
    float vegetationStartHeightAboveSeaLevel = 1.65f;
    float maximumRunupHeightAboveSeaLevel = 0.22f;
    float sandAngleOfReposeDegrees = 38.f;
    float maximumVegetatedSlopeDegrees = 55.f;
};

bool BuildBeachTerrainRenderVertices(const std::vector<BeachVertex>& sourceVertices,
                                     const BeachTerrainClassification& settings,
                                     std::vector<BeachTerrainRenderVertex>& renderVertices);

struct BeachBathymetryMetadata {
    uint32_t width = 0;
    uint32_t height = 0;
    float minimumX = 0.f;
    float maximumX = 0.f;
    float minimumZ = 0.f;
    float maximumZ = 0.f;
    float minimumElevation = 0.f;
    float maximumElevation = 0.f;
    // The texture follows source row order. These signed values therefore
    // describe both grid spacing and the world-XZ to texture-UV orientation.
    float originX = 0.f;
    float originZ = 0.f;
    float averageStepX = 0.f;
    float averageStepZ = 0.f;
    float worldToTextureUScale = 0.f;
    float worldToTextureUBias = 0.f;
    float worldToTextureVScale = 0.f;
    float worldToTextureVBias = 0.f;

    bool IsValid() const;
};

struct BeachBathymetry {
    BeachBathymetryMetadata metadata;
    // Row-major world-space elevation values, ready for an R32_FLOAT upload.
    std::vector<float> elevations;

    bool IsValid() const;
};

bool BuildBeachBathymetry(const std::vector<BeachVertex>& sourceVertices,
                          BeachBathymetry& bathymetry);

// Samples the unchanged survey triangles at finite-volume cell centres.
// Rejects out-of-survey domains instead of inventing/clamping a coast extension.
bool BuildBeachShallowBed(const BeachBathymetry& bathymetry,
                          const BeachShallowSettings& settings, std::vector<float>& bed);

struct BeachVegetationVertex {
    float rootX, rootY, rootZ;
    float facingX, facingZ;
    float sideOffset, height;
    float u, v;
    float windWeight;
};

struct BeachVegetationSettings {
    float bladeDensityPerSquareMeter = 0.35f;
    float minimumBladeHeight = 0.18f;
    float maximumBladeHeight = 0.42f;
    float minimumBladeHalfWidth = 0.015f;
    float maximumBladeHalfWidth = 0.035f;
    uint32_t maximumBladeCount = 12000;
    uint32_t randomSeed = 0x6d2b79f5u;
};

struct BeachVegetationMesh {
    std::vector<BeachVegetationVertex> vertices;
    std::vector<uint32_t> indices;

    uint32_t GetBladeCount() const;
    bool IsValid() const;
};

bool BuildBeachVegetationMesh(const std::vector<BeachTerrainRenderVertex>& terrainVertices,
                              const BeachBathymetryMetadata& bathymetry,
                              const BeachVegetationSettings& settings,
                              BeachVegetationMesh& vegetation);

struct BeachSurfSimulationSettings {
    uint32_t domainGridSize = 512;
    float domainCenterX = -8.f;
    float domainCenterZ = -30.f;
    float domainWorldspaceSize = 180.f;
    float forcingLineCenterX = -8.f;
    float forcingLineCenterZ = -64.f;
    float forcingLineTangentX = 1.f;
    float forcingLineTangentZ = 0.f;
    float forcingLineLength = 120.f;
    float forcingLineSeparation = 4.f;
    uint32_t samplesPerLine = 128;
    float temporalFrequencyHz = 0.14f;
    float secondLinePhaseOffsetRadians = 1.57079632679f;
    float displacementForcingRate = 0.08f;
    float velocityPotentialForcingRate = 0.04f;
};

// Produces grid-coordinate disturbances for direct submission to the Local
// Waves API. No WaveWorks state is read or mutated by this function.
bool BuildBeachSurfDisturbances(const BeachSurfSimulationSettings& settings,
                                float deltaTimeSeconds, double simulationTimeSeconds,
                                std::vector<gfsdk_float4>& disturbances);

enum class BeachSceneStage {
    Clear,
    OpaqueTerrain,
    Sky,
    SceneCapture,
    TransmissiveWater,
    TransparentFoam,
    ToneMap,
    UI
};

enum class BeachBlendMode { Disabled, Replace, PremultipliedAlpha };

struct BeachPassContract {
    BeachSceneStage stage;
    bool depthTest;
    bool depthWrite;
    BeachBlendMode blend;
};

inline constexpr std::array<BeachPassContract, 8> BeachScenePassSequence = {
    {{BeachSceneStage::Clear, false, true, BeachBlendMode::Disabled},
     {BeachSceneStage::OpaqueTerrain, true, true, BeachBlendMode::Disabled},
     {BeachSceneStage::Sky, true, false, BeachBlendMode::Disabled},
     {BeachSceneStage::SceneCapture, false, false, BeachBlendMode::Disabled},
     {BeachSceneStage::TransmissiveWater, true, false, BeachBlendMode::Replace},
     {BeachSceneStage::TransparentFoam, true, false, BeachBlendMode::PremultipliedAlpha},
     {BeachSceneStage::ToneMap, false, false, BeachBlendMode::Disabled},
     {BeachSceneStage::UI, false, false, BeachBlendMode::PremultipliedAlpha}}};

struct BeachRenderTargetSize {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t sampleCount = 1;

    bool IsValid() const { return width > 0 && height > 0 && sampleCount == 1; }
    bool Matches(uint32_t newWidth, uint32_t newHeight, uint32_t newSampleCount) const {
        return width == newWidth && height == newHeight && sampleCount == newSampleCount;
    }
};

void ConfigureBeachScene(MainState& state);
bool LoadBeachTerrainMesh(const char* filename, std::vector<BeachVertex>& vertices,
                          std::vector<uint32_t>& indices);
