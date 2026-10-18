// SPDX-License-Identifier: Apache-2.0
#include "PointFusionScanner.h"
#include "ComputeRuntime.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace gvdb::samples {
using nvrhi::BindingSetItem;
namespace {
void finite(slang::Float4 p) {
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || !std::isfinite(p.w))
        throw std::invalid_argument("Scanner coordinates must be finite");
}
}  // namespace
PointFusionScanner::PointFusionScanner(ethereal::slang::ComputeRuntime& runtime, uint32_t width,
                                       uint32_t height, const std::vector<ScanBox>& boxes, uint32_t seed)
    : m_runtime(&runtime), m_width(width), m_height(height) {
    if (!width || !height || uint64_t(width) * height > 65535u * 256u || width > 65535u * 8u ||
        height > 65535u * 8u || boxes.size() > UINT32_MAX / sizeof(ScanBox))
        throw std::invalid_argument("Scanner dimensions exceed portable dispatch or buffer ranges");
    m_count = width * height;
    m_boxCount = uint32_t(boxes.size());
    for (const auto& box : boxes) {
        finite(box.minimum);
        finite(box.maximum);
        if (box.minimum.x > box.maximum.x || box.minimum.y > box.maximum.y || box.minimum.z > box.maximum.z)
            throw std::invalid_argument("Scanner box bounds are reversed");
    }
    const ScanBox empty{};
    m_boxes = runtime.buffer(std::max(size_t(1), boxes.size()) * sizeof(ScanBox), sizeof(ScanBox),
                             boxes.empty() ? &empty : boxes.data());
    std::vector<uint32_t> seeds(m_count);
    for (uint32_t i = 0; i < m_count; ++i)
        seeds[i] = seed ^ ((i + 1u) * 0x9e3779b9u);
    m_seeds = runtime.buffer(size_t(m_count) * 4, 4, seeds.data());
    m_positions = runtime.buffer(size_t(m_count) * 16, 16);
    m_colors = runtime.buffer(size_t(m_count) * 4, 4);
    m_valid = runtime.buffer(size_t(m_count) * 4, 4);
}
ScanFrame PointFusionScanner::scan(ScanParameters p) {
    finite(p.originAndMaximum);
    finite(p.direction00);
    finite(p.directionDx);
    finite(p.directionDy);
    finite(p.cityMinimum);
    finite(p.cityMaximum);
    if (p.originAndMaximum.w <= 0 || p.cityMinimum.x >= p.cityMaximum.x || p.cityMinimum.z >= p.cityMaximum.z ||
        p.options.x > 1)
        throw std::invalid_argument("Scanner range, city bounds or jitter option is invalid");
    p.counts = {m_width, m_height, m_boxCount, m_count};
    m_runtime->dispatch(
        "PointFusionScan", "scanBuildings", &p, sizeof(p),
        {BindingSetItem::StructuredBuffer_SRV(0, m_boxes), BindingSetItem::StructuredBuffer_UAV(0, m_positions),
         BindingSetItem::StructuredBuffer_UAV(1, m_colors), BindingSetItem::StructuredBuffer_UAV(2, m_valid),
         BindingSetItem::StructuredBuffer_UAV(3, m_seeds)},
        m_width / 8 + uint32_t(m_width % 8 != 0), m_height / 8 + uint32_t(m_height % 8 != 0));
    auto offsets = m_runtime->exclusiveScan(m_valid, m_count);
    ScanFrame result;
    result.totalPixels = m_count;
    result.positions = m_runtime->buffer(size_t(m_count) * 16, 16);
    result.colors = m_runtime->buffer(size_t(m_count) * 4, 4);
    result.pixels = m_runtime->buffer(size_t(m_count) * 4, 4);
    auto total = m_runtime->buffer(4, 4);
    m_runtime->dispatch(
        "PointFusionScan", "compactScanHits", &p, sizeof(p),
        {BindingSetItem::StructuredBuffer_SRV(1, m_positions),
         BindingSetItem::StructuredBuffer_SRV(2, m_colors), BindingSetItem::StructuredBuffer_SRV(3, m_valid),
         BindingSetItem::StructuredBuffer_SRV(4, offsets),
         BindingSetItem::StructuredBuffer_UAV(0, result.positions),
         BindingSetItem::StructuredBuffer_UAV(1, result.colors),
         BindingSetItem::StructuredBuffer_UAV(2, result.pixels),
         BindingSetItem::StructuredBuffer_UAV(3, total)},
        m_count / 256 + uint32_t(m_count % 256 != 0));
    m_runtime->read(total, &result.count, 4);
    return result;
}
std::vector<uint32_t> PointFusionScanner::readSeeds() {
    std::vector<uint32_t> result(m_count);
    m_runtime->read(m_seeds, result.data(), result.size() * 4);
    return result;
}
}  // namespace gvdb::samples
