// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "GVDBNvrhiVolume.h"
namespace gvdb::portable {
struct ResampleOperatorDesc {
    NvrhiVolume* volume = nullptr; // Borrowed; public facade retains the owner.
    const void* artifact = nullptr;
    size_t artifactBytes = 0;
};
struct ResampleSource {
    const void* data = nullptr; // Dense X-fastest Float32 storage, copied by prepare.
    uint64_t byteSize = 0;
    UInt3 resolution{};
};
struct ResampleRequest {
    uint32_t channel = 0;
    ResampleSource source;
    Float4 transformRows[3]{{1,0,0,0},{0,1,0,0},{0,0,1,0}};
    Float4 remap{0,1,0,1}; // Input min/max, output min/max.
    bool preserveOutside = true;
    float background = 0;
};
class NvrhiResample {
 public:
    ~NvrhiResample();
    NvrhiResample(const NvrhiResample&) = delete;
    NvrhiResample& operator=(const NvrhiResample&) = delete;
    static nvrhi::FRESULT create(const ResampleOperatorDesc&, std::unique_ptr<NvrhiResample>& output) noexcept;
    // Requires a current, finished publication and R32_FLOAT channel. A fresh raw
    // allocation receives every active slot's interior and apron. Unused slots are
    // zero. Source coordinates are affine-transformed integer voxel positions and
    // truncated toward zero, as in retained gvdbResample. Out-of-range coordinates
    // preserve exact old bits by default, or use the explicit background. Nonfinite
    // transformed coordinates are outside; no undefined float-to-int conversion.
    // Input values may be nonfinite and follow the remap's floating-point arithmetic.
    // Copies input during preparation, no commands/submission/waits. Publication uses
    // the owner's existing recordAndPublish; retained old snapshots stay unchanged.
    // Empty atlas returns a one-use no-op after descriptor/authentication checks.
    nvrhi::FRESULT prepare(const ResampleRequest&, const PreparationBudget&,
        std::unique_ptr<PreparedGeneration>& output) noexcept;
 private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    explicit NvrhiResample(std::unique_ptr<Impl>) noexcept;
};
} // namespace gvdb::portable
