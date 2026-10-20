// SPDX-License-Identifier: Apache-2.0
#include "PointCloudReconstruction.h"
#include "ComputeRuntime.h"
#include <gvdb/GVDBNvrhiPointQuantizeApi.h>
#include <gvdb/GVDBSlangPointKeys.h>
#include <gvdb/GVDBNvrhiTopologyEditApi.h>
#include <array>
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
namespace p = portable;
namespace {
void check(nvrhi::FRESULT result, const char* message) {
    if (NVRHI_FAILED(result))
        throw std::runtime_error(std::string(message) + ": " + std::to_string(result));
}
std::vector<char> artifact(ethereal::slang::ComputeRuntime& runtime, const char* name,
                           const char* directory = "gvdb_point_cloud") {
    const auto path = donut::app::GetDirectoryWithExecutable() / "shaders" / directory /
                      donut::app::GetShaderTypeName(runtime.device()->getGraphicsAPI()) / name;
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("Missing point-cloud artifact: " + path.string());
    return {(std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>()};
}
}  // namespace
struct PointCloudReconstruction::State {
    nvrhi::DeviceHandle device;
    nvrhi::CommandListHandle commands;
    p::VolumeHandle owner;
    p::SnapshotHandle visible;
    p::PointQuantizeTicketHandle quantized;
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
std::unique_ptr<PointCloudReconstruction::State> PointCloudReconstruction::createState(
    bool scalarChannel) const {
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
    if (scalarChannel) {
        p::ChannelDesc channel;
        channel.voidValue = {3, 0, 0, 0};
        check(state->owner->setChannel(0, channel), "Point-cloud scalar channel");
    }
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
    // Build privately; only a completely reconstructed frame becomes visible.
    auto state = createState(false);
    auto conversionBinary = artifact(*m_runtime, "GVDBPointStreamConversion_convertPointStream.bin");
    p::PointOpsCreateDesc pointCreate;
    pointCreate.volume = state->owner;
    pointCreate.conversionArtifact = conversionBinary.data();
    pointCreate.conversionArtifactBytes = conversionBinary.size();
    p::PointOpsHandle points;
    check(p::createPointOps(pointCreate, &points), "Point-cloud point operator");
    std::vector<std::array<uint16_t, 4>> source(frame.count());
    for (size_t i = 0; i < source.size(); ++i)
        source[i] = {frame.coordinates[i * 3], frame.coordinates[i * 3 + 1], frame.coordinates[i * 3 + 2], 1};
    p::PointSetDesc input;
    input.positions.stream = {source.data(), source.size() * 8,         0, 8,
                              source.size(), p::PointComponent::UInt16, 4};
    input.positions.transform.minimum = {frame.minimum.x, frame.minimum.y, frame.minimum.z, 0};
    input.positions.transform.delta = {(frame.maximum.x - frame.minimum.x) / 65535.f,
                                       (frame.maximum.y - frame.minimum.y) / 65535.f,
                                       (frame.maximum.z - frame.minimum.z) / 65535.f, 1};
    input.positions.transform.scale = {settings.renderScale, settings.renderScale, settings.renderScale, 1};
    p::PointUploadTicketHandle upload;
    check(points->preparePoints(input, {}, &upload), "Point-cloud source preparation");
    state->submit(
        [&](p::RecordingContext context) {
            check(points->recordPoints(context, upload), "Point-cloud source conversion");
        },
        true);
    p::PointSetHandle associations;
    check(upload->finishAfterCallerCompletion(&associations), "Point-cloud source completion");
    p::BorrowedBytes converted;
    check(associations->getData(p::PointAttribute::Position, converted), "Point-cloud source positions");
    std::vector<Float4> host(frame.count());
    if (converted.byteCount != host.size() * sizeof(Float4))
        throw std::logic_error("Point-cloud converted byte count");
    if (converted.byteCount)
        std::memcpy(host.data(), converted.data, size_t(converted.byteCount));
    auto encode = artifact(*m_runtime, "GVDBPointQuantize_encodePointU16.bin", "gvdb_quantize");
    auto decode = artifact(*m_runtime, "GVDBPointQuantize_decodePointU16.bin", "gvdb_quantize");
    p::PointQuantizeCreateDesc quantizeCreate;
    quantizeCreate.device = state->device;
    quantizeCreate.encodeArtifact = encode.data();
    quantizeCreate.encodeArtifactBytes = encode.size();
    quantizeCreate.decodeArtifact = decode.data();
    quantizeCreate.decodeArtifactBytes = decode.size();
    p::PointQuantizeHandle quantize;
    check(p::createPointQuantize(quantizeCreate, &quantize), "Point-cloud quantizer");
    p::PointQuantizeRequestDesc quantizeRequest;
    quantizeRequest.points = associations;
    quantizeRequest.bounds = p::PointQuantizationBounds::LegacyPadded;
    check(quantize->prepare(quantizeRequest, {}, &state->quantized), "Point-cloud quantization preparation");
    QuantizationBounds bounds;
    state->quantized->getBounds(bounds.minimum, bounds.range);
    state->submit(
        [&](p::RecordingContext context) {
            check(quantize->record(context, state->quantized), "Point-cloud quantization record");
        },
        true);
    p::PointSetHandle evaluation;
    check(state->quantized->finishAfterCallerCompletion(&evaluation), "Point-cloud quantization completion");
    bool signedAxis[3] = {false, false, false};
    for (const auto& p : host) {
        signedAxis[0] |= p.x < 0;
        signedAxis[1] |= p.y < 0;
        signedAxis[2] |= p.z < 0;
    }
    // Legacy fromBranching(r4,r3,r2,r1,r0) places r0=4 at the leaf: width16.
    std::set<std::tuple<int32_t, int32_t, int32_t>> leaves;
    // Keep support enumeration in checked signed coordinates. Normalize each
    // bounded batch by its integer minimum before GPU key generation, stable
    // radix sorting and scan compaction. Merge only compacted batches on the CPU.
    constexpr size_t keyBatchCapacity = 1u << 20;
    std::vector<std::array<int32_t, 3>> candidates;
    candidates.reserve(std::min(keyBatchCapacity, host.size()));
    uint32_t keyBatches = 0;
    auto flushKeys = [&] {
        if (candidates.empty())
            return;
        auto minimum = candidates.front(), maximum = minimum;
        for (const auto& candidate : candidates)
            for (size_t axis = 0; axis < 3; ++axis) {
                minimum[axis] = std::min(minimum[axis], candidate[axis]);
                maximum[axis] = std::max(maximum[axis], candidate[axis]);
            }
        for (size_t axis = 0; axis < 3; ++axis)
            if (int64_t(maximum[axis]) - minimum[axis] > 65535ll * 16)
                throw std::invalid_argument("Point-cloud support exceeds the GPU key coordinate domain");
        std::vector<Float4> normalized;
        normalized.reserve(candidates.size());
        for (const auto& candidate : candidates)
            normalized.push_back({float(int64_t(candidate[0]) - minimum[0]),
                                  float(int64_t(candidate[1]) - minimum[1]),
                                  float(int64_t(candidate[2]) - minimum[2]), 1});
        auto positions =
            m_runtime->buffer(normalized.size() * sizeof(Float4), sizeof(Float4), normalized.data());
        auto discovered = discoverBrickKeys(*m_runtime, positions, uint32_t(normalized.size()), {16});
        if (discovered.hasInvalidPositions || discovered.levelCounts[0] != discovered.count ||
            discovered.count > normalized.size())
            throw std::runtime_error("Point-cloud GPU key discovery returned invalid support");
        std::vector<KeyValue> compacted(discovered.count);
        if (!compacted.empty())
            m_runtime->read(discovered.keys, compacted.data(), compacted.size() * sizeof(KeyValue));
        for (const auto& key : compacted) {
            const int64_t x = int64_t(minimum[0]) + int64_t(key.key.y & 65535u) * 16;
            const int64_t y = int64_t(minimum[1]) + int64_t(key.key.x >> 16) * 16;
            const int64_t z = int64_t(minimum[2]) + int64_t(key.key.x & 65535u) * 16;
            if ((key.key.y >> 16) || x > maximum[0] || y > maximum[1] || z > maximum[2])
                throw std::runtime_error("Point-cloud compacted key is outside its declared bounds");
            leaves.emplace(int32_t(x), int32_t(y), int32_t(z));
        }
        candidates.clear();
        ++keyBatches;
    };
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
                for (int32_t x = lo[0]; x <= hi[0]; x += 16) {
                    candidates.push_back({x, y, z});
                    if (candidates.size() == keyBatchCapacity)
                        flushKeys();
                }
    }
    flushKeys();
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

    // Host allocation establishes trusted counts, coordinates and the root. The
    // private GPU transaction clears/rebuilds every child list and reciprocal
    // parent link, then validates the fetched graph before atlas allocation.
    {
        p::TopologyEditCreateDesc editCreate;
        editCreate.volume = state->owner;
        p::TopologyEditHandle edit;
        check(p::createTopologyEdit(editCreate, &edit), "Point-cloud topology editor");
        p::TopologyEditDesc editOptions;
        editOptions.placement = generation.placement;
        p::TopologyEditTicketHandle editTicket;
        check(edit->prepare(editOptions, &editTicket), "Point-cloud topology edit preparation");
        p::TopologyActivityCreateDesc activityCreate;
        activityCreate.device = state->device;
        const char* entries[] = {"applyLeafMarkers", "delinkLeafBricks", "delinkInternalBricks",
                                 "initializeChildLists", "linkBricks"};
        std::array<std::vector<char>, 5> binaries;
        for (size_t i = 0; i < binaries.size(); ++i) {
            binaries[i] = artifact(*m_runtime, (std::string("GVDBTopology_") + entries[i] + ".bin").c_str(),
                                   "gvdb_slang");
            activityCreate.artifacts[i] = {binaries[i].data(), binaries[i].size()};
        }
        p::TopologyActivityHandle activity;
        check(p::createTopologyActivity(activityCreate, &activity), "Point-cloud topology activity");
        const std::vector<uint32_t> active(leaves.size(), 1);
        nvrhi::BufferHandle markers;
        p::TopologyActivityDesc activityOptions;
        if (!active.empty()) {
            nvrhi::BufferDesc desc;
            desc.byteSize = active.size() * 4;
            desc.structStride = 4;
            desc.initialState = nvrhi::ResourceStates::ShaderResource;
            desc.keepInitialState = true;
            check(state->device->createBuffer(desc, &markers), "Point-cloud leaf markers");
            activityOptions.markers = {markers.Get(), 0, desc.byteSize};
        }
        state->submit(
            [&](p::RecordingContext context) {
                check(edit->recordBegin(context, editTicket), "Point-cloud topology begin");
                if (markers)
                    context.commands->writeBuffer(markers, active.data(), active.size() * 4);
                check(activity->record(context, editTicket, activityOptions), "Point-cloud GPU topology links");
                check(edit->recordFetch(context, editTicket), "Point-cloud topology fetch");
            },
            true);
        check(editTicket->finishAfterCallerCompletion(&ticket), "Point-cloud fetched graph validation");
        state->publish(ticket);
        ticket = nullptr;
    }
    p::ChannelDesc channel;
    channel.voidValue = {3, 0, 0, 0};
    check(state->owner->setChannel(0, channel), "Point-cloud scalar channel");
    check(state->owner->prepareGeneration(generation, &ticket), "Point-cloud atlas allocation");
    state->publish(ticket);
    ticket = nullptr;

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
    m_hostPositions = std::move(host);
    m_quantization = bounds;
    m_keyBatches = keyBatches;
    ++m_generation;
}
std::vector<uint16_t> PointCloudReconstruction::quantizationCodes() const {
    if (!m_state->quantized)
        return {};
    p::BorrowedBytes codes;
    check(m_state->quantized->getCodes(codes), "Point-cloud quantized codes");
    std::vector<uint16_t> result(size_t(codes.byteCount / 2));
    if (codes.byteCount)
        std::memcpy(result.data(), codes.data, size_t(codes.byteCount));
    return result;
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
