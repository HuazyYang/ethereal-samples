// Persistent sparse point fusion; topology orchestration uses the CPU, voxel math Slang.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "PointFusionScanner.h"
#include <gvdb/GVDBSlangVolume.h>
#include <gvdb/GVDBSlangAtlas.h>
#include <memory>
#include <set>
#include <tuple>
namespace gvdb::samples {
struct PointFusionConfiguration {
    float radius = 1, support = 2;
    bool color = false;
};
class PointFusionVolume {
    using Coordinate = std::tuple<int32_t, int32_t, int32_t>;
    ethereal::slang::ComputeRuntime* m_runtime;
    PointFusionConfiguration m_configuration;
    std::set<Coordinate> m_leaves;
    slang::PackedVolume m_packed;
    std::unique_ptr<slang::SlangVolume> m_volume;
    std::unique_ptr<slang::SlangAtlas> m_color;
    uint64_t m_frames = 0, m_hits = 0;
    std::vector<slang::Float4> m_lastPositions;
    std::vector<uint32_t> m_lastColors;

 public:
    explicit PointFusionVolume(ethereal::slang::ComputeRuntime&, PointFusionConfiguration = {});
    // Input is compact and valid; positions at the origin are accepted. Empty
    // frames advance statistics but do not alter any existing atlas or topology.
    void fuse(const ScanFrame& frame);
    void reset(bool color);
    bool hasVolume() const { return bool(m_volume); }
    slang::SlangVolume& volume() const;
    const slang::PackedVolume& packed() const { return m_packed; }
    slang::SlangAtlas* colorAtlas() const { return m_color.get(); }
    bool useColor() const { return m_configuration.color; }
    const std::vector<slang::Float4>& lastPositions() const { return m_lastPositions; }
    const std::vector<uint32_t>& lastColors() const { return m_lastColors; }
    uint64_t frames() const { return m_frames; }
    uint64_t totalHits() const { return m_hits; }
};
}  // namespace gvdb::samples
