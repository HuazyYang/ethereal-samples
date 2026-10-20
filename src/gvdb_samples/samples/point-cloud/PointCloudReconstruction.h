// Per-frame point-cloud reconstruction using the unified NVRHI runtime.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "PointCloudInput.h"
#include <gvdb/GVDBNvrhiApi.h>
#include <gvdb/slang/GVDBPortableData.h>
namespace ethereal::slang {
class ComputeRuntime;
}
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
    struct State;
    std::unique_ptr<State> m_state;
    std::unique_ptr<State> createState(bool scalarChannel = true) const;
    std::vector<slang::Float4> m_hostPositions;
    QuantizationBounds m_quantization{};
    uint64_t m_generation = 0;
    uint32_t m_keyBatches = 0;

 public:
    explicit PointCloudReconstruction(ethereal::slang::ComputeRuntime& runtime);
    ~PointCloudReconstruction();
    // Successful reconstruction replaces the previous frame atomically. Failure
    // leaves the previous volume available. There is no accumulation mode.
    void reconstruct(const PointFrame& frame, const ReconstructionSettings& settings = {});
    bool hasVolume() const { return m_generation != 0; }
    portable::SnapshotHandle snapshot() const;
    const portable::IGVDBHostVolume& topology() const;
    std::vector<float> readAtlas() const;
    // Explicit diagnostic readback of the publication; never used for reconstruction/rendering.
    slang::PackedVolume readTopology() const;
    std::vector<uint16_t> quantizationCodes() const;
    const std::vector<slang::Float4>& hostPositions() const { return m_hostPositions; }
    const QuantizationBounds& quantization() const { return m_quantization; }
    uint64_t generation() const { return m_generation; }
    uint32_t topologyKeyBatches() const { return m_keyBatches; }
    std::vector<PointLine> pointLines() const;
};
}  // namespace gvdb::samples::pointcloud
