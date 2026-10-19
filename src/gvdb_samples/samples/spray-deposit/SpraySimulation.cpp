// SPDX-License-Identifier: Apache-2.0
#include "SpraySimulation.h"
#include "ComputeRuntime.h"
#include <gvdb/GVDBNvrhiGatherApi.h>
#include <gvdb/GVDBNvrhiApronApi.h>
#include <gvdb/GVDBNvrhiVoxelFilterApi.h>
#include <gvdb/GVDBNvrhiVoxelizeApi.h>
#include <gvdb/GVDBNvrhiResampleApi.h>
#include <gvdb/GVDBNvrhiSerializer.h>
#include <gvdb/slang/GVDBPortableVbx.h>
#include <sample-utils/OwnedVolumeRayQuery.h>
#include <sample-utils/OwnedVolumeRawViews.h>
#include <donut/app/ApplicationBase.h>
#include <nvrhi/core/datablob.h>
#include <fstream>
#include <cstring>
#include <array>
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

namespace p = portable;
namespace {
void check(nvrhi::FRESULT result, const char* message) {
    if (NVRHI_FAILED(result))
        throw std::runtime_error(std::string(message) + ": " + std::to_string(result));
}
std::vector<char> artifact(ethereal::slang::ComputeRuntime& runtime, const char* name) {
    const auto path = donut::app::GetDirectoryWithExecutable() / "shaders/gvdb_spray_owned" /
                      donut::app::GetShaderTypeName(runtime.device()->getGraphicsAPI()) / name;
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("Missing spray artifact: " + path.string());
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
struct Submission {
    nvrhi::DeviceHandle device;
    nvrhi::CommandListHandle commands;
    explicit Submission(nvrhi::IDevice* d)
        : device(d) {
        check(device->createCommandList({}, &commands), "Spray command list");
    }
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
            throw std::runtime_error("Spray submission failed");
        if (wait) {
            if (!device->waitForIdle())
                throw std::runtime_error("Spray completion failed");
            device->runGarbageCollection();
        }
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
              "Spray topology staging");
        submit(
            [&](p::RecordingContext context) {
                context.commands->copyBuffer(staging, 0, binding.buffer, binding.byteOffset, binding.byteSize);
            },
            true);
        std::vector<uint8_t> result(size_t(binding.byteSize));
        const void* mapped = device->mapBuffer(staging, nvrhi::CpuAccessMode::Read);
        if (!mapped)
            throw std::runtime_error("Spray map failed");
        std::memcpy(result.data(), mapped, result.size());
        device->unmapBuffer(staging);
        return result;
    }
};
p::VolumeHandle makeOwner(ethereal::slang::ComputeRuntime& runtime, const p::LevelConfig& levels) {
    auto remap = artifact(runtime, "GVDBAtlasRepack_relocateAtlas.bin");
    p::VolumeCreateDesc create;
    create.device = runtime.device();
    create.configuration = levels;
    create.remapArtifact = remap.data();
    create.remapArtifactBytes = remap.size();
    p::VolumeHandle owner;
    check(p::createVolume(create, &owner), "Spray owner");
    return owner;
}
p::VolumeHandle importFixture(ethereal::slang::ComputeRuntime& runtime, PackedVolume packed,
                              const std::vector<float>& scalar, uint32_t color) {
    if (packed.levels.empty() || packed.levels.size() > p::c_MaxLevels)
        throw std::invalid_argument("Spray fixture topology");
    p::LevelConfig levels;
    levels.levelCount = uint32_t(packed.levels.size());
    for (uint32_t i = 0; i < levels.levelCount; ++i)
        levels.logDimension[i] = packed.levels[i].nodes.w;
    auto owner = makeOwner(runtime, levels);
    VbxRawVolume raw;
    raw.packed = std::move(packed);
    VbxChannel density;
    density.format = AtlasFormat::R32_FLOAT;
    density.bytes.resize(scalar.size() * 4);
    std::memcpy(density.bytes.data(), scalar.data(), density.bytes.size());
    raw.channels.push_back(std::move(density));
    VbxChannel colors;
    colors.format = AtlasFormat::RGBA8_UINT;
    colors.bytes.resize(scalar.size() * 4);
    for (size_t i = 0; i < scalar.size(); ++i)
        std::memcpy(colors.bytes.data() + i * 4, &color, 4);
    raw.channels.push_back(std::move(colors));
    std::vector<uint8_t> bytes;
    std::string error;
    if (!saveRawVBX(raw, bytes, error))
        throw std::invalid_argument("Spray fixture: " + error);
    nvrhi::AutoPtr<nvrhi::IDataBlob> blob;
    check(nvrhi::CreateBlob(bytes.size(), &blob), "Spray fixture blob");
    std::memcpy(blob->GetDataPtr(), bytes.data(), bytes.size());
    p::SerializerCreateDesc create;
    create.volume = owner;
    p::SerializerHandle serializer;
    check(p::createSerializer(create, &serializer), "Spray fixture serializer");
    p::GenerationTicketHandle ticket;
    check(serializer->prepareLoadVBX(blob, {}, &ticket), "Spray fixture import");
    runtime.flush();
    Submission submission(runtime.device());
    submission.submit(
        [&](p::RecordingContext context) {
            check(owner->recordAndPublish(context, ticket), "Spray fixture publication");
        },
        true);
    return owner;
}
}  // namespace
struct SprayVolume::State : Submission {
    p::VolumeHandle owner;
    p::SnapshotHandle visible;
    p::PointOpsHandle points;
    p::GatherHandle gather;
    p::ApronOpsHandle apron;
    p::VoxelFiltersHandle filters;
    State(ethereal::slang::ComputeRuntime& runtime, p::VolumeHandle volume)
        : Submission(runtime.device()), owner(std::move(volume)) {
        if (!owner || owner->getDevice() != device.Get())
            throw std::invalid_argument("Spray owner device");
        check(owner->getSnapshot(&visible), "Spray snapshot");
        sample::OwnedVolumeRawViews checked(device, visible, 0, 1);
        auto binary = artifact(runtime, "GVDBPointStreamConversion_convertPointStream.bin");
        p::PointOpsCreateDesc pc;
        pc.volume = owner;
        pc.conversionArtifact = binary.data();
        pc.conversionArtifactBytes = binary.size();
        check(p::createPointOps(pc, &points), "Spray point operator");
        binary = artifact(runtime, "GVDBAtlasGather_gatherOwnedAtlas.bin");
        p::GatherCreateDesc gc;
        gc.volume = owner;
        gc.artifact = binary.data();
        gc.artifactBytes = binary.size();
        check(p::createGather(gc, &gather), "Spray gather operator");
        binary = artifact(runtime, "GVDBAtlasMutation_mutateAtlas.bin");
        p::ApronCreateDesc ac;
        ac.volume = owner;
        ac.artifact = binary.data();
        ac.artifactBytes = binary.size();
        check(p::createApronOps(ac, &apron), "Spray apron operator");
        binary = artifact(runtime, "GVDBAtlasFilters_filterAtlas.bin");
        p::VoxelFiltersCreateDesc fc;
        fc.volume = owner;
        fc.artifact = binary.data();
        fc.artifactBytes = binary.size();
        check(p::createVoxelFilters(fc, &filters), "Spray filter operator");
    }
    void publish(p::IGVDBGenerationTicket* ticket) {
        submit([&](p::RecordingContext context) {
            check(owner->recordAndPublish(context, ticket), "Spray publication");
        });
        check(owner->getSnapshot(&visible), "Spray visible snapshot");
    }
    std::vector<uint8_t> readChannel(uint32_t channel) {
        p::ReadbackHandle output;
        check(owner->prepareReadback(visible, 1u << channel, &output), "Spray readback");
        submit(
            [&](p::RecordingContext context) {
                check(owner->recordReadback(context, output), "Spray record readback");
            },
            true);
        check(output->finishAfterCallerCompletion(), "Spray finish readback");
        p::BorrowedBytes bytes;
        check(output->getChannelData(channel, bytes), "Spray channel bytes");
        std::vector<uint8_t> result(size_t(bytes.byteCount));
        if (bytes.byteCount)
            std::memcpy(result.data(), bytes.data, size_t(bytes.byteCount));
        return result;
    }
};
SprayVolume::SprayVolume(ethereal::slang::ComputeRuntime& runtime, PackedVolume packed,
                         const std::vector<float>& scalar, uint32_t color)
    : SprayVolume(runtime, importFixture(runtime, std::move(packed), scalar, color)) {}
SprayVolume::SprayVolume(ethereal::slang::ComputeRuntime& runtime, p::VolumeHandle owner)
    : m_runtime(&runtime), m_state(std::make_unique<State>(runtime, std::move(owner))) {}
SprayVolume::~SprayVolume() = default;
p::SnapshotHandle SprayVolume::snapshot() const { return m_state->visible; }
const p::IGVDBHostVolume& SprayVolume::topology() const { return *m_state->owner; }
std::vector<float> SprayVolume::readAtlas() const {
    m_runtime->flush();
    auto bytes = m_state->readChannel(0);
    std::vector<float> result(bytes.size() / 4);
    std::memcpy(result.data(), bytes.data(), bytes.size());
    return result;
}
std::vector<uint8_t> SprayVolume::readColor() const {
    m_runtime->flush();
    return m_state->readChannel(1);
}
PackedVolume SprayVolume::readTopology() const {
    m_runtime->flush();
    PackedVolume result;
    auto desc = m_state->visible->getDesc();
    result.volume = desc.volume;
    for (uint32_t i = 0; i < desc.levelCount; ++i) {
        p::LevelInfo level;
        check(m_state->visible->getLevel(i, level), "Spray topology level");
        result.levels.push_back(level);
    }
    auto read = [&](p::TopologyBuffer kind, auto& output) {
        p::BorrowedBufferBinding binding;
        check(m_state->visible->getTopologyBuffer(kind, binding), "Spray topology view");
        auto bytes = m_state->read(binding);
        output.resize(bytes.size() / sizeof(typename std::decay_t<decltype(output)>::value_type));
        if (!bytes.empty())
            std::memcpy(output.data(), bytes.data(), bytes.size());
    };
    read(p::TopologyBuffer::Nodes, result.nodes);
    read(p::TopologyBuffer::Children, result.children);
    read(p::TopologyBuffer::AtlasMap, result.atlasMap);
    return result;
}
double SprayVolume::interiorSum() const {
    auto packed = readTopology();
    auto values = readAtlas();
    const auto r = packed.volume.atlasResolutionAndApron;
    const auto& level = packed.levels[0];
    double sum = 0;
    for (uint32_t i = 0; i < level.nodes.z; ++i) {
        const auto* node = packed.nodes.data() + (level.nodes.x + i * level.nodes.y) / 4;
        if (!((node[0] >> 8) & 255))
            continue;
        for (int32_t z = 0; z < level.nodeExtent.z; ++z)
            for (int32_t y = 0; y < level.nodeExtent.y; ++y)
                for (int32_t x = 0; x < level.nodeExtent.x; ++x)
                    sum += values[(size_t(node[6] + z) * r.y + node[5] + y) * r.x + node[4] + x];
    }
    return sum;
}
void SprayVolume::refreshAprons(uint32_t mask) {
    m_runtime->flush();
    p::ApronRequestDesc request;
    request.channelMask = mask;
    p::GenerationTicketHandle ticket;
    check(m_state->apron->prepare(request, {}, &ticket), "Spray apron preparation");
    m_state->publish(ticket);
}
void SprayVolume::smooth(uint32_t iterations) {
    m_runtime->flush();
    p::VoxelFilterRequest request;
    request.iterations = iterations;
    request.parameters = {4, 0, 0, 0};
    p::GenerationTicketHandle ticket;
    check(m_state->filters->prepare(request, {}, &ticket), "Spray smooth preparation");
    m_state->publish(ticket);
}
void SprayVolume::depositPoints(nvrhi::IBuffer* positions, nvrhi::IBuffer* colors, uint32_t count) {
    if (count > UINT32_MAX / 16 || !positions || !colors || positions->getDesc().structStride != 16 ||
        colors->getDesc().structStride != 4 || positions->getDesc().byteSize < uint64_t(count) * 16 ||
        colors->getDesc().byteSize < uint64_t(count) * 4)
        throw std::invalid_argument("Spray point streams");
    std::vector<Float4> input(count), host;
    std::vector<uint32_t> inputColors(count), hostColors;
    if (count) {
        m_runtime->read(positions, input.data(), size_t(count) * 16);
        m_runtime->read(colors, inputColors.data(), size_t(count) * 4);
    } else
        m_runtime->flush();
    host.reserve(count);
    hostColors.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        auto v = input[i];
        if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z))
            continue;
        host.push_back(v);
        hostColors.push_back(inputColors[i]);
    }
    p::PointSetDesc desc;
    desc.positions.stream = {host.data(), host.size() * 16, 0, 16, host.size(), p::PointComponent::Float32, 4};
    if (!hostColors.empty())
        desc.colors.stream = {hostColors.data(), hostColors.size() * 4,    0, 4,
                              hostColors.size(), p::PointComponent::UInt8, 4};
    p::PointUploadTicketHandle upload;
    check(m_state->points->preparePoints(desc, {}, &upload), "Spray point preparation");
    m_state->submit(
        [&](p::RecordingContext context) {
            check(m_state->points->recordPoints(context, upload), "Spray point conversion");
        },
        true);
    p::PointSetHandle points;
    check(upload->finishAfterCallerCompletion(&points), "Spray point completion");
    p::GatherRequestDesc request;
    request.points = points;
    request.colorChannel = 1;
    request.subcellWidth = 4;
    request.radius = 1;
    request.supportRadius = 1;
    request.accumulate = 1;
    p::GenerationTicketHandle ticket;
    check(m_state->gather->prepare(request, {}, &ticket), "Spray gather preparation");
    m_state->publish(ticket);
    refreshAprons(3);
}
void SprayVolume::step(WandStyle style) {
    m_rays = m_wand.next(style);
    const auto count = uint32_t(m_rays.origins.size());
    auto origins = m_runtime->buffer(size_t(count) * 16, 16, m_rays.origins.data()),
         directions = m_runtime->buffer(size_t(count) * 16, 16, m_rays.directions.data()),
         colors = m_runtime->buffer(size_t(count) * 4, 4, m_rays.colors.data());
    auto query = sample::queryOwnedVolumeRays(*m_runtime, snapshot(), origins, directions, count);
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
        smooth(1);
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
    p::VoxelizeRequestDesc parameters;
    parameters.boundsMinimum = {float(origin.x), float(origin.y), float(origin.z), 0};
    parameters.boundsMaximum = {float(origin.x) + resolution.x, float(origin.y) + resolution.y,
                                float(origin.z) + resolution.z, 0};
    parameters.resolution = {resolution.x, resolution.y, resolution.z};
    parameters.values = {1, 1, 0, 0};
    parameters.transformRows[0] = {scale, 0, 0, translation.x};
    parameters.transformRows[1] = {0, scale, 0, translation.y};
    parameters.transformRows[2] = {0, 0, scale, translation.z};

    parameters.vertices = mesh.positions.data();
    parameters.vertexCount = mesh.positions.size();
    parameters.indices = mesh.indices.data();
    parameters.indexCount = mesh.indices.size();
    runtime.flush();
    Submission submission(runtime.device());
    p::VoxelizeCreateDesc voxelCreate;
    voxelCreate.device = runtime.device();
    std::array<std::vector<char>, 4> binaries;
    const char* names[] = {"GVDBMeshVoxelize_clearTriangleBins.bin", "GVDBMeshVoxelize_countTriangleBins.bin",
                           "GVDBMeshVoxelize_scatterTriangleBins.bin", "GVDBMeshVoxelize_solidVoxelize.bin"};
    for (unsigned i = 0; i < 4; ++i) {
        binaries[i] = artifact(runtime, names[i]);
        voxelCreate.artifacts[i] = {binaries[i].data(), binaries[i].size()};
    }
    p::VoxelizeHandle voxelizer;
    check(p::createVoxelize(voxelCreate, &voxelizer), "Spray voxelizer");
    p::VoxelizeTicketHandle voxelTicket;
    check(voxelizer->prepare(parameters, {}, &voxelTicket), "Spray voxelization preparation");
    submission.submit(
        [&](p::RecordingContext context) {
            check(voxelizer->recordCounts(context, voxelTicket), "Spray triangle counts");
        },
        true);
    check(voxelTicket->finishCountsAfterCallerCompletion(), "Spray triangle allocation");
    submission.submit(
        [&](p::RecordingContext context) {
            check(voxelizer->recordVoxelization(context, voxelTicket), "Spray voxelization");
        },
        true);
    p::BorrowedBytes classified;
    check(voxelTicket->finishAfterCallerCompletion(classified), "Spray classified dense voxels");
    std::vector<float> dense(size_t(classified.byteCount / 4));
    std::memcpy(dense.data(), classified.data, size_t(classified.byteCount));
    voxelTicket = nullptr;
    voxelizer = nullptr;
    p::LevelConfig levels;
    if (!p::LevelConfig::fromBranching(3, 3, 3, 3, 5, levels))
        throw std::logic_error("Spray level configuration");
    auto owner = makeOwner(runtime, levels);
    check(owner->setChannel(0, {}), "Spray density channel");
    check(owner->setChannel(1, {p::AtlasFormat::RGBA8_UINT, {}}), "Spray color channel");
    p::HostExtents extents;
    check(owner->computeExtents(1, parameters.boundsMinimum, parameters.boundsMaximum, extents),
          "Spray extents");
    const UInt4 cells{resolution.x / 32, resolution.y / 32, resolution.z / 32, 0};
    if (extents.cellWidth != 32 || extents.count != uint64_t(cells.x) * cells.y * cells.z)
        throw std::logic_error("Spray coarse grid");
    std::vector<uint8_t> active(size_t(extents.count), 0);
    for (uint32_t z = 0; z < resolution.z; ++z)
        for (uint32_t y = 0; y < resolution.y; ++y)
            for (uint32_t x = 0; x < resolution.x; ++x)
                if (dense[(size_t(z) * resolution.y + y) * resolution.x + x] != 0)
                    active[(size_t(z / 32) * cells.y + y / 32) * cells.x + x / 32] = 1;
    uint64_t selected = 0;
    check(owner->activateRegionFromMasks(extents, active.data(), active.size(), selected), "Spray activation");
    if (!selected)
        throw std::runtime_error("Spray mesh has no occupied voxels");
    check(owner->finishTopology(), "Spray finish topology");
    p::GenerationDesc generation;
    generation.placement.minimumBrickCount = {1, 1, 1};
    p::VolumeInfo info;
    check(owner->describePlacement(generation.placement, info), "Spray placement");
    auto atlas = info.atlasResolutionAndApron;
    std::vector<uint32_t> colors(size_t(atlas.x) * atlas.y * atlas.z, 0xffb2b2b2u);
    p::ChannelUploadDesc colorUpload{1, 0, colors.data(), colors.size() * 4};
    generation.uploads = &colorUpload;
    generation.uploadCount = 1;
    p::GenerationTicketHandle ticket;
    check(owner->prepareGeneration(generation, &ticket), "Spray initial generation");
    submission.submit([&](p::RecordingContext context) {
        check(owner->recordAndPublish(context, ticket), "Spray initial publication");
    });
    auto binary = artifact(runtime, "GVDBAtlasResample_resampleOwnedAtlas.bin");
    p::ResampleCreateDesc rc;
    rc.volume = owner;
    rc.artifact = binary.data();
    rc.artifactBytes = binary.size();
    p::ResampleHandle resample;
    check(p::createResample(rc, &resample), "Spray resample operator");
    p::ResampleRequestDesc request;
    request.sourceData = dense.data();
    request.sourceBytes = dense.size() * 4;
    request.sourceResolution = {resolution.x, resolution.y, resolution.z};
    request.preserveOutside = 0;
    request.transformRows[0].w = -float(origin.x);
    request.transformRows[1].w = -float(origin.y);
    request.transformRows[2].w = -float(origin.z);
    check(resample->prepare(request, {}, &ticket), "Spray dense-to-sparse preparation");
    submission.submit([&](p::RecordingContext context) {
        check(owner->recordAndPublish(context, ticket), "Spray dense publication");
    });
    auto result = std::make_unique<SprayVolume>(runtime, std::move(owner));
    result->smooth(2);
    result->refreshAprons(2);
    return result;
}
}  // namespace gvdb::samples::spray
