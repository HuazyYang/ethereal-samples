// SPDX-License-Identifier: Apache-2.0
#include "ResampleVolume.h"
#include "ComputeRuntime.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <utility>
namespace gvdb::samples {
using namespace slang;
ResampleVolume::ResampleVolume(ethereal::slang::ComputeRuntime& runtime, const std::vector<float>& values,
                               ResampleConfiguration configuration)
    : m_runtime(&runtime), m_configuration(std::move(configuration)) {
    const auto source = m_configuration.sourceResolution, destination = m_configuration.destinationResolution;
    if (!source.x || !source.y || !source.z || uint64_t(source.x) * source.y > UINT32_MAX ||
        uint64_t(source.x) * source.y * source.z != values.size() || values.size() > UINT32_MAX / 4 ||
        m_configuration.logDimensions.empty() || m_configuration.logDimensions[0] < 1 ||
        m_configuration.logDimensions[0] > 10 || !std::isfinite(m_configuration.threshold) ||
        m_configuration.threshold < 0)
        throw std::invalid_argument("Resample source dimensions, threshold or leaf size are invalid");
    const uint32_t width = 1u << m_configuration.logDimensions[0];
    if (!destination.x || !destination.y || !destination.z || destination.x % width || destination.y % width ||
        destination.z % width || uint64_t(destination.x) * destination.y > UINT32_MAX ||
        uint64_t(destination.x) * destination.y * destination.z > UINT32_MAX / 4)
        throw std::invalid_argument(
            "Resample destination must contain complete leaves within portable addressing");
    if (!std::all_of(values.begin(), values.end(), [](float value) { return std::isfinite(value); }))
        throw std::invalid_argument("Resample source contains nonfinite values");
    m_coarseResolution = {destination.x / width, destination.y / width, destination.z / width, 0};
    m_source = runtime.buffer(values.size() * 4, 4, values.data());
}
void ResampleVolume::rebuild(bool sparse, bool halo) {
    const auto resolution = m_coarseResolution, destination = m_configuration.destinationResolution;
    const uint32_t width = 1u << m_configuration.logDimensions[0];
    const size_t count = size_t(resolution.x) * resolution.y * resolution.z;
    std::vector<float> coarse;
    std::vector<uint8_t> active(count, sparse ? 0u : 1u);
    if (sparse) {
        coarse = downsampleRegion(*m_runtime, m_source, m_configuration.sourceResolution, resolution,
                                  {float(destination.x), float(destination.y), float(destination.z), 0},
                                  m_configuration.transform, {0, 1, 0, 1});
        for (size_t i = 0; i < count; ++i)
            active[i] = coarse[i] > m_configuration.threshold ? 1u : 0u;
        if (halo) {
            auto expanded = active;
            for (uint32_t z = 0; z < resolution.z; ++z)
                for (uint32_t y = 0; y < resolution.y; ++y)
                    for (uint32_t x = 0; x < resolution.x; ++x) {
                        const size_t i = (size_t(z) * resolution.y + y) * resolution.x + x;
                        if (!active[i])
                            continue;
                        if (x)
                            expanded[i - 1] = 1;
                        if (x + 1 < resolution.x)
                            expanded[i + 1] = 1;
                        if (y)
                            expanded[i - resolution.x] = 1;
                        if (y + 1 < resolution.y)
                            expanded[i + resolution.x] = 1;
                        const size_t plane = size_t(resolution.x) * resolution.y;
                        if (z)
                            expanded[i - plane] = 1;
                        if (z + 1 < resolution.z)
                            expanded[i + plane] = 1;
                    }
            active = std::move(expanded);
        }
    }
    std::vector<Int4> origins;
    for (uint32_t z = 0; z < resolution.z; ++z)
        for (uint32_t y = 0; y < resolution.y; ++y)
            for (uint32_t x = 0; x < resolution.x; ++x)
                if (active[(size_t(z) * resolution.y + y) * resolution.x + x])
                    origins.push_back({int32_t(x * width), int32_t(y * width), int32_t(z * width), 0});
    TopologyBuildOptions options;
    options.logDimensions = m_configuration.logDimensions;
    PackedVolume packed;
    std::string error;
    if (!buildTopology(origins, options, packed, error))
        throw std::runtime_error("Resample topology: " + error);
    const auto atlasResolution = packed.volume.atlasResolutionAndApron;
    const uint64_t voxels = uint64_t(atlasResolution.x) * atlasResolution.y * atlasResolution.z;
    if (voxels > UINT32_MAX / 4)
        throw std::invalid_argument("Resample atlas exceeds portable scalar storage");
    std::vector<float> empty(size_t(voxels), 0);
    auto volume = std::make_unique<SlangVolume>(*m_runtime, packed, empty.data(), empty.size());
    volume->resample(m_source, m_configuration.sourceResolution, m_configuration.transform, {0, 1, 0, 1}, 0);
    m_volume = std::move(volume);
    m_packed = std::move(packed);
    m_active = std::move(active);
    m_coarseValues = std::move(coarse);
    m_sparse = sparse;
    m_halo = halo;
}
std::vector<float> loadRawBytes(const std::filesystem::path& path, UInt4 resolution) {
    const uint64_t count = uint64_t(resolution.x) * resolution.y * resolution.z;
    if (!resolution.x || !resolution.y || !resolution.z || uint64_t(resolution.x) * resolution.y > UINT32_MAX ||
        count > UINT32_MAX / 4)
        throw std::invalid_argument("RAW scalar dimensions exceed portable storage");
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        throw std::runtime_error("Cannot open RAW volume: " + path.string());
    if (file.tellg() != std::streamoff(count))
        throw std::runtime_error("RAW volume byte count does not match its dimensions");
    file.seekg(0);
    std::vector<uint8_t> bytes(size_t(count), 0);
    file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(count));
    if (!file)
        throw std::runtime_error("Cannot read complete RAW volume");
    std::vector<float> values(bytes.size());
    for (size_t i = 0; i < bytes.size(); ++i)
        values[i] = float(bytes[i]) / 256.f;
    return values;
}
std::vector<Float4> resampleTransferFunction() {
    struct Stop {
        float position;
        Float4 value;
    };
    const Stop stops[] = {
        {0,    {0, 0, 0, 0}         },
        {.15f, {0, 0, 0, 0}         },
        {.25f, {0, 0, 1, .01f}      },
        {.5f,  {1, 0, 0, .02f}      },
        {.75f, {.2f, .2f, .2f, .02f}},
        {1,    {.1f, .1f, .1f, .1f} }
    };
    std::vector<Float4> values(256);
    size_t segment = 0;
    for (size_t i = 0; i < values.size(); ++i) {
        const float x = float(i) / 255;
        while (segment + 2 < std::size(stops) && x > stops[segment + 1].position)
            ++segment;
        const auto& a = stops[segment];
        const auto& b = stops[segment + 1];
        const float t = (x - a.position) / (b.position - a.position);
        values[i] = {a.value.x + (b.value.x - a.value.x) * t, a.value.y + (b.value.y - a.value.y) * t,
                     a.value.z + (b.value.z - a.value.z) * t, a.value.w + (b.value.w - a.value.w) * t};
    }
    return values;
}
}  // namespace gvdb::samples
