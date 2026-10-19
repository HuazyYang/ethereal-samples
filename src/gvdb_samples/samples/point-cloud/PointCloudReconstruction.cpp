// SPDX-License-Identifier: Apache-2.0
#include "PointCloudReconstruction.h"
#include "ComputeRuntime.h"
#include <gvdb/GVDBSlangPointConversion.h>
#include <gvdb/GVDBNvrhiGatherApi.h>
#include <gvdb/GVDBNvrhiApronApi.h>
#include <gvdb/GVDBNvrhiVoxelFilterApi.h>
#include <donut/app/ApplicationBase.h>
#include <fstream>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <tuple>
namespace gvdb::samples::pointcloud {
using namespace slang;
using nvrhi::BindingSetItem;
namespace p = portable;
namespace {
void check(nvrhi::FRESULT result, const char* message) {
    if (NVRHI_FAILED(result))
        throw std::runtime_error(std::string(message) + ": " + std::to_string(result));
}
std::vector<char> artifact(ethereal::slang::ComputeRuntime& runtime, const char* name) {
    const auto path = donut::app::GetDirectoryWithExecutable() / "shaders/gvdb_point_cloud" /
                      donut::app::GetShaderTypeName(runtime.device()->getGraphicsAPI()) / name;
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("Missing point-cloud artifact: " + path.string());
    return {(std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>()};
}
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
struct PointCloudReconstruction::State {
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
            throw std::runtime_error("Point-cloud submission failed");
        if (wait) {
            if (!device->waitForIdle())
                throw std::runtime_error("Point-cloud completion failed");
            device->runGarbageCollection();
        }
    }
    void publish(p::IGVDBGenerationTicket* ticket) {
        submit([&](p::RecordingContext context) {
            check(owner->recordAndPublish(context, ticket), "Point-cloud publication");
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
              "Point-cloud topology staging");
        submit(
            [&](p::RecordingContext context) {
                context.commands->copyBuffer(staging, 0, binding.buffer, binding.byteOffset, binding.byteSize);
            },
            true);
        std::vector<uint8_t> result(size_t(binding.byteSize));
        const void* mapped = device->mapBuffer(staging, nvrhi::CpuAccessMode::Read);
        if (!mapped)
            throw std::runtime_error("Point-cloud topology map failed");
        std::memcpy(result.data(), mapped, result.size());
        device->unmapBuffer(staging);
        return result;
    }
};
std::unique_ptr<PointCloudReconstruction::State> PointCloudReconstruction::createState() const {
    m_runtime->flush();
    auto state = std::make_unique<State>();
    state->device = m_runtime->device();
    check(state->device->createCommandList({}, &state->commands), "Point-cloud command list");
    auto binary = artifact(*m_runtime, "GVDBAtlasRepack_relocateAtlas.bin");
    p::VolumeCreateDesc desc;
    desc.device = state->device;
    if (!p::LevelConfig::fromBranching(3, 3, 3, 3, 4, desc.configuration))
        throw std::logic_error("Point-cloud levels");
    desc.remapArtifact = binary.data();
    desc.remapArtifactBytes = binary.size();
    check(p::createVolume(desc, &state->owner), "Point-cloud owner");
    p::ChannelDesc channel;
    channel.voidValue = {3, 0, 0, 0};
    check(state->owner->setChannel(0, channel), "Point-cloud scalar channel");
    return state;
}
PointCloudReconstruction::PointCloudReconstruction(ethereal::slang::ComputeRuntime& runtime)
    : m_runtime(&runtime) {
    m_state = createState();
    check(m_state->owner->finishTopology(), "Point-cloud empty topology");
    p::GenerationDesc generation;
    generation.placement.minimumBrickCount = {1, 1, 1};
    p::GenerationTicketHandle ticket;
    check(m_state->owner->prepareGeneration(generation, &ticket), "Point-cloud empty generation");
    m_state->publish(ticket);
    check(m_state->owner->getSnapshot(&m_state->visible), "Point-cloud empty snapshot");
}
PointCloudReconstruction::~PointCloudReconstruction() = default;
p::SnapshotHandle PointCloudReconstruction::snapshot() const { return m_state->visible; }
const p::IGVDBHostVolume& PointCloudReconstruction::topology() const { return *m_state->owner; }
std::vector<float> PointCloudReconstruction::readAtlas() const {
    m_runtime->flush();
    p::ReadbackHandle read;
    check(m_state->owner->prepareReadback(m_state->visible, 1, &read), "Point-cloud readback preparation");
    m_state->submit(
        [&](p::RecordingContext context) {
            check(m_state->owner->recordReadback(context, read), "Point-cloud readback record");
        },
        true);
    check(read->finishAfterCallerCompletion(), "Point-cloud readback completion");
    p::BorrowedBytes bytes;
    check(read->getChannelData(0, bytes), "Point-cloud atlas bytes");
    std::vector<float> result(size_t(bytes.byteCount / 4));
    if (bytes.byteCount)
        std::memcpy(result.data(), bytes.data, size_t(bytes.byteCount));
    return result;
}
PackedVolume PointCloudReconstruction::readTopology() const {
    m_runtime->flush();
    PackedVolume result;
    const auto desc = m_state->visible->getDesc();
    result.volume = desc.volume;
    for (uint32_t i = 0; i < desc.levelCount; ++i) {
        p::LevelInfo level;
        check(m_state->visible->getLevel(i, level), "Point-cloud snapshot level");
        result.levels.push_back(level);
    }
    auto read = [&](p::TopologyBuffer kind, auto& destination) {
        p::BorrowedBufferBinding binding;
        check(m_state->visible->getTopologyBuffer(kind, binding), "Point-cloud topology binding");
        auto bytes = m_state->read(binding);
        using Value = typename std::decay_t<decltype(destination)>::value_type;
        if (bytes.size() % sizeof(Value))
            throw std::logic_error("Point-cloud topology byte count");
        destination.resize(bytes.size() / sizeof(Value));
        if (!bytes.empty())
            std::memcpy(destination.data(), bytes.data(), bytes.size());
    };
    read(p::TopologyBuffer::Nodes, result.nodes);
    read(p::TopologyBuffer::Children, result.children);
    read(p::TopologyBuffer::AtlasMap, result.atlasMap);
    return result;
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
    // Construct a fresh public owner; failures leave the previous snapshot visible.
    auto state = createState();
    for (const auto& origin : leaves) {
        p::NodeId id;
        check(state->owner->activateAtLevel(0, {std::get<0>(origin), std::get<1>(origin), std::get<2>(origin)},
                                            id),
              "Point-cloud leaf activation");
    }
    check(state->owner->finishTopology(), "Point-cloud topology finish");
    p::GenerationDesc generation;
    generation.placement.minimumBrickCount = {1, 1, 1};
    p::GenerationTicketHandle ticket;
    check(state->owner->prepareGeneration(generation, &ticket), "Point-cloud generation preparation");
    state->publish(ticket);
    ticket = nullptr;

    // Quantize each ordinal once. The gather extension keeps membership based on
    // original positions even if this GPU calculation crosses a cell boundary.
    auto compressed = quantizePointsU16(*m_runtime, positions, frame.count(), bounds.minimum, bounds.range);
    auto decoded = m_runtime->buffer(size_t(frame.count()) * 16, 16);
    Parameters parameters{
        {frame.count(), 0, 0, 0},
        bounds.minimum, bounds.range
    };
    launch(*m_runtime, "decodeSubcellPoints", parameters,
           {BindingSetItem::RawBuffer_SRV(0, compressed), BindingSetItem::StructuredBuffer_UAV(0, decoded)},
           frame.count());
    std::vector<Float4> evaluated(frame.count());
    m_runtime->read(decoded, evaluated.data(), evaluated.size() * sizeof(Float4));
    // Explicit application readback feeds copied public point inputs. Preparation
    // itself neither reads back nor submits, and CPU never recomputes quantization.
    auto conversionBinary = artifact(*m_runtime, "GVDBPointStreamConversion_convertPointStream.bin");
    p::PointOpsCreateDesc pointCreate;
    pointCreate.volume = state->owner;
    pointCreate.conversionArtifact = conversionBinary.data();
    pointCreate.conversionArtifactBytes = conversionBinary.size();
    p::PointOpsHandle points;
    check(p::createPointOps(pointCreate, &points), "Point-cloud point operator");
    auto upload = [&](const std::vector<Float4>& values) {
        p::PointSetDesc desc;
        desc.positions.stream = {values.data(), values.size() * 16,         0, 16,
                                 values.size(), p::PointComponent::Float32, 4};
        p::PointUploadTicketHandle upload;
        check(points->preparePoints(desc, {}, &upload), "Point-cloud point preparation");
        state->submit(
            [&](p::RecordingContext context) {
                check(points->recordPoints(context, upload), "Point-cloud point upload");
            },
            true);
        p::PointSetHandle result;
        check(upload->finishAfterCallerCompletion(&result), "Point-cloud point completion");
        return result;
    };
    auto associations = upload(host), evaluation = upload(evaluated);
    auto gatherBinary = artifact(*m_runtime, "GVDBAtlasGather_gatherOwnedAtlas.bin");
    p::GatherCreateDesc gatherCreate;
    gatherCreate.volume = state->owner;
    gatherCreate.artifact = gatherBinary.data();
    gatherCreate.artifactBytes = gatherBinary.size();
    p::GatherHandle gather;
    check(p::createGather(gatherCreate, &gather), "Point-cloud gather");
    p::GatherEvaluationHandle extension;
    check(gather->QueryInterface(nvrhi::uuid_of<p::IGVDBGatherEvaluation>(),
                                 reinterpret_cast<void**>(extension.GetAddressOf())),
          "Point-cloud gather extension");
    p::GatherEvaluationDesc request;
    request.request.points = associations;
    request.evaluationPoints = evaluation;
    request.request.kind = p::GatherKind::LevelSet;
    request.request.subcellWidth = 4;
    request.request.radius = 1;
    request.request.supportRadius = 1;
    check(extension->prepareEvaluation(request, {}, &ticket), "Point-cloud gather preparation");
    state->publish(ticket);
    ticket = nullptr;
    auto mutation = artifact(*m_runtime, "GVDBAtlasMutation_mutateAtlas.bin");
    p::ApronCreateDesc apronCreate;
    apronCreate.volume = state->owner;
    apronCreate.artifact = mutation.data();
    apronCreate.artifactBytes = mutation.size();
    p::ApronOpsHandle apron;
    check(p::createApronOps(apronCreate, &apron), "Point-cloud apron operator");
    p::ApronRequestDesc apronRequest;
    apronRequest.channelMask = 1;
    check(apron->prepare(apronRequest, {}, &ticket), "Point-cloud apron preparation");
    state->publish(ticket);
    ticket = nullptr;
    if (settings.smoothingIterations) {
        auto filterBinary = artifact(*m_runtime, "GVDBAtlasFilters_filterAtlas.bin");
        p::VoxelFiltersCreateDesc filterCreate;
        filterCreate.volume = state->owner;
        filterCreate.artifact = filterBinary.data();
        filterCreate.artifactBytes = filterBinary.size();
        p::VoxelFiltersHandle filters;
        check(p::createVoxelFilters(filterCreate, &filters), "Point-cloud filter operator");
        p::VoxelFilterRequest filter;
        filter.iterations = settings.smoothingIterations;
        filter.parameters = {settings.smoothingCenterWeight, settings.smoothingBias, 0, 0};
        filter.boundaryValue = {3, 0, 0, 0};
        check(filters->prepare(filter, {}, &ticket), "Point-cloud smoothing preparation");
        state->publish(ticket);
    }
    check(state->owner->getSnapshot(&state->visible), "Point-cloud snapshot");
    m_state = std::move(state);
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
