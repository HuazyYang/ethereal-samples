// SPDX-License-Identifier: Apache-2.0
#include "PrintVolume.h"
#include "ComputeRuntime.h"
#include <gvdb/slang/GVDBTopologyBuilder.h>
#include <nvrhi/core/autoptr.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace gvdb::samples {
using namespace slang;
PrintVolume::PrintVolume(ethereal::slang::ComputeRuntime& runtime, const SampleUtils::TriangleMesh& mesh,
                         PrintConfiguration configuration)
    : m_runtime(&runtime), m_bounds(donut::math::box3::empty()), m_configuration(configuration) {
    if (mesh.positions.empty() || mesh.indices.empty())
        throw std::invalid_argument("Print mesh must contain vertices and triangles");
    for (const auto& v : mesh.positions) {
        m_vertices.push_back({v.x, v.y, v.z, 1});
        m_bounds |= v;
    }
    for (const auto& t : mesh.indices) {
        m_indices.push_back(uint32_t(t.x));
        m_indices.push_back(uint32_t(t.y));
        m_indices.push_back(uint32_t(t.z));
    }
    rebuild(configuration.voxelSize);
}
void PrintVolume::rebuild(float voxelSize) {
    namespace dm = donut::math;
    const auto& c = m_configuration;
    if (!std::isfinite(voxelSize) || voxelSize <= 0 || !std::isfinite(c.partSize) || c.partSize <= 0 ||
        !std::isfinite(c.rotationDegrees) || !std::isfinite(c.margin) || c.margin < 0 ||
        !std::isfinite(c.pivot.x) || !std::isfinite(c.pivot.y) || !std::isfinite(c.pivot.z))
        throw std::invalid_argument("Print scale, pivot, rotation or margin is invalid");
    const dm::affine3 transform = dm::rotation(dm::float3(0, 1, 0), dm::radians(c.rotationDegrees)) *
                                  dm::translation(dm::float3(c.pivot.x, c.pivot.y, c.pivot.z)) *
                                  dm::scaling(dm::float3(1 / voxelSize)) * dm::scaling(dm::float3(c.partSize));
    const auto bounds = m_bounds * transform;
    const auto padding = bounds.diagonal() * c.margin;
    const auto lower = bounds.lower() - padding, upper = bounds.upper() + padding;
    int32_t low[3], high[3];
    uint32_t resolution[3];
    for (int axis = 0; axis < 3; ++axis) {
        const double a = std::floor(double(lower[axis]) / 8) * 8, b = std::ceil(double(upper[axis]) / 8) * 8;
        if (!std::isfinite(a) || !std::isfinite(b) || a < INT32_MIN || b > INT32_MAX || b <= a ||
            b - a > INT32_MAX)
            throw std::invalid_argument("Print mesh bounds exceed portable signed coordinates");
        low[axis] = int32_t(a);
        high[axis] = int32_t(b);
        resolution[axis] = uint32_t(b - a);
    }
    VoxelizeParameters p{};
    p.boundsMinimum = {float(low[0]), float(low[1]), float(low[2]), 0};
    p.boundsMaximum = {float(high[0]), float(high[1]), float(high[2]), 0};
    p.resolution = {resolution[0], resolution[1], resolution[2], 0};
    p.values = {1, .5f, 0, 0};
    const auto matrix = dm::affineToHomogeneous(transform);
    for (int axis = 0; axis < 3; ++axis)
        p.transformRows[axis] = {matrix[0][axis], matrix[1][axis], matrix[2][axis], matrix[3][axis]};
    auto grid = voxelizeMesh(*m_runtime, m_vertices, m_indices, p, VoxelFormat::R32_FLOAT);
    std::vector<float> dense(grid.byteSize / 4);
    m_runtime->read(grid.values, dense.data(), grid.byteSize);
    size_t surface = 0, interior = 0;
    for (float value : dense) {
        if (value == 1)
            ++surface;
        else if (value == .5f)
            ++interior;
        else if (value != 0)
            throw std::runtime_error("Print voxelizer produced an invalid classification");
    }
    if (!surface)
        throw std::runtime_error("Print voxelization produced no surface voxels");
    TopologyBuildOptions topology;
    topology.logDimensions = {3, 3, 3, 3, 5};
    ScalarVolumeData data;
    std::string error;
    const Int4 origin{low[0], low[1], low[2], 0};
    if (!buildScalarVolumeAtOrigin(dense.data(), dense.size(), p.resolution, origin, topology, 0, 0, data,
                                   error))
        throw std::runtime_error("Print topology: " + error);
    auto volume = std::make_unique<SlangVolume>(*m_runtime, data.packed, data.atlas.data(), data.atlas.size());
    m_configuration.voxelSize = voxelSize;
    m_packed = std::move(data.packed);
    m_volume = std::move(volume);
    m_dense = std::move(dense);
    m_resolution = p.resolution;
    m_origin = origin;
    m_surfaceCount = surface;
    m_interiorCount = interior;
}
SampleUtils::TriangleMesh loadPrintMesh(const std::filesystem::path& path) {
    auto fs = MAKE_RC_OBJ_PTR(donut::vfs::NativeFileSystem);
    auto mesh = SampleUtils::ObjLoader()(fs, path.string().c_str());
    if (mesh.positions.empty() || mesh.indices.empty())
        throw std::runtime_error("Cannot load print OBJ mesh: " + path.string());
    return mesh;
}
std::vector<Float4> printTransferFunction() {
    std::vector<Float4> result(256);
    for (size_t i = 0; i < result.size(); ++i) {
        const float v = float(i) / 255;
        result[i] = v <= .5f ? Float4{2 * v, 2 * v, 2 * v, v} : Float4{1, 1, 1, .5f + .6f * (v - .5f)};
    }
    return result;
}
}  // namespace gvdb::samples
