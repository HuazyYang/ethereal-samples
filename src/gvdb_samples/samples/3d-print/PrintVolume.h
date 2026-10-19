// Indexed mesh -> GPU solid voxels -> sparse scalar topology, without gp/CUDA setup.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <gvdb/GVDBNvrhiApi.h>
#include <sample-utils/ObjLoader.h>
#include <memory>
#include <filesystem>
namespace ethereal::slang {
class ComputeRuntime;
}
namespace gvdb::samples {
struct PrintConfiguration {
    float voxelSize = .5f, partSize = 100, rotationDegrees = -10, margin = .1f;
    slang::Float4 pivot{.3f, .45f, .3f, 0};
};
class PrintVolume {
    ethereal::slang::ComputeRuntime* m_runtime;
    std::vector<slang::Float4> m_vertices;
    std::vector<uint32_t> m_indices;
    donut::math::box3 m_bounds;
    PrintConfiguration m_configuration;
    struct State;
    std::unique_ptr<State> m_state;
    std::vector<float> m_dense;
    slang::UInt4 m_resolution{};
    slang::Int4 m_origin{};
    size_t m_surfaceCount = 0, m_interiorCount = 0;

 public:
    PrintVolume(ethereal::slang::ComputeRuntime& runtime, const SampleUtils::TriangleMesh& mesh,
                PrintConfiguration configuration = {});
    void rebuild(float voxelSize);
    ~PrintVolume();
    portable::SnapshotHandle snapshot() const;
    const portable::IGVDBHostVolume& topology() const;
    std::vector<float> readAtlas();
    std::vector<slang::AtlasNode> readAtlasMap();
    const std::vector<float>& dense() const { return m_dense; }
    slang::UInt4 resolution() const { return m_resolution; }
    slang::Int4 origin() const { return m_origin; }
    float voxelSize() const { return m_configuration.voxelSize; }
    size_t surfaceCount() const { return m_surfaceCount; }
    size_t interiorCount() const { return m_interiorCount; }
};
SampleUtils::TriangleMesh loadPrintMesh(const std::filesystem::path& path);
std::vector<slang::Float4> printTransferFunction();
}  // namespace gvdb::samples
