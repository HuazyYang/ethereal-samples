// SPDX-License-Identifier: Apache-2.0
#include "PortableFluid.h"

#include <gvdb/GVDBNvrhiGatherApi.h>
#include <gvdb/GVDBNvrhiApronApi.h>
#include <donut/app/ApplicationBase.h>
#include <fstream>
#include <gvdb/slang/GVDBTopologyBuilder.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <unordered_set>
#include <set>
#include <tuple>

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
    const float minX = minimum.x + .1f, minZ = minimum.z + .1f, maxX = maximum.x - .1f, maxZ = maximum.z - .1f;
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
        if (p.x == -1000 && p.y == -1000 && p.z == -1000)
            continue;
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
    if (keys.empty())
        throw std::runtime_error("Fluid has no valid particles to reconstruct");
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
    if (!buildTopology(leaves, options, packed, error))
        throw std::runtime_error("Fluid topology: " + error);
    return packed;
}
namespace p = portable;
namespace {
void check(nvrhi::FRESULT result, const char* message) {
    if (NVRHI_FAILED(result))
        throw std::runtime_error(std::string(message) + ": " + std::to_string(result));
}
std::vector<char> artifact(ethereal::slang::ComputeRuntime& runtime, const char* name) {
    const auto path = donut::app::GetDirectoryWithExecutable() / "shaders/gvdb_fluid_owned" /
                      donut::app::GetShaderTypeName(runtime.device()->getGraphicsAPI()) / name;
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("Missing fluid artifact: " + path.string());
    return {(std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>()};
}
}  // namespace
struct FluidSurface::State {
    nvrhi::DeviceHandle device;
    nvrhi::CommandListHandle commands;
    p::VolumeHandle owner;
    p::SnapshotHandle visible;
    template <class F>
    void submit(F&& record, bool wait = false) {
        struct Close {
            nvrhi::ICommandList* commands;
            bool open = false;
            ~Close() {
                if (open)
                    commands->close();
            }
        } scope{commands};
        commands->open();
        scope.open = true;
        record(p::RecordingContext{device, commands});
        commands->close();
        scope.open = false;
        if (!device->executeCommandList(commands) && device->getGraphicsAPI() != nvrhi::GraphicsAPI::D3D11)
            throw std::runtime_error("Fluid submission failed");
        if (wait) {
            if (!device->waitForIdle())
                throw std::runtime_error("Fluid completion failed");
            device->runGarbageCollection();
        }
    }
    void publish(p::IGVDBGenerationTicket* ticket) {
        submit([&](p::RecordingContext context) {
            check(owner->recordAndPublish(context, ticket), "Fluid publication");
        });
    }
    std::vector<uint8_t> read(p::BorrowedBufferBinding binding) {
        if (!binding.byteSize)
            return {};
        nvrhi::BufferHandle staging;
        check(device->createBuffer(nvrhi::BufferDesc()
                                       .setByteSize(binding.byteSize)
                                       .setCpuAccess(nvrhi::CpuAccessMode::Read)
                                       .setInitialState(nvrhi::ResourceStates::CopyDest)
                                       .setKeepInitialState(true),
                                   &staging),
              "Fluid topology staging");
        submit(
            [&](p::RecordingContext context) {
                context.commands->copyBuffer(staging, 0, binding.buffer, binding.byteOffset, binding.byteSize);
            },
            true);
        std::vector<uint8_t> result(size_t(binding.byteSize));
        const void* mapped = device->mapBuffer(staging, nvrhi::CpuAccessMode::Read);
        if (!mapped)
            throw std::runtime_error("Fluid topology map failed");
        std::memcpy(result.data(), mapped, result.size());
        device->unmapBuffer(staging);
        return result;
    }
};
FluidSurface::FluidSurface(ethereal::slang::ComputeRuntime& runtime)
    : m_runtime(&runtime) {}
FluidSurface::~FluidSurface() = default;
p::SnapshotHandle FluidSurface::snapshot() const { return m_state ? m_state->visible : p::SnapshotHandle{}; }
const p::IGVDBHostVolume& FluidSurface::topology() const { return *m_state->owner; }
void FluidSurface::reconstruct(nvrhi::IBuffer* positions, uint32_t count, nvrhi::IBuffer* colors) {
    if (!positions || !count || positions->getDesc().byteSize < uint64_t(count) * 16 ||
        (colors && colors->getDesc().byteSize < uint64_t(count) * 4))
        throw std::invalid_argument("Invalid fluid particle streams");
    std::vector<Float4> source(count), host;
    m_runtime->read(positions, source.data(), source.size() * 16);
    std::vector<uint32_t> sourceColors(colors ? count : 0), hostColors;
    if (colors)
        m_runtime->read(colors, sourceColors.data(), sourceColors.size() * 4);
    host.reserve(count);
    if (colors)
        hostColors.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const auto& value = source[i];
        if (value.x == -1000 && value.y == -1000 && value.z == -1000)
            continue;
        if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z) ||
            std::abs(value.x) > 8388592 || std::abs(value.y) > 8388592 || std::abs(value.z) > 8388592)
            throw std::invalid_argument("Fluid position exceeds exact sparse coordinate domain");
        host.push_back(value);
        if (colors)
            hostColors.push_back(sourceColors[i]);
    }
    if (host.empty())
        throw std::runtime_error("Fluid has no valid particles to reconstruct");
    auto state = std::make_unique<State>();
    state->device = m_runtime->device();
    check(state->device->createCommandList({}, &state->commands), "Fluid command list");
    auto remap = artifact(*m_runtime, "GVDBAtlasRepack_relocateAtlas.bin");
    p::VolumeCreateDesc create;
    create.device = state->device;
    if (!p::LevelConfig::fromBranching(3, 3, 3, 3, 5, create.configuration))
        throw std::logic_error("Fluid levels");
    create.remapArtifact = remap.data();
    create.remapArtifactBytes = remap.size();
    check(p::createVolume(create, &state->owner), "Fluid owner");
    check(state->owner->setChannel(0,
                                   {
                                       p::AtlasFormat::R32_FLOAT, {3, 0, 0, 0}
    }),
          "Fluid scalar channel");
    if (colors)
        check(state->owner->setChannel(1, {p::AtlasFormat::RGBA8_UINT, {}}), "Fluid color channel");
    auto conversion = artifact(*m_runtime, "GVDBPointStreamConversion_convertPointStream.bin");
    p::PointOpsCreateDesc pointCreate;
    pointCreate.volume = state->owner;
    pointCreate.conversionArtifact = conversion.data();
    pointCreate.conversionArtifactBytes = conversion.size();
    p::PointOpsHandle points;
    check(p::createPointOps(pointCreate, &points), "Fluid point operator");
    p::PointSetDesc input;
    input.positions.stream = {host.data(), host.size() * 16, 0, 16, host.size(), p::PointComponent::Float32, 4};
    if (colors)
        input.colors.stream = {hostColors.data(), hostColors.size() * 4,    0, 4,
                               hostColors.size(), p::PointComponent::UInt8, 4};
    p::PointUploadTicketHandle upload;
    check(points->preparePoints(input, {}, &upload), "Fluid point preparation");
    state->submit(
        [&](p::RecordingContext context) {
            check(points->recordPoints(context, upload), "Fluid point conversion");
        },
        true);
    p::PointSetHandle converted;
    check(upload->finishAfterCallerCompletion(&converted), "Fluid point completion");
    // This sample uses floor for signed support; the generic legacy point
    // topology request truncates negative coordinates. Activate exact leaf origins.
    std::set<std::tuple<int32_t, int32_t, int32_t>> leaves;
    for (const auto& value : host) {
        const float v[] = {value.x, value.y, value.z};
        int32_t lo[3], hi[3];
        for (uint32_t axis = 0; axis < 3; ++axis) {
            lo[axis] = int32_t(std::floor((std::floor(double(v[axis])) - 2) / 32)) * 32;
            hi[axis] = int32_t(std::floor((std::floor(double(v[axis])) + 4) / 32)) * 32;
        }
        for (int32_t z = lo[2]; z <= hi[2]; z += 32)
            for (int32_t y = lo[1]; y <= hi[1]; y += 32)
                for (int32_t x = lo[0]; x <= hi[0]; x += 32)
                    leaves.emplace(z, y, x);
    }
    for (const auto& origin : leaves) {
        p::NodeId id;
        check(state->owner->activateAtLevel(0, {std::get<2>(origin), std::get<1>(origin), std::get<0>(origin)},
                                            id),
              "Fluid leaf activation");
    }
    check(state->owner->finishTopology(), "Fluid topology finish");
    p::GenerationDesc generation;
    generation.placement.minimumBrickCount = {1, 1, 1};
    p::GenerationTicketHandle ticket;
    check(state->owner->prepareGeneration(generation, &ticket), "Fluid topology preparation");
    state->publish(ticket);
    ticket = nullptr;
    auto gatherBinary = artifact(*m_runtime, "GVDBAtlasGather_gatherOwnedAtlas.bin");
    p::GatherCreateDesc gatherCreate;
    gatherCreate.volume = state->owner;
    gatherCreate.artifact = gatherBinary.data();
    gatherCreate.artifactBytes = gatherBinary.size();
    p::GatherHandle gather;
    check(p::createGather(gatherCreate, &gather), "Fluid gather operator");
    p::GatherRequestDesc gatherRequest;
    gatherRequest.points = converted;
    gatherRequest.kind = p::GatherKind::LevelSet;
    gatherRequest.subcellWidth = 4;
    gatherRequest.supportRadius = 2;
    gatherRequest.colorChannel = colors ? 1 : UINT32_MAX;
    check(gather->prepare(gatherRequest, {}, &ticket), "Fluid gather preparation");
    state->publish(ticket);
    ticket = nullptr;
    auto mutation = artifact(*m_runtime, "GVDBAtlasMutation_mutateAtlas.bin");
    p::ApronCreateDesc apronCreate;
    apronCreate.volume = state->owner;
    apronCreate.artifact = mutation.data();
    apronCreate.artifactBytes = mutation.size();
    p::ApronOpsHandle apron;
    check(p::createApronOps(apronCreate, &apron), "Fluid apron operator");
    p::ApronRequestDesc apronRequest;
    apronRequest.channelMask = 1;
    check(apron->prepare(apronRequest, {}, &ticket), "Fluid apron preparation");
    state->publish(ticket);
    check(state->owner->getSnapshot(&state->visible), "Fluid snapshot");
    m_state = std::move(state);
    m_positions = std::move(host);
}
std::vector<float> FluidSurface::readAtlas() const {
    m_runtime->flush();
    p::ReadbackHandle read;
    check(m_state->owner->prepareReadback(m_state->visible, 1, &read), "Fluid readback preparation");
    m_state->submit(
        [&](p::RecordingContext context) {
            check(m_state->owner->recordReadback(context, read), "Fluid readback record");
        },
        true);
    check(read->finishAfterCallerCompletion(), "Fluid readback completion");
    p::BorrowedBytes bytes;
    check(read->getChannelData(0, bytes), "Fluid atlas bytes");
    std::vector<float> result(size_t(bytes.byteCount / 4));
    if (bytes.byteCount)
        std::memcpy(result.data(), bytes.data, size_t(bytes.byteCount));
    return result;
}
PackedVolume FluidSurface::readTopology() const {
    m_runtime->flush();
    PackedVolume result;
    const auto desc = m_state->visible->getDesc();
    result.volume = desc.volume;
    for (uint32_t i = 0; i < desc.levelCount; ++i) {
        p::LevelInfo level;
        check(m_state->visible->getLevel(i, level), "Fluid snapshot level");
        result.levels.push_back(level);
    }
    auto read = [&](p::TopologyBuffer kind, auto& destination) {
        p::BorrowedBufferBinding binding;
        check(m_state->visible->getTopologyBuffer(kind, binding), "Fluid topology binding");
        auto bytes = m_state->read(binding);
        using Value = typename std::decay_t<decltype(destination)>::value_type;
        if (bytes.size() % sizeof(Value))
            throw std::logic_error("Fluid topology byte count");
        destination.resize(bytes.size() / sizeof(Value));
        if (!bytes.empty())
            std::memcpy(destination.data(), bytes.data(), bytes.size());
    };
    read(p::TopologyBuffer::Nodes, result.nodes);
    read(p::TopologyBuffer::Children, result.children);
    read(p::TopologyBuffer::AtlasMap, result.atlasMap);
    return result;
}
std::vector<uint32_t> FluidSurface::readColor() const {
    m_runtime->flush();
    p::ReadbackHandle read;
    check(m_state->owner->prepareReadback(m_state->visible, 2, &read), "Fluid readback preparation");
    m_state->submit(
        [&](p::RecordingContext context) {
            check(m_state->owner->recordReadback(context, read), "Fluid readback record");
        },
        true);
    check(read->finishAfterCallerCompletion(), "Fluid readback completion");
    p::BorrowedBytes bytes;
    check(read->getChannelData(1, bytes), "Fluid atlas bytes");
    std::vector<uint32_t> result(size_t(bytes.byteCount / 4));
    if (bytes.byteCount)
        std::memcpy(result.data(), bytes.data, size_t(bytes.byteCount));
    return result;
}
}  // namespace gvdb::samples::fluid
