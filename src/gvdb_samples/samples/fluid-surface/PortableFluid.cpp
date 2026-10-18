// SPDX-License-Identifier: Apache-2.0
#include "PortableFluid.h"

#include <gvdb/GVDBSlangPointFusion.h>
#include <gvdb/GVDBSlangSubcells.h>
#include <gvdb/slang/GVDBTopologyBuilder.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <unordered_set>

#include "ComputeRuntime.h"
namespace gvdb::samples::fluid {
using namespace slang;
InitialFluid makeInitialFluid(uint32_t requested, bool compact, uint32_t seed) {
    if (!requested || requested > 1500000)
        throw std::invalid_argument("Fluid particle count must be 1..1500000");
    InitialFluid result;
    auto& p = result.parameters;
    constexpr float radius = .015f, scale = .005f, mass = .00020543f, density = 400;
    const float pi = 3.14159265358979323846f, spacing = std::pow(mass / density, 1.f / 3.f) * .87f / scale;
    p.gridMinimumAndCellWidth = {0, 0, 0, radius / scale};
    p.gridResolutionAndCount = {uint32_t(std::ceil(400 / (radius / scale))),
                                uint32_t(std::ceil(200 / (radius / scale))),
                                uint32_t(std::ceil(400 / (radius / scale))), 0};
    p.physical = {radius / scale, scale, mass, density};
    p.kernels = {315.f / (64 * pi * std::pow(radius, 9.f)), -45.f / (pi * std::pow(radius, 6.f)),
                 .5f * 45.f / (pi * std::pow(radius, 6.f)), 1};
    p.boundaryMinimum = {6, 6, 6, 0};
    p.boundaryMaximum = {394, 194, 394, 0};
    p.boundary = {.015f, 50000, 100, 0};
    p.integration = {0, .003f, 150, 3};
    p.gravityAndSlope = {0, -9.8f, 0, .1f};
    p.wall = {6, 100, 0, 1};
    const Float4 minimum = compact ? Float4{184, 88, 184, 0} : Float4{100, 80, 100, 0};
    const Float4 maximum = compact ? Float4{216, 120, 216, 0} : Float4{300, 190, 300, 0};
    const float minX = minimum.x + .1f, minZ = minimum.z + .1f, maxX = maximum.x - .1f,
                maxZ = maximum.z - .1f;
    const uint32_t nx = uint32_t(std::ceil((maximum.x - minimum.x - .1f) / spacing));
    const uint32_t nz = uint32_t(std::ceil((maximum.z - minimum.z - .1f) / spacing));
    // Fixed MSVC rand sequence and z/y/x argument evaluation order of the retained
    // Windows application. Each reset starts at the same seed on every platform.
    auto random = [&]() {
        seed = seed * 214013u + 2531011u;
        return float((seed >> 16) & 32767u) / 32767.f * spacing;
    };
    result.positions.reserve(requested);
    result.colors.reserve(requested);
    for (float y = minimum.y + .1f; y <= maximum.y - .1f && result.positions.size() < requested; y += spacing)
        for (uint32_t xz = 0; xz < nx * nz && result.positions.size() < requested; ++xz) {
            const float x = minimum.x + .1f + float(xz % nx) * spacing,
                        z = minimum.z + .1f + float(xz / nx) * spacing;
            const float rz = random(), ry = random(), rx = random();
            const float red = std::clamp((x - minX) / (maxX - minX) * .8f + .2f, 0.f, 1.f);
            const float blue = std::clamp((z - minZ) / (maxZ - minZ) * .8f + .2f, 0.f, 1.f);
            const uint32_t color =
                uint32_t(red * 255) | uint32_t(.2f * 255) << 8 | uint32_t(blue * 255) << 16 | 0xff000000u;
            float opaque;
            std::memcpy(&opaque, &color, 4);
            result.positions.push_back({x + rx, y + ry, z + rz, opaque});
            result.colors.push_back(color);
        }
    p.gridResolutionAndCount.w = uint32_t(result.positions.size());
    return result;
}
PackedVolume buildFluidTopology(const std::vector<Float4>& positions) {
    std::unordered_set<uint64_t> keys;
    // GVDBLevelConfig::fromBranching(r4,r3,r2,r1,r0) reverses its arguments.
    // The retained (3,3,3,3,5) preset therefore has 32-voxel leaves.
    constexpr int leafWidth = 32, bias = 1048576;
    for (const auto& p : positions) {
        if (p.x == -1000 && p.y == -1000 && p.z == -1000) continue;
        const float v[] = {p.x, p.y, p.z};
        int lo[3], hi[3];
        for (int a = 0; a < 3; ++a) {
            if (!std::isfinite(v[a]) || std::abs(v[a]) > 8388592)
                throw std::invalid_argument("Fluid position exceeds exact sparse coordinate domain");
            // The retained full rebuild activates [int(position)-radius, int(position)+2*radius].
            // Use floor for signed coordinates; positive legacy positions are unchanged.
            lo[a] = int(std::floor((std::floor(double(v[a])) - 2) / leafWidth));
            hi[a] = int(std::floor((std::floor(double(v[a])) + 4) / leafWidth));
        }
        for (int z = lo[2]; z <= hi[2]; ++z)
            for (int y = lo[1]; y <= hi[1]; ++y)
                for (int x = lo[0]; x <= hi[0]; ++x)
                    keys.insert(uint64_t(x + bias) | (uint64_t(y + bias) << 21) | (uint64_t(z + bias) << 42));
    }
    if (keys.empty()) throw std::runtime_error("Fluid has no valid particles to reconstruct");
    std::vector<Int4> leaves;
    leaves.reserve(keys.size());
    for (uint64_t k : keys)
        leaves.push_back({(int(k & 0x1fffff) - bias) * leafWidth,
                          (int((k >> 21) & 0x1fffff) - bias) * leafWidth, (int(k >> 42) - bias) * leafWidth,
                          0});
    std::sort(leaves.begin(), leaves.end(), [](const Int4& a, const Int4& b) {
        return a.z != b.z ? a.z < b.z : a.y != b.y ? a.y < b.y : a.x < b.x;
    });
    TopologyBuildOptions options;
    options.logDimensions = {5, 3, 3, 3, 3};
    PackedVolume packed;
    std::string error;
    if (!buildTopology(leaves, options, packed, error)) throw std::runtime_error("Fluid topology: " + error);
    return packed;
}
void FluidSurface::reconstruct(nvrhi::IBuffer* positions, uint32_t count, nvrhi::IBuffer* colors) {
    if (!positions || !count || positions->getDesc().byteSize < uint64_t(count) * 16 ||
        (colors && colors->getDesc().byteSize < uint64_t(count) * 4))
        throw std::invalid_argument("Invalid fluid particle streams");
    std::vector<Float4> host(count);
    m_runtime->read(positions, host.data(), host.size() * 16);
    // CPU topology is explicit; point binning, gather, atlas publication and aprons
    // execute on the selected NVRHI device. Stable particle indices retain colors.
    auto packed = buildFluidTopology(host);
    auto bins = binPointSubcells(*m_runtime, packed, positions, count, 4, 2, {}, colors);
    if (!bins.pointCount) throw std::runtime_error("Fluid support binning lost all particles");
    const auto resolution = packed.volume.atlasResolutionAndApron;
    const uint64_t voxels = uint64_t(resolution.x) * resolution.y * resolution.z;
    if (voxels > UINT32_MAX / 4) throw std::runtime_error("Fluid atlas exceeds portable buffer limits");
    SlangPointFusion gather(*m_runtime, resolution, bins.cells, bins.positions, bins.pointCount, bins.colors);
    gather.gather(1, true, false);
    std::vector<float> initial(size_t(voxels), 3);
    auto volume = std::make_unique<SlangVolume>(*m_runtime, packed, initial.data(), initial.size());
    nvrhi::BufferHandle color;
    if (colors) color = m_runtime->buffer(size_t(voxels) * 4, 0, nullptr, true);
    UInt4 parameters{uint32_t(voxels), colors ? 1u : 0u, 0, 0};
    std::vector<nvrhi::BindingSetItem> bindings = {
        nvrhi::BindingSetItem::StructuredBuffer_SRV(0, gather.density()),
        nvrhi::BindingSetItem::StructuredBuffer_UAV(0, volume->atlas())};
    if (colors) {
        bindings.push_back(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, gather.color()));
        bindings.push_back(nvrhi::BindingSetItem::RawBuffer_UAV(1, color));
    }
    const uint32_t groups = scanBlockCount(uint32_t(voxels));
    m_runtime->dispatch("FluidSurface", "publishFluidAtlas", &parameters, sizeof(parameters), bindings,
                        std::min(groups, 65535u), (groups + 65534) / 65535);
    volume->updateApron(3);
    m_volume = std::move(volume);
    m_color = std::move(color);
    m_packed = std::move(packed);
    m_positions = std::move(host);
}
}  // namespace gvdb::samples::fluid
