// Dense RAW data -> dense/sparse/halo GVDB topology through the shared runtime.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <gvdb/GVDBNvrhiDownsampleApi.h>
#include <memory>
#include <array>
#include <vector>
#include <filesystem>
namespace ethereal::slang {
class ComputeRuntime;
}
namespace gvdb::samples {
struct ResampleConfiguration {
    slang::UInt4 sourceResolution{128, 256, 256, 0};
    slang::UInt4 destinationResolution{256, 256, 256, 0};
    std::vector<uint32_t> logDimensions{4, 3, 3, 3, 3};
    std::array<slang::Float4, 3> transform = {
        slang::Float4{.5f, 0,  0, 0  },
        slang::Float4{0,   0,  1, 0  },
        slang::Float4{0,   -1, 0, 252}
    };
    float threshold = .05f;
};
class ResampleVolume {
    ethereal::slang::ComputeRuntime* m_runtime;
    ResampleConfiguration m_configuration;
    struct State;
    std::unique_ptr<State> m_state;
    std::vector<float> m_source;
    std::vector<float> m_coarseValues;
    std::vector<uint8_t> m_active;
    slang::UInt4 m_coarseResolution{};
    bool m_sparse = false, m_halo = true;

 public:
    ResampleVolume(ethereal::slang::ComputeRuntime& runtime, const std::vector<float>& values,
                   ResampleConfiguration configuration = {});
    void rebuild(bool sparse, bool halo);
    ~ResampleVolume();
    portable::SnapshotHandle snapshot() const;
    const portable::IGVDBHostVolume& topology() const;
    // Verification only: copies the retained snapshot channel through explicit readback.
    std::vector<float> readAtlas();
    std::vector<slang::AtlasNode> readAtlasMap();
    const ResampleConfiguration& configuration() const { return m_configuration; }
    const std::vector<float>& coarseValues() const { return m_coarseValues; }
    const std::vector<uint8_t>& activeCells() const { return m_active; }
    slang::UInt4 coarseResolution() const { return m_coarseResolution; }
    bool sparse() const { return m_sparse; }
    bool halo() const { return m_halo; }
};
std::vector<float> queryResampleRegions(ethereal::slang::ComputeRuntime&, const std::vector<float>&,
                                        slang::UInt4 source, slang::UInt4 destination, slang::Float4 extent,
                                        const std::array<slang::Float4, 3>&);
std::vector<float> loadRawBytes(const std::filesystem::path& path, slang::UInt4 resolution);
std::vector<slang::Float4> resampleTransferFunction();
}  // namespace gvdb::samples
