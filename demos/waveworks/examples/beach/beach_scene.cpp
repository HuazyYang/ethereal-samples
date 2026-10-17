#include "beach_scene.h"
#include "beach_shallow_simulation.h"

#include "main_state.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

namespace
{
struct BeachMeshHeader
{
    char magic[8];
    uint32_t version;
    uint32_t vertexCount;
    uint32_t indexCount;
};

constexpr char BeachMeshMagic[8] = { 'B', 'C', 'H', 'M', 'E', 'S', 'H', '\0' };
constexpr uint32_t BeachMeshVersion = 1;
constexpr uint32_t MaxBeachMeshElements = 8u * 1024u * 1024u;
constexpr uint32_t MaxBeachSurfSamplesPerLine = 4096u;
constexpr float Pi = 3.14159265358979323846f;
static_assert(sizeof(BeachMeshHeader) == 20, "beach mesh header layout");
static_assert(sizeof(BeachVertex) == 8 * sizeof(float), "beach source vertex layout");
static_assert(sizeof(BeachTerrainRenderVertex) == 9 * sizeof(float),
    "beach runtime terrain vertex layout");
static_assert(offsetof(BeachTerrainRenderVertex, packedMaterialWeights) == 8 * sizeof(float),
    "beach material weights offset");
static_assert(sizeof(BeachVegetationVertex) == 10 * sizeof(float),
    "beach vegetation vertex layout");

bool IsFinite(float value)
{
    return std::isfinite(value);
}

bool IsFinite(const BeachVertex& vertex)
{
    return IsFinite(vertex.x) && IsFinite(vertex.y) && IsFinite(vertex.z) &&
        IsFinite(vertex.nx) && IsFinite(vertex.ny) && IsFinite(vertex.nz) &&
        IsFinite(vertex.u) && IsFinite(vertex.v);
}

bool IsValid(const BeachTerrainClassification& settings)
{
    return IsFinite(settings.terrainDatumSeaLevel) &&
        IsFinite(settings.upperBeachHeightAboveSeaLevel) &&
        settings.upperBeachHeightAboveSeaLevel >= 0.f &&
        IsFinite(settings.vegetationStartHeightAboveSeaLevel) &&
        settings.vegetationStartHeightAboveSeaLevel >= 0.f &&
        IsFinite(settings.maximumRunupHeightAboveSeaLevel) &&
        settings.maximumRunupHeightAboveSeaLevel >= 0.f &&
        settings.maximumRunupHeightAboveSeaLevel <=
            settings.upperBeachHeightAboveSeaLevel &&
        IsFinite(settings.sandAngleOfReposeDegrees) &&
        settings.sandAngleOfReposeDegrees >= 0.f &&
        settings.sandAngleOfReposeDegrees <= 90.f &&
        IsFinite(settings.maximumVegetatedSlopeDegrees) &&
        settings.maximumVegetatedSlopeDegrees >= 0.f &&
        settings.maximumVegetatedSlopeDegrees <= 90.f;
}

uint32_t PackMaterialWeights(uint8_t sand, uint8_t vegetation, uint8_t rock,
    uint8_t waterCoat)
{
    return uint32_t(sand) | (uint32_t(vegetation) << 8u) |
        (uint32_t(rock) << 16u) | (uint32_t(waterCoat) << 24u);
}

uint32_t Hash32(uint32_t value)
{
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return value;
}

float UnitFloat(uint32_t value)
{
    return float(value >> 8u) * (1.f / 16777216.f);
}

float Interpolate(float minimum, float maximum, float amount)
{
    return minimum + (maximum - minimum) * amount;
}

bool IsPowerOfTwo(uint32_t value)
{
    return value != 0u && (value & (value - 1u)) == 0u;
}

bool IsInsideSurfDomain(const BeachSurfSimulationSettings& settings,
    float worldX, float worldZ)
{
    const float halfSize = settings.domainWorldspaceSize * 0.5f;
    return worldX >= settings.domainCenterX - halfSize &&
        worldX <= settings.domainCenterX + halfSize &&
        worldZ >= settings.domainCenterZ - halfSize &&
        worldZ <= settings.domainCenterZ + halfSize;
}

bool WorldToGrid(const BeachSurfSimulationSettings& settings,
    float worldX, float worldZ, float& gridX, float& gridY)
{
    if (!IsInsideSurfDomain(settings, worldX, worldZ)) return false;
    const float u = 0.5f + (worldX - settings.domainCenterX) /
        settings.domainWorldspaceSize;
    const float v = 0.5f + (worldZ - settings.domainCenterZ) /
        settings.domainWorldspaceSize;
    const float maximumCell = float(settings.domainGridSize - 1u);
    gridX = std::round(std::clamp(u, 0.f, 1.f) * maximumCell);
    gridY = std::round(std::clamp(v, 0.f, 1.f) * maximumCell);
    return true;
}
}

bool BuildBeachShallowBed(const BeachBathymetry& bathymetry,
    const BeachShallowSettings& s, std::vector<float>& bed)
{
    bed.clear();
    if (!bathymetry.IsValid() || s.width < 2 || s.height < 2 ||
        s.width > 2048 || s.height > 2048 || !std::isfinite(s.originX) ||
        !std::isfinite(s.originZ) || !std::isfinite(s.cellSizeX) ||
        !std::isfinite(s.cellSizeZ) || s.cellSizeX <= 0 || s.cellSizeZ <= 0)
        return false;
    const auto& b = bathymetry.metadata;
    if (s.originX < b.minimumX || s.originZ < b.minimumZ ||
        s.originX + s.width * s.cellSizeX > b.maximumX ||
        s.originZ + s.height * s.cellSizeZ > b.maximumZ) return false;
    bed.resize(size_t(s.width) * s.height);
    for (uint32_t y = 0; y < s.height; ++y)
        for (uint32_t x = 0; x < s.width; ++x) {
            float wx = s.originX + (x + 0.5f) * s.cellSizeX;
            float wz = s.originZ + (y + 0.5f) * s.cellSizeZ;
            float u = (wx - b.originX) * b.worldToTextureUScale * (b.width - 1);
            float v = (wz - b.originZ) * b.worldToTextureVScale * (b.height - 1);
            u = std::clamp(u, 0.f, float(b.width - 1));
            v = std::clamp(v, 0.f, float(b.height - 1));
            uint32_t i = std::min(uint32_t(u), b.width - 2);
            uint32_t j = std::min(uint32_t(v), b.height - 2);
            float a = u - i, c = v - j;
            size_t k = size_t(j) * b.width + i;
            float h00 = bathymetry.elevations[k], h10 = bathymetry.elevations[k + 1];
            float h01 = bathymetry.elevations[k + b.width];
            float h11 = bathymetry.elevations[k + b.width + 1];
            // Source index order (a,b,c),(b,d,c), with signed row orientation.
            bed[size_t(y) * s.width + x] = a + c <= 1.f
                ? h00 + a * (h10 - h00) + c * (h01 - h00)
                : h11 + (1 - a) * (h01 - h11) + (1 - c) * (h10 - h11);
        }
    return true;
}

void ConfigureBeachScene(MainState& state)
{
    state.bBeachScene = true;
    state.bBoat = false;
    state.bRain = false;
    // Keep the local solver active for the cove.  Its short wavelengths are
    // what give the broad, shallow shelf visible motion instead of relying on
    // a synthetic sine layer in the material shader.
    state.OceanLocalSimulationSettings.simulation_domain_center = { -8.f, -30.f };
    state.OceanLocalSimulationSettings.simulation_domain_worldspace_size = 180.f;
    state.OceanLocalSimulationParameters.amplitude_multiplier = 0.34f;
    state.OceanLocalSimulationParameters.lateral_multiplier = 0.72f;
    // Sustained surf uses the documented local-foam ranges: modest injection
    // at breaking crests, strong spatial diffusion, and slow temporal decay.
    state.OceanLocalSimulationParameters.foam_whitecaps_threshold = 0.28f;
    state.OceanLocalSimulationParameters.foam_dissipation_speed = 0.62f;
    state.OceanLocalSimulationParameters.foam_falloff_speed = 0.96f;
    state.OceanLocalSimulationParameters.foam_generation_amount = 0.085f;
    state.OceanLocalSimulationParameters.foam_generation_threshold = 0.34f;

    // An active sea breeze supplies short, choppy shore waves while the
    // sheltered cove still suppresses long open-ocean swell.
    state.OceanWindSimulationParameters.time_scale = 0.92f;
    state.OceanWindSimulationParameters.lateral_multiplier = 0.90f;
    // Beaufort mode overrides amplitude, fetch and foam inputs with the SDK's
    // lookup table. Use SI mode so this cove's explicit physical settings apply.
    state.OceanWindSimulationSettings.use_Beaufort_scale = false;
    state.OceanWindSimulationParameters.base_wind_speed = 6.34f;
    state.OceanWindSimulationParameters.base_wind_distance = 1.4f;
    state.OceanWindSimulationParameters.base_spectrum_peaking = 3.9f;
    state.OceanWindSimulationParameters.base_amplitude_multiplier = 0.80f;
    state.OceanWindSimulationParameters.base_small_waves_cutoff_length = 0.35f;
    state.OceanWindSimulationParameters.base_small_waves_cutoff_power = 0.18f;
    state.OceanWindSimulationParameters.swell_wind_speed = 1.04f;
    state.OceanWindSimulationParameters.swell_amplitude_multiplier = 0.28f;
    // Sample the authored WaveWorks cascades directly. The former sine-based
    // UV perturbation was an artificial detail layer, not simulated motion.
    state.OceanWindSimulationParameters.uv_warping_amplitude = 0.f;
    state.OceanWindSimulationParameters.uv_warping_frequency = 0.f;
    state.OceanWindSimulationParameters.foam_whitecaps_threshold = 0.54f;
    state.OceanWindSimulationParameters.foam_generation_amount = 0.095f;
    state.OceanWindSimulationParameters.foam_generation_threshold = 0.43f;
    state.OceanWindSimulationParameters.foam_dissipation_speed = 0.52f;
    // This is a base unresolved-slope variance for filtered microfacet water,
    // not a perceptual material roughness.
    // Unresolved capillary microstructure: perceptual roughness 0.18, stored
    // as slope variance alpha^2 = roughness^4. Resolved FFT moments add to it.
    state.fBeckmannRoughness = 0.00105f;
    state.fSunAngle = state.bBeachCoveTerrain ? 48.f : 55.f;
    state.fBeachSunAzimuth = state.bBeachCoveTerrain ? 16.f : 0.f;
    state.fSunIntensity = state.bBeachCoveTerrain ? 0.64f : 0.82f;
    // Sea-level aerosol extinction in 1/m, shared by sky and finite air paths.
    state.fBeachHaze = state.bBeachCoveTerrain ? 0.000060f : 0.000035f;
    state.fBeachShoreFoamIntensity = 0.90f;
    if (state.bBeachCoveTerrain) {
        // Strengthen resolved short-wave slopes rather than widening the
        // microfacet lobe to manufacture a band of sun glitter.
        state.OceanWindSimulationParameters.base_amplitude_multiplier = 1.20f;
        state.OceanWindSimulationParameters.base_wind_dependency = .55f;
        state.OceanWindSimulationParameters.base_spectrum_peaking = 2.4f;
        state.fBeckmannRoughness = 0.00006561f; // 0.09^4 capillary slope variance.
    }
}

bool LoadBeachTerrainMesh(const char* filename,
    std::vector<BeachVertex>& vertices, std::vector<uint32_t>& indices)
{
    vertices.clear();
    indices.clear();
    FILE* file = nullptr;
#ifdef WIN32
    fopen_s(&file, filename, "rb");
#else
    file = std::fopen(filename, "rb");
#endif
    if (!file) return false;

    BeachMeshHeader header{};
    bool loaded = std::fread(&header, sizeof(header), 1, file) == 1;
    loaded = loaded && std::memcmp(header.magic, BeachMeshMagic, sizeof(header.magic)) == 0;
    loaded = loaded && header.version == BeachMeshVersion;
    loaded = loaded && header.vertexCount > 0 && header.vertexCount <= MaxBeachMeshElements;
    loaded = loaded && header.indexCount > 0 && header.indexCount <= MaxBeachMeshElements;
    if (loaded)
    {
        vertices.resize(header.vertexCount);
        indices.resize(header.indexCount);
        loaded = std::fread(vertices.data(), sizeof(BeachVertex), vertices.size(), file) ==
            vertices.size();
        loaded = loaded && std::fread(indices.data(), sizeof(uint32_t), indices.size(), file) ==
            indices.size();
        loaded = loaded && std::fgetc(file) == EOF;
        loaded = loaded && std::all_of(indices.begin(), indices.end(),
            [&vertices](uint32_t index) { return index < vertices.size(); });
    }
    std::fclose(file);
    if (!loaded)
    {
        vertices.clear();
        indices.clear();
    }
    return loaded;
}

bool BuildBeachTerrainRenderVertices(const std::vector<BeachVertex>& sourceVertices,
    const BeachTerrainClassification& settings,
    std::vector<BeachTerrainRenderVertex>& renderVertices)
{
    renderVertices.clear();
    if (sourceVertices.empty() || !IsValid(settings)) return false;
    renderVertices.reserve(sourceVertices.size());

    for (const BeachVertex& source : sourceVertices)
    {
        if (!IsFinite(source) || source.ny < -1.001f || source.ny > 1.001f)
        {
            renderVertices.clear();
            return false;
        }

        const float heightAboveSeaLevel = source.y - settings.terrainDatumSeaLevel;
        const float upDotNormal = std::clamp(source.ny, -1.f, 1.f);
        const float slopeDegrees = std::acos(upDotNormal) * (180.f / Pi);
        const bool isSand = heightAboveSeaLevel <=
                settings.upperBeachHeightAboveSeaLevel &&
            slopeDegrees <= settings.sandAngleOfReposeDegrees;
        const bool isVegetation = !isSand && heightAboveSeaLevel >=
                settings.vegetationStartHeightAboveSeaLevel &&
            slopeDegrees <= settings.maximumVegetatedSlopeDegrees;
        const uint8_t sand = isSand ? 255u : 0u;
        const uint8_t vegetation = isVegetation ? 255u : 0u;
        const uint8_t rock = !isSand && !isVegetation ? 255u : 0u;
        const uint8_t waterCoat = isSand && heightAboveSeaLevel <=
            settings.maximumRunupHeightAboveSeaLevel ? 255u : 0u;

        renderVertices.push_back({ source.x, source.y, source.z,
            source.nx, source.ny, source.nz, source.u, source.v,
            PackMaterialWeights(sand, vegetation, rock, waterCoat) });
    }
    return true;
}

bool BeachBathymetryMetadata::IsValid() const
{
    return width >= 2u && height >= 2u &&
        IsFinite(minimumX) && IsFinite(maximumX) && minimumX < maximumX &&
        IsFinite(minimumZ) && IsFinite(maximumZ) && minimumZ < maximumZ &&
        IsFinite(minimumElevation) && IsFinite(maximumElevation) &&
            minimumElevation <= maximumElevation &&
        IsFinite(originX) && IsFinite(originZ) &&
        IsFinite(averageStepX) && averageStepX != 0.f &&
        IsFinite(averageStepZ) && averageStepZ != 0.f &&
        IsFinite(worldToTextureUScale) && worldToTextureUScale != 0.f &&
        IsFinite(worldToTextureUBias) &&
        IsFinite(worldToTextureVScale) && worldToTextureVScale != 0.f &&
        IsFinite(worldToTextureVBias);
}

bool BeachBathymetry::IsValid() const
{
    if (!metadata.IsValid() ||
        metadata.width > std::numeric_limits<size_t>::max() / metadata.height ||
        elevations.size() != size_t(metadata.width) * metadata.height)
        return false;
    return std::all_of(elevations.begin(), elevations.end(),
        [](float elevation) { return IsFinite(elevation); });
}

bool BuildBeachBathymetry(const std::vector<BeachVertex>& sourceVertices,
    BeachBathymetry& bathymetry)
{
    bathymetry = {};
    if (sourceVertices.size() < 4u) return false;
    for (const BeachVertex& vertex : sourceVertices)
        if (!IsFinite(vertex.x) || !IsFinite(vertex.y) || !IsFinite(vertex.z))
            return false;

    const float firstZ = sourceVertices.front().z;
    const float rowDetectionTolerance = std::max(1e-5f,
        std::max(std::abs(firstZ), 1.f) * 1e-6f);
    size_t width = 1u;
    while (width < sourceVertices.size() &&
        std::abs(sourceVertices[width].z - firstZ) <= rowDetectionTolerance)
        ++width;
    if (width < 2u || width == sourceVertices.size() ||
        sourceVertices.size() % width != 0u ||
        width > std::numeric_limits<uint32_t>::max())
        return false;
    const size_t height = sourceVertices.size() / width;
    if (height < 2u || height > std::numeric_limits<uint32_t>::max()) return false;

    const float originX = sourceVertices.front().x;
    const float finalX = sourceVertices[width - 1u].x;
    const float originZ = sourceVertices.front().z;
    const float finalZ = sourceVertices[(height - 1u) * width].z;
    const float averageStepX = (finalX - originX) / float(width - 1u);
    const float averageStepZ = (finalZ - originZ) / float(height - 1u);
    if (!IsFinite(averageStepX) || !IsFinite(averageStepZ) ||
        std::abs(averageStepX) <= 1e-7f || std::abs(averageStepZ) <= 1e-7f)
        return false;

    const float coordinateTolerance = std::max(1e-4f,
        std::min(std::abs(averageStepX), std::abs(averageStepZ)) * 0.075f);
    const float stepSignX = averageStepX > 0.f ? 1.f : -1.f;
    const float stepSignZ = averageStepZ > 0.f ? 1.f : -1.f;
    for (size_t row = 0; row < height; ++row)
    {
        const float rowZ = sourceVertices[row * width].z;
        const float expectedZ = originZ + averageStepZ * float(row);
        if (std::abs(rowZ - expectedZ) > coordinateTolerance) return false;
        if (row + 1u < height)
        {
            const float step = sourceVertices[(row + 1u) * width].z - rowZ;
            if (step * stepSignZ <= 0.f ||
                std::abs(step) < std::abs(averageStepZ) * 0.5f ||
                std::abs(step) > std::abs(averageStepZ) * 1.5f)
                return false;
        }
        for (size_t column = 0; column < width; ++column)
        {
            const BeachVertex& vertex = sourceVertices[row * width + column];
            if (std::abs(vertex.z - rowZ) > coordinateTolerance ||
                std::abs(vertex.x - sourceVertices[column].x) > coordinateTolerance)
                return false;
            if (column + 1u < width)
            {
                const float step = sourceVertices[row * width + column + 1u].x - vertex.x;
                if (step * stepSignX <= 0.f ||
                    std::abs(step) < std::abs(averageStepX) * 0.5f ||
                    std::abs(step) > std::abs(averageStepX) * 1.5f)
                    return false;
            }
        }
    }

    BeachBathymetry result;
    BeachBathymetryMetadata& metadata = result.metadata;
    metadata.width = static_cast<uint32_t>(width);
    metadata.height = static_cast<uint32_t>(height);
    metadata.minimumX = std::min(originX, finalX);
    metadata.maximumX = std::max(originX, finalX);
    metadata.minimumZ = std::min(originZ, finalZ);
    metadata.maximumZ = std::max(originZ, finalZ);
    metadata.minimumElevation = sourceVertices.front().y;
    metadata.maximumElevation = sourceVertices.front().y;
    metadata.originX = originX;
    metadata.originZ = originZ;
    metadata.averageStepX = averageStepX;
    metadata.averageStepZ = averageStepZ;
    metadata.worldToTextureUScale = 1.f / (finalX - originX);
    metadata.worldToTextureUBias = -originX * metadata.worldToTextureUScale;
    metadata.worldToTextureVScale = 1.f / (finalZ - originZ);
    metadata.worldToTextureVBias = -originZ * metadata.worldToTextureVScale;
    result.elevations.reserve(sourceVertices.size());
    for (const BeachVertex& vertex : sourceVertices)
    {
        result.elevations.push_back(vertex.y);
        metadata.minimumElevation = std::min(metadata.minimumElevation, vertex.y);
        metadata.maximumElevation = std::max(metadata.maximumElevation, vertex.y);
    }
    if (!result.IsValid()) return false;
    bathymetry = std::move(result);
    return true;
}

uint32_t BeachVegetationMesh::GetBladeCount() const
{
    return static_cast<uint32_t>(vertices.size() / 3u);
}

bool BeachVegetationMesh::IsValid() const
{
    if (vertices.size() % 3u != 0u ||
        indices.size() != vertices.size() ||
        vertices.size() / 3u > std::numeric_limits<uint32_t>::max())
        return false;
    for (const BeachVegetationVertex& vertex : vertices)
    {
        if (!IsFinite(vertex.rootX) || !IsFinite(vertex.rootY) ||
            !IsFinite(vertex.rootZ) || !IsFinite(vertex.facingX) ||
            !IsFinite(vertex.facingZ) || !IsFinite(vertex.sideOffset) ||
            !IsFinite(vertex.height) || !IsFinite(vertex.u) ||
            !IsFinite(vertex.v) || !IsFinite(vertex.windWeight))
            return false;
    }
    return std::all_of(indices.begin(), indices.end(),
        [this](uint32_t index) { return index < vertices.size(); });
}

bool BuildBeachVegetationMesh(
    const std::vector<BeachTerrainRenderVertex>& terrainVertices,
    const BeachBathymetryMetadata& bathymetry,
    const BeachVegetationSettings& settings,
    BeachVegetationMesh& vegetation)
{
    vegetation = {};
    const bool validSettings = IsFinite(settings.bladeDensityPerSquareMeter) &&
        settings.bladeDensityPerSquareMeter >= 0.f &&
        IsFinite(settings.minimumBladeHeight) && settings.minimumBladeHeight > 0.f &&
        IsFinite(settings.maximumBladeHeight) &&
            settings.maximumBladeHeight >= settings.minimumBladeHeight &&
        IsFinite(settings.minimumBladeHalfWidth) && settings.minimumBladeHalfWidth > 0.f &&
        IsFinite(settings.maximumBladeHalfWidth) &&
            settings.maximumBladeHalfWidth >= settings.minimumBladeHalfWidth &&
        settings.maximumBladeCount <= std::numeric_limits<uint32_t>::max() / 3u;
    if (!validSettings || !bathymetry.IsValid() ||
        bathymetry.width > std::numeric_limits<size_t>::max() / bathymetry.height ||
        terrainVertices.size() != size_t(bathymetry.width) * bathymetry.height)
        return false;
    if (settings.maximumBladeCount == 0u || settings.bladeDensityPerSquareMeter == 0.f)
        return true;

    const float cellArea = std::abs(bathymetry.averageStepX * bathymetry.averageStepZ);
    if (!IsFinite(cellArea) || cellArea <= 0.f) return false;
    const float acceptance = std::clamp(
        settings.bladeDensityPerSquareMeter * cellArea, 0.f, 1.f);

    struct Candidate
    {
        uint32_t vertexIndex;
        uint32_t priority;
    };
    std::vector<Candidate> candidates;
    for (size_t index = 0; index < terrainVertices.size(); ++index)
    {
        if (index > std::numeric_limits<uint32_t>::max()) return false;
        const BeachTerrainRenderVertex& vertex = terrainVertices[index];
        const uint8_t sand = GetBeachTerrainMaterialWeight(
            vertex.packedMaterialWeights, BeachTerrainMaterialChannel::Sand);
        const uint8_t vegetationWeight = GetBeachTerrainMaterialWeight(
            vertex.packedMaterialWeights, BeachTerrainMaterialChannel::Vegetation);
        const uint8_t rock = GetBeachTerrainMaterialWeight(
            vertex.packedMaterialWeights, BeachTerrainMaterialChannel::Rock);
        if (sand != 0u || vegetationWeight != 255u || rock != 0u) continue;
        const uint32_t sourceIndex = static_cast<uint32_t>(index);
        const uint32_t selectionHash = Hash32(sourceIndex ^ settings.randomSeed);
        if (UnitFloat(selectionHash) >= acceptance) continue;
        candidates.push_back({ sourceIndex,
            Hash32(selectionHash ^ 0xa511e9b3u) });
    }
    std::sort(candidates.begin(), candidates.end(),
        [](const Candidate& left, const Candidate& right)
        {
            return left.priority < right.priority ||
                (left.priority == right.priority &&
                    left.vertexIndex < right.vertexIndex);
        });
    if (candidates.size() > settings.maximumBladeCount)
        candidates.resize(settings.maximumBladeCount);

    vegetation.vertices.reserve(candidates.size() * 3u);
    vegetation.indices.reserve(candidates.size() * 3u);
    for (const Candidate& candidate : candidates)
    {
        const BeachTerrainRenderVertex& root = terrainVertices[candidate.vertexIndex];
        if (!IsFinite(root.x) || !IsFinite(root.y) || !IsFinite(root.z))
        {
            vegetation = {};
            return false;
        }
        const float angle = 2.f * Pi * UnitFloat(Hash32(
            candidate.vertexIndex ^ settings.randomSeed ^ 0x63d83595u));
        const float facingX = std::cos(angle);
        const float facingZ = std::sin(angle);
        const float height = Interpolate(settings.minimumBladeHeight,
            settings.maximumBladeHeight, UnitFloat(Hash32(
                candidate.vertexIndex ^ settings.randomSeed ^ 0x9e3779b9u)));
        const float halfWidth = Interpolate(settings.minimumBladeHalfWidth,
            settings.maximumBladeHalfWidth, UnitFloat(Hash32(
                candidate.vertexIndex ^ settings.randomSeed ^ 0x85ebca6bu)));
        const uint32_t baseVertex = static_cast<uint32_t>(vegetation.vertices.size());
        vegetation.vertices.push_back({ root.x, root.y, root.z, facingX, facingZ,
            -halfWidth, 0.f, 0.f, 1.f, 0.f });
        vegetation.vertices.push_back({ root.x, root.y, root.z, facingX, facingZ,
            halfWidth, 0.f, 1.f, 1.f, 0.f });
        vegetation.vertices.push_back({ root.x, root.y, root.z, facingX, facingZ,
            0.f, height, 0.5f, 0.f, 1.f });
        // The vegetation pipeline uses CULL_NONE. Keeping one winding lets the
        // pixel shader distinguish the transmitted back face with SV_IsFrontFace.
        vegetation.indices.insert(vegetation.indices.end(), {
            baseVertex, baseVertex + 1u, baseVertex + 2u });
    }
    return vegetation.IsValid();
}

bool BuildBeachSurfDisturbances(const BeachSurfSimulationSettings& settings,
    float deltaTimeSeconds, double simulationTimeSeconds,
    std::vector<gfsdk_float4>& disturbances)
{
    disturbances.clear();
    const bool validSettings = settings.domainGridSize >= 64u &&
        settings.domainGridSize <= 2048u && IsPowerOfTwo(settings.domainGridSize) &&
        IsFinite(settings.domainCenterX) && IsFinite(settings.domainCenterZ) &&
        IsFinite(settings.domainWorldspaceSize) && settings.domainWorldspaceSize > 0.f &&
        IsFinite(settings.forcingLineCenterX) && IsFinite(settings.forcingLineCenterZ) &&
        IsFinite(settings.forcingLineTangentX) && IsFinite(settings.forcingLineTangentZ) &&
        IsFinite(settings.forcingLineLength) && settings.forcingLineLength > 0.f &&
        IsFinite(settings.forcingLineSeparation) && settings.forcingLineSeparation > 0.f &&
        settings.samplesPerLine >= 3u &&
        settings.samplesPerLine <= MaxBeachSurfSamplesPerLine &&
        IsFinite(settings.temporalFrequencyHz) && settings.temporalFrequencyHz > 0.f &&
        IsFinite(settings.secondLinePhaseOffsetRadians) &&
        IsFinite(settings.displacementForcingRate) &&
            settings.displacementForcingRate >= 0.f &&
        IsFinite(settings.velocityPotentialForcingRate) &&
            settings.velocityPotentialForcingRate >= 0.f &&
        (settings.displacementForcingRate > 0.f ||
            settings.velocityPotentialForcingRate > 0.f) &&
        IsFinite(deltaTimeSeconds) && deltaTimeSeconds > 0.f &&
        std::isfinite(simulationTimeSeconds);
    if (!validSettings) return false;

    const float tangentLength = std::sqrt(
        settings.forcingLineTangentX * settings.forcingLineTangentX +
        settings.forcingLineTangentZ * settings.forcingLineTangentZ);
    if (!IsFinite(tangentLength) || tangentLength <= 1e-6f) return false;
    const float tangentX = settings.forcingLineTangentX / tangentLength;
    const float tangentZ = settings.forcingLineTangentZ / tangentLength;
    const float normalX = -tangentZ;
    const float normalZ = tangentX;
    const double phaseValue = 2.0 * double(Pi) *
        double(settings.temporalFrequencyHz) * simulationTimeSeconds;
    if (!std::isfinite(phaseValue)) return false;
    const float basePhase = static_cast<float>(std::remainder(
        phaseValue, 2.0 * double(Pi)));

    disturbances.reserve(size_t(settings.samplesPerLine) * 2u);
    for (uint32_t line = 0; line < 2u; ++line)
    {
        const float signedOffset = (line == 0u ? -0.5f : 0.5f) *
            settings.forcingLineSeparation;
        const float centerX = settings.forcingLineCenterX + normalX * signedOffset;
        const float centerZ = settings.forcingLineCenterZ + normalZ * signedOffset;
        const double phase = basePhase + double(line) *
            settings.secondLinePhaseOffsetRadians;
        const double omega = 2.0 * double(Pi) * settings.temporalFrequencyHz;
        const double endPhase = phase + omega * deltaTimeSeconds;
        // Integrate the wavemaker over the time step, so forcing impulses do
        // not depend on render cadence (the solver itself still has a time step).
        const float sineIntegral = float((std::cos(phase) - std::cos(endPhase)) / omega);
        const float cosineIntegral = float((std::sin(endPhase) - std::sin(phase)) / omega);
        for (uint32_t sample = 0; sample < settings.samplesPerLine; ++sample)
        {
            const float lineAmount = float(sample) /
                float(settings.samplesPerLine - 1u);
            const float alongLine = (lineAmount - 0.5f) * settings.forcingLineLength;
            const float worldX = centerX + tangentX * alongLine;
            const float worldZ = centerZ + tangentZ * alongLine;
            float gridX = 0.f;
            float gridY = 0.f;
            if (!WorldToGrid(settings, worldX, worldZ, gridX, gridY))
            {
                disturbances.clear();
                return false;
            }
            const float window = sample == 0u || sample + 1u == settings.samplesPerLine
                ? 0.f : 0.5f - 0.5f * std::cos(2.f * Pi * lineAmount);
            const float displacement = settings.displacementForcingRate *
                sineIntegral * window;
            const float velocityPotential = settings.velocityPotentialForcingRate *
                cosineIntegral * window;
            disturbances.push_back({ gridX, gridY, displacement, velocityPotential });
        }
    }
    // Coarse domains can map several source samples onto one cell. The GPU
    // injection uses non-atomic writes, so submit each cell only once.
    std::sort(disturbances.begin(), disturbances.end(),
        [](const gfsdk_float4& a, const gfsdk_float4& b) {
            return a.y < b.y || (a.y == b.y && a.x < b.x);
        });
    size_t count = 0;
    for (const auto& impulse : disturbances)
    {
        if (count && disturbances[count - 1].x == impulse.x &&
            disturbances[count - 1].y == impulse.y)
        {
            disturbances[count - 1].z += impulse.z;
            disturbances[count - 1].w += impulse.w;
        }
        else disturbances[count++] = impulse;
    }
    disturbances.resize(count);
    return true;
}
