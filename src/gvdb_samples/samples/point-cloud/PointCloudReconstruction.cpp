// SPDX-License-Identifier: Apache-2.0
#include "PointCloudReconstruction.h"
#include "ComputeRuntime.h"
#include <gvdb/GVDBSlangPointConversion.h>
#include <gvdb/GVDBSlangPointFusion.h>
#include <gvdb/GVDBSlangSubcells.h>
#include <gvdb/slang/GVDBTopologyBuilder.h>
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <tuple>
namespace gvdb::samples::pointcloud {
using namespace slang;
using nvrhi::BindingSetItem;
namespace {
struct alignas(16) Parameters {
    UInt4 counts;
    Float4 minimum, range;
};
static_assert(sizeof(Parameters) == 48);
void launch(ethereal::slang::ComputeRuntime& runtime, const char* entry, const Parameters& p,
            const std::vector<BindingSetItem>& bindings, uint32_t count) {
    const uint32_t groups = count / 256 + uint32_t(count % 256 != 0);
    if (groups)
        runtime.dispatch("PointCloudReconstruction", entry, &p, sizeof(p), bindings, std::min(groups, 65535u),
                         groups / 65535u + uint32_t(groups % 65535u != 0));
}
}  // namespace
SlangVolume& PointCloudReconstruction::volume() const {
    if (!m_volume)
        throw std::logic_error("Point cloud has no reconstructed frame");
    return *m_volume;
}
void PointCloudReconstruction::reconstruct(const PointFrame& frame, const ReconstructionSettings& settings) {
    validatePointFrame(frame, settings.renderScale);
    if (!std::isfinite(settings.smoothingCenterWeight) || settings.smoothingCenterWeight == -6 ||
        !std::isfinite(settings.smoothingBias))
        throw std::invalid_argument("Point cloud smoothing parameters are invalid");
    std::vector<uint8_t> bytes((frame.coordinates.size() * 2 + 3) & ~size_t(3), 0);
    for (size_t i = 0; i < frame.coordinates.size(); ++i) {
        bytes[i * 2] = uint8_t(frame.coordinates[i]);
        bytes[i * 2 + 1] = uint8_t(frame.coordinates[i] >> 8);
    }
    auto packedPoints = m_runtime->buffer(bytes.size(), 0, bytes.data(), true);
    PointConversionParameters conversion{
        {frame.count(), 2, 0, 6},
        frame.minimum,
        {(frame.maximum.x - frame.minimum.x) / 65535.f, (frame.maximum.y - frame.minimum.y) / 65535.f,
         (frame.maximum.z - frame.minimum.z) / 65535.f, 0},
        {settings.renderScale, settings.renderScale, settings.renderScale, 1},
        {}
    };
    auto positions = convertPackedPoints(*m_runtime, packedPoints, conversion);
    std::vector<Float4> host(frame.count());
    m_runtime->read(positions, host.data(), host.size() * sizeof(Float4));
    const auto bounds = pointQuantizationBounds(host);
    bool signedAxis[3] = {false, false, false};
    for (const auto& p : host) {
        signedAxis[0] |= p.x < 0;
        signedAxis[1] |= p.y < 0;
        signedAxis[2] |= p.z < 0;
    }
    // Legacy fromBranching(r4,r3,r2,r1,r0) places r0=4 at the leaf: width16.
    std::set<std::tuple<int32_t, int32_t, int32_t>> leaves;
    for (const auto& p : host) {
        const float values[] = {p.x, p.y, p.z};
        int32_t lo[3], hi[3];
        for (int a = 0; a < 3; ++a) {
            if (signedAxis[a]) {
                lo[a] = int32_t(std::floor((std::floor(double(values[a])) - 2) / 16)) * 16;
                hi[a] = int32_t(std::floor((std::floor(double(values[a])) + 4) / 16)) * 16;
            } else {
                lo[a] = ((int32_t(values[a]) - 2) / 16) * 16;
                hi[a] = ((int32_t(values[a]) + 4) / 16) * 16;
            }
        }
        for (int32_t z = lo[2]; z <= hi[2]; z += 16)
            for (int32_t y = lo[1]; y <= hi[1]; y += 16)
                for (int32_t x = lo[0]; x <= hi[0]; x += 16)
                    leaves.emplace(x, y, z);
    }
    std::vector<Int4> origins;
    origins.reserve(leaves.size());
    for (const auto& p : leaves)
        origins.push_back({std::get<0>(p), std::get<1>(p), std::get<2>(p), 0});
    PackedVolume packed;
    TopologyBuildOptions options;
    options.logDimensions = {4, 3, 3, 3, 3};
    std::string error;
    if (!buildTopology(origins, options, packed, error))
        throw std::runtime_error("Point cloud topology: " + error);
    auto bins = binPointSubcells(*m_runtime, packed, positions, frame.count(), 4, 1);
    if (!bins.pointCount)
        throw std::runtime_error("Point cloud subcell binning lost all valid positions");
    // Bin before quantization: quantization may move a coordinate across a cell
    // boundary, but must not change the original subcell membership.
    auto compressed =
        quantizePointsU16(*m_runtime, bins.positions, bins.pointCount, bounds.minimum, bounds.range);
    auto decoded = m_runtime->buffer(size_t(bins.pointCount) * 16, 16);
    Parameters p{
        {bins.pointCount, 0, 0, 0},
        bounds.minimum, bounds.range
    };
    launch(*m_runtime, "decodeSubcellPoints", p,
           {BindingSetItem::RawBuffer_SRV(0, compressed), BindingSetItem::StructuredBuffer_UAV(0, decoded)},
           bins.pointCount);
    const auto resolution = packed.volume.atlasResolutionAndApron;
    const uint64_t voxels = uint64_t(resolution.x) * resolution.y * resolution.z;
    if (voxels > UINT32_MAX / 4)
        throw std::invalid_argument("Point cloud atlas exceeds portable buffer limits");
    SlangPointFusion gather(*m_runtime, resolution, bins.cells, decoded, bins.pointCount);
    gather.gather(1, true, false);
    std::vector<float> initial(size_t(voxels), 3);
    auto volume = std::make_unique<SlangVolume>(*m_runtime, packed, initial.data(), initial.size());
    p.counts.x = uint32_t(voxels);
    launch(*m_runtime, "publishPointCloudAtlas", p,
           {BindingSetItem::StructuredBuffer_SRV(1, gather.density()),
            BindingSetItem::StructuredBuffer_UAV(1, volume->atlas())},
           uint32_t(voxels));
    volume->updateApron(3);
    if (settings.smoothingIterations)
        volume->smooth(settings.smoothingCenterWeight, settings.smoothingBias, settings.smoothingIterations, 3);
    m_volume = std::move(volume);
    m_packed = std::move(packed);
    m_positions = std::move(positions);
    m_hostPositions = std::move(host);
    m_quantization = bounds;
    ++m_generation;
}
std::vector<PointLine> PointCloudReconstruction::pointLines() const {
    constexpr size_t cap = 250000;
    const size_t stride = std::max(size_t(1), (m_hostPositions.size() + cap - 1) / cap);
    std::vector<PointLine> result;
    result.reserve((m_hostPositions.size() + stride - 1) / stride);
    for (size_t i = 0; i < m_hostPositions.size(); i += stride) {
        const auto& p = m_hostPositions[i];
        result.push_back({
            p, {p.x + .5f, p.y + .5f, p.z + .5f, 1},
             {p.x / 256, p.y / 256, p.z / 256, 1}
        });
    }
    return result;
}
}  // namespace gvdb::samples::pointcloud
