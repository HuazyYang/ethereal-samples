// Deterministic procedural-city scanner on the public NVRHI compute runtime.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <gvdb/slang/GVDBPortable.h>
#include <nvrhi/nvrhi.h>
#include <vector>
namespace ethereal::slang {
class ComputeRuntime;
}
namespace gvdb::samples {
struct alignas(16) ScanBox {
    slang::Float4 minimum, maximum;
    slang::UInt4 color;  // packed RGBA in x
};
struct alignas(16) ScanParameters {
    slang::UInt4 counts;  // width,height,boxes,pixels (filled by host)
    slang::Float4 originAndMaximum;
    slang::Float4 direction00, directionDx, directionDy;  // normalized pixel coordinates
    slang::Float4 cityMinimum, cityMaximum;               // x/z gate; y ignored
    slang::UInt4 options;                                 // jitter enabled, remaining reserved
};
static_assert(sizeof(ScanBox) == 48 && sizeof(ScanParameters) == 128);
struct ScanFrame {
    nvrhi::BufferHandle positions, colors, pixels;
    uint32_t count = 0, totalPixels = 0;
};
class PointFusionScanner {
    ethereal::slang::ComputeRuntime* m_runtime;
    uint32_t m_width, m_height, m_count, m_boxCount;
    nvrhi::BufferHandle m_boxes, m_seeds, m_positions, m_colors, m_valid;

 public:
    PointFusionScanner(ethereal::slang::ComputeRuntime&, uint32_t width, uint32_t height,
                       const std::vector<ScanBox>& boxes, uint32_t seed = 1043);
    ScanFrame scan(ScanParameters parameters);
    std::vector<uint32_t> readSeeds();
};
}  // namespace gvdb::samples
