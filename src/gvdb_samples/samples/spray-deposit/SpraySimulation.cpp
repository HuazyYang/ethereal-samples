// SPDX-License-Identifier: Apache-2.0
#include "SpraySimulation.h"
#include "ComputeRuntime.h"
#include <gvdb/GVDBSlangPointFusion.h>
#include <gvdb/GVDBSlangSubcells.h>
#include <gvdb/GVDBSlangVoxelizer.h>
#include <gvdb/slang/GVDBTopologyBuilder.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace gvdb::samples::spray {
using namespace slang;
namespace {
Float4 f4(dm::float3 v) { return {v.x, v.y, v.z, 0}; }
uint32_t packColor(dm::float3 v, float alpha) {
    return uint32_t(v.x * 255) | (uint32_t(v.y * 255) << 8) | (uint32_t(v.z * 255) << 16) |
           (uint32_t(alpha * 255) << 24);
}
}  // namespace
SprayRays SprayWand::next(WandStyle style, uint32_t count) {
    if (!count || count > 1000000 || uint32_t(style) > 2 || m_time == UINT32_MAX)
        throw std::invalid_argument("Spray ray count, wand mode or simulation time is invalid");
    ++m_time;
    const float time = float(m_time);
    std::uniform_real_distribution<float> random(-1, 1);
    const uint32_t side = uint32_t(std::sqrt(float(count)));
    const dm::float3 effect = style == WandStyle::Rotate  ? dm::float3(1, 0, 0)
                              : style == WandStyle::Sweep ? dm::float3(0, 50, 0)
                                                          : dm::float3(0, 0, .5f);
    const auto rotation = dm::rotation(dm::float3(0, 1, 0), dm::radians(effect.x * time));
    const dm::float3 color(std::sin(dm::radians(2 * time)) * .5f + .5f, std::sin(dm::radians(time)) * .5f + .5f,
                           std::sin(dm::radians(.5f * time)) * .5f + .5f);
    const float st = std::sin(dm::radians(time)), lt = float(m_time % 100) / 100 - .5f;
    SprayRays result;
    result.origins.resize(count);
    result.directions.resize(count);
    result.colors.assign(count, packColor(color, .2f));
    for (uint32_t n = 0; n < count; ++n) {
        float x = float(n % side) / side - .5f + random(m_rng) * .1f,
              y = float(n / side) / side - .5f + random(m_rng) * .1f;
        x = std::clamp(x, -.5f, .5f);
        y = std::clamp(y, -.5f, .5f);
        dm::float3 origin(x * 25, 0, y * .5f + lt * effect.y);
        origin = rotation.transformVector(origin) + dm::float3(200, 400, 200);
        dm::float3 direction(random(m_rng) * .1f, 0, 0);
        direction += dm::float3(x, -.5f, y * .04f + st * effect.z);
        direction = rotation.transformVector(dm::normalize(direction));
        result.origins[n] = f4(origin);
        result.directions[n] = f4(direction);
    }
    return result;
}
SprayVolume::SprayVolume(ethereal::slang::ComputeRuntime& runtime, PackedVolume packed,
                         const std::vector<float>& scalar, uint32_t color)
    : m_runtime(&runtime), m_packed(std::move(packed)) {
    m_density = std::make_unique<SlangVolume>(runtime, m_packed, scalar.data(), scalar.size());
    std::vector<uint32_t> colors(scalar.size(), color);
    m_color = std::make_unique<SlangAtlas>(runtime, m_packed, AtlasFormat::RGBA8_UINT, colors.data(),
                                           colors.size() * 4);
}
uint32_t SprayVolume::depositPoints(nvrhi::IBuffer* positions, nvrhi::IBuffer* colors, uint32_t count) {
    auto bins = binPointSubcells(*m_runtime, m_packed, positions, count, 4, 1, {}, colors);
    SlangPointFusion gather(*m_runtime, m_density->info().atlasResolutionAndApron, bins.cells, bins.positions,
                            bins.pointCount, bins.colors);
    const auto resolution = m_density->info().atlasResolutionAndApron;
    const UInt4 params{resolution.x * resolution.y * resolution.z, 0, 0, 0};
    const uint32_t groups = (params.x + 255) / 256;
    using B = nvrhi::BindingSetItem;
    m_runtime->dispatch(
        "SprayTransfer", "seedSprayGather", &params, sizeof(params),
        {B::StructuredBuffer_SRV(0, m_density->atlas()), B::RawBuffer_SRV(1, m_color->atlas()),
         B::StructuredBuffer_UAV(0, gather.density()), B::StructuredBuffer_UAV(1, gather.color())},
        std::min(groups, 65535u), (groups + 65534) / 65535);
    gather.gather(1, false, true);
    m_runtime->dispatch(
        "SprayTransfer", "publishSprayGather", &params, sizeof(params),
        {B::StructuredBuffer_SRV(0, gather.density()), B::StructuredBuffer_SRV(2, gather.color()),
         B::StructuredBuffer_UAV(0, m_density->atlas()), B::RawBuffer_UAV(2, m_color->atlas())},
        std::min(groups, 65535u), (groups + 65534) / 65535);
    m_density->updateApron();
    m_color->updateApron();
    return bins.pointCount;
}
void SprayVolume::step(WandStyle style) {
    m_rays = m_wand.next(style);
    const auto count = uint32_t(m_rays.origins.size());
    auto origins = m_runtime->buffer(size_t(count) * 16, 16, m_rays.origins.data());
    auto directions = m_runtime->buffer(size_t(count) * 16, 16, m_rays.directions.data());
    auto colors = m_runtime->buffer(size_t(count) * 4, 4, m_rays.colors.data());
    auto query = queryVolumeRays(*m_runtime, *m_density, origins, directions, count);
    m_rays.hits.resize(count);
    m_rays.normals.resize(count);
    m_runtime->read(query.hits, m_rays.hits.data(), size_t(count) * 16);
    m_runtime->read(query.normals, m_rays.normals.data(), size_t(count) * 16);
    m_hitCount = 0;
    for (const auto& hit : m_rays.hits) {
        if (hit.w < 0)
            throw std::runtime_error("Spray ray-query iteration limit exceeded");
        m_hitCount += hit.w > 0;
    }
    depositPoints(query.hits, colors, count);
    if (m_wand.time() % 20 == 0)
        m_density->smooth(4, 0, 1);
}
std::unique_ptr<SprayVolume> voxelizeSprayPart(ethereal::slang::ComputeRuntime& runtime,
                                               const sample::SceneMesh& mesh, float scale, Float4 translation) {
    if (mesh.positions.empty() || mesh.indices.empty() || !std::isfinite(scale) || scale <= 0 ||
        !std::isfinite(translation.x) || !std::isfinite(translation.y) || !std::isfinite(translation.z))
        throw std::invalid_argument("Spray part geometry or transform is invalid");
    dm::float3 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
    const dm::float3 offset(translation.x, translation.y, translation.z);
    for (const auto& p : mesh.positions) {
        const dm::float3 q = dm::float3(p.x, p.y, p.z) * scale + offset;
        lo = dm::min(lo, q);
        hi = dm::max(hi, q);
    }
    Int4 origin{};
    UInt4 resolution{};
    int32_t* low = &origin.x;
    uint32_t* res = &resolution.x;
    for (int axis = 0; axis < 3; ++axis) {
        const double a = std::floor(double(lo[axis]) / 32) * 32, b = std::ceil(double(hi[axis]) / 32) * 32;
        if (!std::isfinite(a) || !std::isfinite(b) || a < -4000000 || b > 4000000 || b <= a)
            throw std::invalid_argument("Spray part bounds exceed portable coordinates");
        low[axis] = int32_t(a);
        res[axis] = uint32_t(b - a);
    }
    VoxelizeParameters parameters{};
    parameters.boundsMinimum = {float(origin.x), float(origin.y), float(origin.z), 0};
    parameters.boundsMaximum = {float(origin.x) + resolution.x, float(origin.y) + resolution.y,
                                float(origin.z) + resolution.z, 0};
    parameters.resolution = resolution;
    parameters.values = {1, 1, 0, 0};
    parameters.transformRows[0] = {scale, 0, 0, translation.x};
    parameters.transformRows[1] = {0, scale, 0, translation.y};
    parameters.transformRows[2] = {0, 0, scale, translation.z};
    auto grid = voxelizeMesh(runtime, mesh.positions, mesh.indices, parameters);
    std::vector<float> dense(grid.byteSize / 4);
    runtime.read(grid.values, dense.data(), grid.byteSize);
    TopologyBuildOptions options;
    options.logDimensions = {5, 3, 3, 3, 3};
    ScalarVolumeData data;
    std::string error;
    if (!buildScalarVolumeAtOrigin(dense.data(), dense.size(), resolution, origin, options, 0, 0, data, error))
        throw std::runtime_error("Spray topology: " + error);
    auto result = std::make_unique<SprayVolume>(runtime, std::move(data.packed), data.atlas);
    result->volume().smooth(4, 0, 2);
    result->color().updateApron();
    return result;
}
}  // namespace gvdb::samples::spray
