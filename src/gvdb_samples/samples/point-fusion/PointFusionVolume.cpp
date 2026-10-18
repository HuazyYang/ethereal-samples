// SPDX-License-Identifier: Apache-2.0
#include "PointFusionVolume.h"
#include <gvdb/GVDBSlangPointFusion.h>
#include <gvdb/slang/GVDBTopologyBuilder.h>
#include "ComputeRuntime.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
namespace gvdb::samples {
using namespace slang;
using nvrhi::BindingSetItem;
namespace {
using Coordinate = std::tuple<int32_t, int32_t, int32_t>;
struct alignas(16) UpdateParameters {
    VolumeInfo previous, current;
    UInt4 operation;
};
static_assert(sizeof(UpdateParameters) == 176);
Coordinate key(int32_t x, int32_t y, int32_t z) { return {x, y, z}; }
int32_t floorCell(float value, int32_t width) { return int32_t(std::floor(double(value) / width)) * width; }
uint32_t groups(uint32_t count) { return count / 256 + uint32_t(count % 256 != 0); }
void dispatch1D(ethereal::slang::ComputeRuntime& runtime, const char* entry, const UpdateParameters& p,
                const std::vector<BindingSetItem>& bindings, uint32_t count) {
    const auto n = groups(count);
    runtime.dispatch("PointFusionUpdate", entry, &p, sizeof(p), bindings, std::min(n, 65535u),
                     n / 65535u + uint32_t(n % 65535u != 0));
}
}  // namespace
PointFusionVolume::PointFusionVolume(ethereal::slang::ComputeRuntime& runtime,
                                     PointFusionConfiguration configuration)
    : m_runtime(&runtime), m_configuration(configuration) {
    if (!std::isfinite(configuration.radius) || !std::isfinite(configuration.support) ||
        configuration.radius <= 0 || configuration.support < configuration.radius || configuration.support > 32)
        throw std::invalid_argument("Point fusion requires 0 < radius <= support <= 32");
}
SlangVolume& PointFusionVolume::volume() const {
    if (!m_volume)
        throw std::logic_error("Point fusion has no valid scans yet");
    return *m_volume;
}
void PointFusionVolume::reset(bool color) {
    m_configuration.color = color;
    m_leaves.clear();
    m_packed = {};
    m_volume.reset();
    m_color.reset();
    m_frames = 0;
    m_hits = 0;
    m_lastPositions.clear();
    m_lastColors.clear();
}
void PointFusionVolume::fuse(const ScanFrame& frame) {
    if (!frame.count) {
        ++m_frames;
        m_lastPositions.clear();
        m_lastColors.clear();
        return;
    }
    if (frame.count > UINT32_MAX / 16 || !frame.positions || frame.positions->getDesc().structStride != 16 ||
        frame.positions->getDesc().byteSize < uint64_t(frame.count) * 16 ||
        (m_configuration.color && (!frame.colors || frame.colors->getDesc().structStride != 4 ||
                                   frame.colors->getDesc().byteSize < uint64_t(frame.count) * 4)))
        throw std::invalid_argument("Point fusion requires complete compact position/color buffers");
    std::vector<Float4> positions(frame.count);
    m_runtime->read(frame.positions, positions.data(), positions.size() * 16);
    std::vector<uint32_t> colors(frame.count, 0xffffffffu);
    if (frame.colors && frame.colors->getDesc().structStride == 4 &&
        frame.colors->getDesc().byteSize >= uint64_t(frame.count) * 4)
        m_runtime->read(frame.colors, colors.data(), colors.size() * 4);
    auto leaves = m_leaves;
    const float support = m_configuration.support;
    for (const auto& p : positions) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || p.w != 1 ||
            std::abs(p.x) > 4000000 || std::abs(p.y) > 4000000 || std::abs(p.z) > 4000000)
            throw std::invalid_argument(
                "Point fusion requires valid positions in the exact half-voxel coordinate domain");
        const int32_t lo[3] = {floorCell(p.x - support, 8), floorCell(p.y - support, 8),
                               floorCell(p.z - support, 8)};
        const int32_t hi[3] = {floorCell(p.x + 1 + support, 8), floorCell(p.y + 1 + support, 8),
                               floorCell(p.z + 1 + support, 8)};
        for (int32_t z = lo[2]; z <= hi[2]; z += 8)
            for (int32_t y = lo[1]; y <= hi[1]; y += 8)
                for (int32_t x = lo[0]; x <= hi[0]; x += 8)
                    leaves.emplace(x, y, z);
    }
    std::vector<Int4> origins;
    origins.reserve(leaves.size());
    for (const auto& p : leaves)
        origins.push_back({std::get<0>(p), std::get<1>(p), std::get<2>(p), 0});
    PackedVolume packed;
    std::string error;
    TopologyBuildOptions options;
    options.logDimensions = {3, 3, 3, 3, 5};
    if (!buildTopology(origins, options, packed, error))
        throw std::runtime_error("Point fusion topology: " + error);
    const auto& level = packed.levels[0];
    const auto resolution = packed.volume.atlasResolutionAndApron;
    const uint64_t voxelCount = uint64_t(resolution.x) * resolution.y * resolution.z;
    if (voxelCount > UINT32_MAX / 4 || uint64_t(level.nodes.z) * 8 > UINT32_MAX / sizeof(GatherCell))
        throw std::invalid_argument("Point fusion atlas exceeds portable storage");
    std::vector<GatherCell> cells(size_t(level.nodes.z) * 8);
    std::map<Coordinate, uint32_t> cellIndex;
    for (uint32_t leaf = 0; leaf < level.nodes.z; ++leaf) {
        const uint32_t* node = packed.nodes.data() + (level.nodes.x + size_t(leaf) * level.nodes.y) / 4;
        for (uint32_t z = 0; z < 2; ++z)
            for (uint32_t y = 0; y < 2; ++y)
                for (uint32_t x = 0; x < 2; ++x) {
                    const uint32_t i = leaf * 8 + (z * 2 + y) * 2 + x;
                    auto& cell = cells[i];
                    cell.worldOriginAndWidth = {int32_t(node[1]) + int32_t(x * 4),
                                                int32_t(node[2]) + int32_t(y * 4),
                                                int32_t(node[3]) + int32_t(z * 4), 4};
                    cell.atlasOrigin = {node[4] + x * 4, node[5] + y * 4, node[6] + z * 4, 0};
                    cellIndex.emplace(
                        key(cell.worldOriginAndWidth.x, cell.worldOriginAndWidth.y, cell.worldOriginAndWidth.z),
                        i);
                }
    }
    std::vector<std::vector<uint32_t>> indices(cells.size());
    uint64_t pairCount = 0;
    for (uint32_t i = 0; i < frame.count; ++i) {
        const auto& p = positions[i];
        const int32_t lo[3] = {floorCell(p.x - support, 4), floorCell(p.y - support, 4),
                               floorCell(p.z - support, 4)};
        const int32_t hi[3] = {floorCell(p.x + 1 + support, 4), floorCell(p.y + 1 + support, 4),
                               floorCell(p.z + 1 + support, 4)};
        for (int32_t z = lo[2]; z <= hi[2]; z += 4)
            for (int32_t y = lo[1]; y <= hi[1]; y += 4)
                for (int32_t x = lo[0]; x <= hi[0]; x += 4) {
                    const auto found = cellIndex.find(key(x, y, z));
                    if (found == cellIndex.end())
                        throw std::logic_error("Point support has no allocated subcell");
                    if (++pairCount > UINT32_MAX / 16)
                        throw std::invalid_argument("Point fusion expanded pairs exceed portable storage");
                    indices[found->second].push_back(i);
                }
    }
    std::vector<Float4> expanded;
    std::vector<uint32_t> expandedColors;
    expanded.reserve(size_t(pairCount));
    if (m_configuration.color)
        expandedColors.reserve(size_t(pairCount));
    for (size_t i = 0; i < cells.size(); ++i) {
        cells[i].points = {uint32_t(expanded.size()), uint32_t(indices[i].size()), 0, 0};
        for (uint32_t j : indices[i]) {
            expanded.push_back(positions[j]);
            if (m_configuration.color)
                expandedColors.push_back(colors[j]);
        }
    }
    cells.erase(
        std::remove_if(cells.begin(), cells.end(), [](const GatherCell& cell) { return cell.points.y == 0; }),
        cells.end());
    auto pointBuffer = m_runtime->buffer(expanded.size() * 16, 16, expanded.data());
    nvrhi::BufferHandle colorBuffer;
    if (m_configuration.color)
        colorBuffer = m_runtime->buffer(expandedColors.size() * 4, 4, expandedColors.data());
    SlangPointFusion gather(*m_runtime, resolution, cells, pointBuffer, uint32_t(expanded.size()), colorBuffer);
    auto map = m_runtime->buffer(packed.atlasMap.size() * sizeof(AtlasNode), sizeof(AtlasNode),
                                 packed.atlasMap.data());
    UpdateParameters p{};
    p.current = packed.volume;
    p.operation = {m_volume ? 1u : 0u, m_configuration.color ? 1u : 0u, uint32_t(voxelCount), 0};
    if (m_volume)
        p.previous = m_volume->info();
    std::vector<BindingSetItem> bindings = {BindingSetItem::StructuredBuffer_SRV(5, map),
                                            BindingSetItem::StructuredBuffer_UAV(0, gather.density()),
                                            BindingSetItem::StructuredBuffer_UAV(1, gather.color())};
    if (m_volume) {
        bindings.push_back(BindingSetItem::RawBuffer_SRV(0, m_volume->nodes()));
        bindings.push_back(BindingSetItem::RawBuffer_SRV(1, m_volume->children()));
        bindings.push_back(BindingSetItem::StructuredBuffer_SRV(2, m_volume->levels()));
        bindings.push_back(BindingSetItem::StructuredBuffer_SRV(3, m_volume->atlas()));
    }
    if (m_color)
        bindings.push_back(BindingSetItem::RawBuffer_SRV(4, m_color->atlas()));
    dispatch1D(*m_runtime, "remapFusionAtlas", p, bindings, uint32_t(voxelCount));
    gather.gather(m_configuration.radius, true, true);
    std::vector<float> initial(size_t(voxelCount), 0);
    auto volume = std::make_unique<SlangVolume>(*m_runtime, packed, initial.data(), initial.size());
    std::unique_ptr<SlangAtlas> color;
    if (m_configuration.color)
        color = std::make_unique<SlangAtlas>(*m_runtime, packed, AtlasFormat::RGBA8_UINT, initial.data(),
                                             initial.size() * 4);
    bindings = {BindingSetItem::StructuredBuffer_SRV(6, gather.density()),
                BindingSetItem::StructuredBuffer_SRV(7, gather.color()),
                BindingSetItem::StructuredBuffer_UAV(0, volume->atlas())};
    if (color)
        bindings.push_back(BindingSetItem::RawBuffer_UAV(2, color->atlas()));
    dispatch1D(*m_runtime, "publishFusionAtlas", p, bindings, uint32_t(voxelCount));
    volume->updateApron(0);
    if (color)
        color->updateApron(0);
    m_volume = std::move(volume);
    m_color = std::move(color);
    m_packed = std::move(packed);
    m_leaves = std::move(leaves);
    ++m_frames;
    m_hits += frame.count;
    m_lastPositions = std::move(positions);
    m_lastColors = std::move(colors);
}
}  // namespace gvdb::samples
