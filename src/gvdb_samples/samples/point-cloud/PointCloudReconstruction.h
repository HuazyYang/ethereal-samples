// Per-frame point-cloud reconstruction using the unified NVRHI runtime.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "PointCloudInput.h"
#include <gvdb/GVDBSlangVolume.h>
#include <memory>
namespace gvdb::samples::pointcloud {
struct ReconstructionSettings {
    float renderScale = 1;
    uint32_t smoothingIterations = 0;
    float smoothingCenterWeight = 0, smoothingBias = 0;
};
struct PointLine {
    slang::Float4 start, end, color;
};
class PointCloudReconstruction {
    ethereal::slang::ComputeRuntime* m_runtime;
    slang::PackedVolume m_packed;
    std::unique_ptr<slang::SlangVolume> m_volume;
    nvrhi::BufferHandle m_positions;
    std::vector<slang::Float4> m_hostPositions;
    QuantizationBounds m_quantization{};
    uint64_t m_generation = 0;

 public:
    explicit PointCloudReconstruction(ethereal::slang::ComputeRuntime& runtime)
        : m_runtime(&runtime) {}
    // Successful reconstruction replaces the previous frame atomically. Failure
    // leaves the previous volume available. There is no accumulation mode.
    void reconstruct(const PointFrame& frame, const ReconstructionSettings& settings = {});
    bool hasVolume() const { return bool(m_volume); }
    slang::SlangVolume& volume() const;
    const slang::PackedVolume& packed() const { return m_packed; }
    nvrhi::IBuffer* positions() const { return m_positions; }
    const std::vector<slang::Float4>& hostPositions() const { return m_hostPositions; }
    const QuantizationBounds& quantization() const { return m_quantization; }
    uint64_t generation() const { return m_generation; }
    std::vector<PointLine> pointLines() const;
};
}  // namespace gvdb::samples::pointcloud
