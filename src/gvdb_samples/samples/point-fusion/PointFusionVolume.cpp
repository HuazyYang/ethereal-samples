// SPDX-License-Identifier: Apache-2.0
#include "PointFusionVolume.h"
#include "ComputeRuntime.h"
#include <gvdb/GVDBNvrhiGatherApi.h>
#include <gvdb/GVDBNvrhiApronApi.h>
#include <donut/app/ApplicationBase.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
namespace gvdb::samples
{
namespace
{
namespace p = portable;
void require(bool v, const char *text)
{
    if (!v)
        throw std::invalid_argument(text);
}
void check(nvrhi::FRESULT result, const char *text)
{
    if (NVRHI_FAILED(result))
        throw std::runtime_error(std::string(text) + ": " + std::to_string(result));
}
std::vector<char> readArtifact(const std::filesystem::path &path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        throw std::runtime_error("Missing point-fusion artifact: " + path.string());
    return {(std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()};
}
p::PlacementDesc placement()
{
    p::PlacementDesc result;
    result.minimumBrickCount = {1, 1, 1};
    result.apron = 1;
    return result;
}
} // namespace
struct PointFusionVolume::State
{
    nvrhi::DeviceHandle device;
    nvrhi::CommandListHandle commands;
    p::VolumeHandle owner;
    p::PointOpsHandle points;
    p::GatherHandle gather;
    p::ApronOpsHandle apron;
    p::SnapshotHandle visible;
    p::PointSetHandle last;
    bool failed = false;
    template <class F> void submit(F &&record, bool wait = false)
    {
        struct Close
        {
            nvrhi::ICommandList *list;
            bool open = false;
            ~Close()
            {
                if (open)
                    list->close();
            }
        } scope{commands};
        commands->open();
        scope.open = true;
        record(p::RecordingContext{device, commands});
        commands->close();
        scope.open = false;
        const auto submission = device->executeCommandList(commands);
        if (!submission && device->getGraphicsAPI() != nvrhi::GraphicsAPI::D3D11)
            throw std::runtime_error("Point-fusion submission failed");
        if (wait)
        {
            if (!device->waitForIdle())
                throw std::runtime_error("Point-fusion completion failed");
            device->runGarbageCollection();
        }
    }
    void publish(p::IGVDBGenerationTicket *ticket)
    {
        submit([&](p::RecordingContext context) {
            check(owner->recordAndPublish(context, ticket), "Point-fusion publication");
        });
    }
};
PointFusionVolume::PointFusionVolume(ethereal::slang::ComputeRuntime &runtime, PointFusionConfiguration config,
                                     std::filesystem::path artifacts)
    : m_runtime(&runtime), m_configuration(config), m_artifacts(std::move(artifacts))
{
    require(std::isfinite(config.radius) && std::isfinite(config.support) && config.radius > 0 &&
                config.support >= config.radius && config.support <= 32,
            "Point fusion requires 0 < radius <= support <= 32");
    if (m_artifacts.empty())
        m_artifacts = donut::app::GetDirectoryWithExecutable() / "shaders/gvdb_owned" /
                      donut::app::GetShaderTypeName(runtime.device()->getGraphicsAPI());
    runtime.flush();
    m_state = createState(config.color);
}
PointFusionVolume::~PointFusionVolume() = default;
std::unique_ptr<PointFusionVolume::State> PointFusionVolume::createState(bool color)
{
    auto state = std::make_unique<State>();
    state->device = m_runtime->device();
    check(state->device->createCommandList({}, &state->commands), "Point-fusion command list");
    auto remap = readArtifact(m_artifacts / "GVDBAtlasRepack_relocateAtlas.bin");
    p::VolumeCreateDesc desc;
    desc.device = state->device;
    require(p::LevelConfig::fromBranching(3, 3, 3, 3, 5, desc.configuration), "Point-fusion level configuration");
    desc.remapArtifact = remap.data();
    desc.remapArtifactBytes = remap.size();
    check(p::createVolume(desc, &state->owner), "Point-fusion owner");
    p::ChannelDesc scalar;
    scalar.voidValue = {3, 0, 0, 0};
    check(state->owner->setChannel(0, scalar), "Point-fusion scalar channel");
    if (color)
    {
        p::ChannelDesc rgba;
        rgba.format = p::AtlasFormat::RGBA8_UINT;
        check(state->owner->setChannel(1, rgba), "Point-fusion color channel");
    }
    auto conversion = readArtifact(m_artifacts / "GVDBPointStreamConversion_convertPointStream.bin");
    p::PointOpsCreateDesc points;
    points.volume = state->owner;
    points.conversionArtifact = conversion.data();
    points.conversionArtifactBytes = conversion.size();
    check(p::createPointOps(points, &state->points), "Point-fusion point operator");
    auto gatherArtifact = readArtifact(m_artifacts / "GVDBAtlasGather_gatherOwnedAtlas.bin");
    p::GatherCreateDesc gather;
    gather.volume = state->owner;
    gather.artifact = gatherArtifact.data();
    gather.artifactBytes = gatherArtifact.size();
    check(p::createGather(gather, &state->gather), "Point-fusion gather operator");
    auto mutation = readArtifact(m_artifacts / "GVDBAtlasMutation_mutateAtlas.bin");
    p::ApronCreateDesc apron;
    apron.volume = state->owner;
    apron.artifact = mutation.data();
    apron.artifactBytes = mutation.size();
    check(p::createApronOps(apron, &state->apron), "Point-fusion apron operator");
    check(state->owner->finishTopology(), "Point-fusion empty topology");
    p::GenerationDesc generation;
    generation.placement = placement();
    p::GenerationTicketHandle ticket;
    check(state->owner->prepareGeneration(generation, &ticket), "Point-fusion empty preparation");
    state->publish(ticket);
    check(state->owner->getSnapshot(&state->visible), "Point-fusion empty snapshot");
    return state;
}
bool PointFusionVolume::hasVolume() const
{
    return m_state->visible->getDesc().activeLeafCount != 0;
}
p::SnapshotHandle PointFusionVolume::snapshot() const
{
    return m_state->visible;
}
const p::IGVDBHostVolume &PointFusionVolume::topology() const
{
    const auto actual = m_state->owner->getTopologyRevision(), expected = m_state->visible->getDesc().revision;
    if (actual.epoch != expected.epoch || actual.topology != expected.topology)
        throw std::logic_error("Point-fusion failed frame has unpublished display topology; reset required");
    return *m_state->owner;
}
p::PointSetHandle PointFusionVolume::lastPoints() const
{
    return m_state->last;
}
p::ReadbackHandle PointFusionVolume::readback(uint32_t mask) const
{
    m_runtime->flush();
    p::ReadbackHandle result;
    check(m_state->owner->prepareReadback(m_state->visible, mask, &result), "Point-fusion readback preparation");
    m_state->submit(
        [&](p::RecordingContext context) {
            check(m_state->owner->recordReadback(context, result), "Point-fusion readback record");
        },
        true);
    check(result->finishAfterCallerCompletion(), "Point-fusion readback completion");
    return result;
}
void PointFusionVolume::reset(bool color)
{
    m_runtime->flush();
    auto replacement = createState(color);
    m_state = std::move(replacement);
    m_configuration.color = color;
    m_frames = m_hits = 0;
}
void PointFusionVolume::fuse(const ScanFrame &frame)
{
    auto &state = *m_state;
    if (state.failed)
        throw std::logic_error("Point-fusion requires reset after a failed publication sequence");
    if (!frame.count)
    {
        ++m_frames;
        state.last = nullptr;
        return;
    }
    require(frame.count <= UINT32_MAX / 16 && frame.positions && frame.positions->getDesc().structStride == 16 &&
                frame.positions->getDesc().byteSize >= uint64_t(frame.count) * 16 &&
                (!m_configuration.color || (frame.colors && frame.colors->getDesc().structStride == 4 &&
                                            frame.colors->getDesc().byteSize >= uint64_t(frame.count) * 4)),
            "Point fusion requires complete compact position/color buffers");
    std::vector<slang::Float4> positions(frame.count);
    m_runtime->read(frame.positions, positions.data(), positions.size() * 16);
    std::vector<uint32_t> colors(frame.count, 0xffffffffu);
    if (frame.colors && frame.colors->getDesc().structStride == 4 &&
        frame.colors->getDesc().byteSize >= uint64_t(frame.count) * 4)
        m_runtime->read(frame.colors, colors.data(), colors.size() * 4);
    for (const auto &position : positions)
        require(std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z) &&
                    position.w == 1 && std::abs(position.x) <= 4000000 && std::abs(position.y) <= 4000000 &&
                    std::abs(position.z) <= 4000000,
                "Point fusion requires valid positions in the exact half-voxel coordinate domain");
    p::PointSetDesc input;
    input.positions.stream = {positions.data(), positions.size() * 16,      0, 16,
                              frame.count,      p::PointComponent::Float32, 4};
    input.colors.stream = {colors.data(), colors.size() * 4, 0, 4, frame.count, p::PointComponent::UInt8, 4};
    p::PointUploadTicketHandle upload;
    check(state.points->preparePoints(input, {}, &upload), "Point-fusion point preparation");
    state.submit(
        [&](p::RecordingContext context) {
            check(state.points->recordPoints(context, upload), "Point-fusion point conversion");
        },
        true);
    p::PointSetHandle points;
    check(upload->finishAfterCallerCompletion(&points), "Point-fusion point completion");
    p::PointTopologyDesc topology;
    topology.mode = p::PointTopologyMode::Accumulate;
    topology.radius = m_configuration.support;
    topology.coverage = p::PointSupportCoverage::FloatSupport;
    p::GenerationTicketHandle ticket;
    check(state.points->prepareTopology(points, topology, placement(), {}, &ticket),
          "Point-fusion topology preparation");
    // Each completed preparation is published/submitted in order. Preparation
    // failure after the first publication cannot undo an earlier operation; the
    // app preserves its last visible snapshot and requires reset after failure.
    try
    {
        state.publish(ticket);
        ticket = nullptr;
        p::GatherRequestDesc gather;
        gather.kind = p::GatherKind::LevelSet;
        gather.channel = 0;
        gather.colorChannel = m_configuration.color ? 1 : UINT32_MAX;
        gather.points = points;
        gather.subcellWidth = 4;
        gather.radius = m_configuration.radius;
        gather.supportRadius = m_configuration.support;
        gather.accumulate = 1;
        p::GatherPreparationBudget budget;
        budget.maxPointPairs = UINT32_MAX / 16;
        check(state.gather->prepare(gather, budget, &ticket), "Point-fusion gather preparation");
        state.publish(ticket);
        ticket = nullptr;
        p::ApronBoundaryOverride overrides[2];
        overrides[1].channel = 1;
        p::ApronRequestDesc apron;
        apron.channelMask = m_configuration.color ? 3 : 1;
        apron.overrideCount = m_configuration.color ? 2 : 1;
        apron.overrides = overrides;
        check(state.apron->prepare(apron, {}, &ticket), "Point-fusion apron preparation");
        state.publish(ticket);
        p::SnapshotHandle snapshot;
        check(state.owner->getSnapshot(&snapshot), "Point-fusion snapshot");
        state.visible = std::move(snapshot);
        state.last = std::move(points);
        ++m_frames;
        m_hits += frame.count;
    }
    catch (...)
    {
        state.failed = true;
        throw;
    }
}
} // namespace gvdb::samples
