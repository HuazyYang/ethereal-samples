// SPDX-License-Identifier: Apache-2.0
#include "ResampleVolume.h"
#include "ComputeRuntime.h"
#include <gvdb/GVDBNvrhiResampleApi.h>
#include <donut/app/ApplicationBase.h>
#include <cstring>
#include <iterator>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <utility>
namespace gvdb::samples {
using namespace slang;
namespace p = portable;
namespace {
void check(nvrhi::FRESULT r, const char* what) {
    if (NVRHI_FAILED(r))
        throw std::runtime_error(std::string(what) + ": " + std::to_string(r));
}
std::vector<char> artifact(ethereal::slang::ComputeRuntime& runtime, const char* directory, const char* name) {
    auto path = donut::app::GetDirectoryWithExecutable() / "shaders" / directory /
                donut::app::GetShaderTypeName(runtime.device()->getGraphicsAPI()) / name;
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("Missing resample artifact: " + path.string());
    return {(std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>()};
}
template <class F>
void submit(nvrhi::IDevice* device, nvrhi::ICommandList* commands, F&& record, bool wait = false) {
    struct Close {
        nvrhi::ICommandList* list;
        bool open = false;
        ~Close() {
            if (open)
                list->close();
        }
    } scope{commands};
    commands->open();
    scope.open = true;
    record(p::RecordingContext{device, commands});
    commands->close();
    scope.open = false;
    if (!device->executeCommandList(commands) && device->getGraphicsAPI() != nvrhi::GraphicsAPI::D3D11)
        throw std::runtime_error("Resample submission failed");
    if (wait) {
        if (!device->waitForIdle())
            throw std::runtime_error("Resample completion failed");
        device->runGarbageCollection();
    }
}
}  // namespace
struct ResampleVolume::State {
    nvrhi::DeviceHandle device;
    nvrhi::CommandListHandle commands;
    p::VolumeHandle owner;
    p::SnapshotHandle visible;
    void publish(p::IGVDBGenerationTicket* ticket) {
        submit(device, commands, [&](p::RecordingContext context) {
            check(owner->recordAndPublish(context, ticket), "Resample publication");
        });
    }
};
std::vector<float> queryResampleRegions(ethereal::slang::ComputeRuntime& runtime,
                                        const std::vector<float>& values, UInt4 source, UInt4 destination,
                                        Float4 extent, const std::array<Float4, 3>& transform) {
    runtime.flush();
    auto binary = artifact(runtime, "gvdb_downsample", "GVDBDenseDownsample_downsampleDenseRegion.bin");
    p::DownsampleCreateDesc create;
    create.device = runtime.device();
    create.artifact = binary.data();
    create.artifactBytes = binary.size();
    p::DownsampleHandle op;
    check(p::createDownsample(create, &op), "Resample downsample factory");
    p::DownsampleRequestDesc request;
    request.sourceData = values.data();
    request.sourceBytes = values.size() * sizeof(float);
    request.sourceResolution = {source.x, source.y, source.z};
    request.destinationResolution = {destination.x, destination.y, destination.z};
    request.destinationExtent = extent;
    std::copy(transform.begin(), transform.end(), request.transformRows);
    p::DownsampleTicketHandle ticket;
    check(op->prepare(request, {}, &ticket), "Resample downsample prepare");
    nvrhi::CommandListHandle commands;
    check(runtime.device()->createCommandList({}, &commands), "Resample query commands");
    submit(
        runtime.device(), commands,
        [&](p::RecordingContext context) { check(op->record(context, ticket), "Resample downsample record"); },
        true);
    p::BorrowedBytes bytes;
    check(ticket->finishAfterCallerCompletion(bytes), "Resample downsample finish");
    std::vector<float> result(size_t(bytes.byteCount / sizeof(float)));
    if (bytes.byteCount)
        std::memcpy(result.data(), bytes.data, size_t(bytes.byteCount));
    return result;
}
ResampleVolume::~ResampleVolume() = default;
p::SnapshotHandle ResampleVolume::snapshot() const { return m_state ? m_state->visible : p::SnapshotHandle{}; }
const p::IGVDBHostVolume& ResampleVolume::topology() const {
    if (!m_state)
        throw std::logic_error("No resample publication");
    return *m_state->owner;
}
std::vector<float> ResampleVolume::readAtlas() {
    m_runtime->flush();
    p::ReadbackHandle read;
    check(m_state->owner->prepareReadback(m_state->visible, 1, &read), "Resample prepare readback");
    submit(
        m_state->device, m_state->commands,
        [&](p::RecordingContext context) {
            check(m_state->owner->recordReadback(context, read), "Resample record readback");
        },
        true);
    check(read->finishAfterCallerCompletion(), "Resample finish readback");
    p::BorrowedBytes bytes;
    check(read->getChannelData(0, bytes), "Resample channel bytes");
    std::vector<float> result(size_t(bytes.byteCount / 4));
    if (bytes.byteCount)
        std::memcpy(result.data(), bytes.data, size_t(bytes.byteCount));
    return result;
}
std::vector<AtlasNode> ResampleVolume::readAtlasMap() {
    m_runtime->flush();
    p::BorrowedBufferBinding binding;
    check(m_state->visible->getTopologyBuffer(p::TopologyBuffer::AtlasMap, binding), "Resample atlas map");
    if (binding.byteSize % sizeof(AtlasNode))
        throw std::logic_error("Malformed snapshot atlas map");
    std::vector<AtlasNode> result(size_t(binding.byteSize / sizeof(AtlasNode)));
    if (result.empty())
        return result;
    nvrhi::BufferHandle staging;
    check(m_state->device->createBuffer(nvrhi::BufferDesc()
                                            .setByteSize(binding.byteSize)
                                            .setCpuAccess(nvrhi::CpuAccessMode::Read)
                                            .setInitialState(nvrhi::ResourceStates::CopyDest)
                                            .setKeepInitialState(true),
                                        &staging),
          "Resample atlas map staging");
    submit(
        m_state->device, m_state->commands,
        [&](p::RecordingContext context) {
            context.commands->copyBuffer(staging, 0, binding.buffer, binding.byteOffset, binding.byteSize);
        },
        true);
    const void* data = m_state->device->mapBuffer(staging, nvrhi::CpuAccessMode::Read);
    if (!data)
        throw std::runtime_error("Resample map failed");
    std::memcpy(result.data(), data, size_t(binding.byteSize));
    m_state->device->unmapBuffer(staging);
    return result;
}
ResampleVolume::ResampleVolume(ethereal::slang::ComputeRuntime& runtime, const std::vector<float>& values,
                               ResampleConfiguration configuration)
    : m_runtime(&runtime), m_configuration(std::move(configuration)) {
    const auto source = m_configuration.sourceResolution, destination = m_configuration.destinationResolution;
    if (!source.x || !source.y || !source.z || uint64_t(source.x) * source.y > UINT32_MAX ||
        uint64_t(source.x) * source.y * source.z != values.size() || values.size() > UINT32_MAX / 4 ||
        m_configuration.logDimensions.empty() || m_configuration.logDimensions[0] < 1 ||
        m_configuration.logDimensions[0] > 10 || !std::isfinite(m_configuration.threshold) ||
        m_configuration.threshold < 0)
        throw std::invalid_argument("Resample source dimensions, threshold or leaf size are invalid");
    const uint32_t width = 1u << m_configuration.logDimensions[0];
    if (!destination.x || !destination.y || !destination.z || destination.x % width || destination.y % width ||
        destination.z % width || uint64_t(destination.x) * destination.y > UINT32_MAX ||
        uint64_t(destination.x) * destination.y * destination.z > UINT32_MAX / 4)
        throw std::invalid_argument(
            "Resample destination must contain complete leaves within portable addressing");
    if (!std::all_of(values.begin(), values.end(), [](float value) { return std::isfinite(value); }))
        throw std::invalid_argument("Resample source contains nonfinite values");
    m_coarseResolution = {destination.x / width, destination.y / width, destination.z / width, 0};
    m_source = values;
}
void ResampleVolume::rebuild(bool sparse, bool halo) {
    const auto resolution = m_coarseResolution, destination = m_configuration.destinationResolution;
    const uint32_t width = 1u << m_configuration.logDimensions[0];
    const size_t count = size_t(resolution.x) * resolution.y * resolution.z;
    std::vector<float> coarse;
    std::vector<uint8_t> active(count, sparse ? 0u : 1u);
    if (sparse) {
        coarse = queryResampleRegions(*m_runtime, m_source, m_configuration.sourceResolution, resolution,
                                      {float(destination.x), float(destination.y), float(destination.z), 0},
                                      m_configuration.transform);
        for (size_t i = 0; i < count; ++i)
            active[i] = coarse[i] > m_configuration.threshold ? 1u : 0u;
    }
    // A fresh owner keeps the previous visible publication intact on rebuild failure.
    m_runtime->flush();
    auto state = std::make_unique<State>();
    state->device = m_runtime->device();
    check(state->device->createCommandList({}, &state->commands), "Resample commands");
    auto remap = artifact(*m_runtime, "gvdb_owned", "GVDBAtlasRepack_relocateAtlas.bin");
    p::VolumeCreateDesc create;
    create.device = state->device;
    create.remapArtifact = remap.data();
    create.remapArtifactBytes = remap.size();
    if (m_configuration.logDimensions.size() > p::c_MaxLevels)
        throw std::invalid_argument("Too many resample levels");
    create.configuration.levelCount = uint32_t(m_configuration.logDimensions.size());
    std::copy(m_configuration.logDimensions.begin(), m_configuration.logDimensions.end(),
              create.configuration.logDimension.begin());
    check(p::createVolume(create, &state->owner), "Resample owner");
    check(state->owner->setChannel(0, {}), "Resample scalar channel");
    p::HostExtents extents;
    check(state->owner->computeExtents(
              1, {0, 0, 0, 0},
              {float(destination.x), float(destination.y), float(destination.z), 0}, extents),
          "Resample extents");
    if (extents.count != active.size() || extents.cellWidth != width)
        throw std::logic_error("Resample coarse grid mismatch");
    uint64_t selected = 0;
    check(state->owner->activateRegionFromMasks(extents, active.data(), active.size(), selected),
          "Resample activation");
    if (sparse && halo)
        check(state->owner->activateHalo(extents, selected), "Resample six-face halo");
    for (uint32_t z = 0; z < resolution.z; ++z)
        for (uint32_t y = 0; y < resolution.y; ++y)
            for (uint32_t x = 0; x < resolution.x; ++x)
                active[(size_t(z) * resolution.y + y) * resolution.x + x] =
                    state->owner->isActive({int32_t(x * width), int32_t(y * width), int32_t(z * width)}) ? 1u
                                                                                                         : 0u;
    check(state->owner->finishTopology(), "Resample finish topology");
    p::GenerationDesc generation;
    generation.placement.minimumBrickCount = {1, 1, 1};
    p::GenerationTicketHandle ticket;
    check(state->owner->prepareGeneration(generation, &ticket), "Resample initial generation");
    state->publish(ticket);
    auto binary = artifact(*m_runtime, "gvdb_resample", "GVDBAtlasResample_resampleOwnedAtlas.bin");
    p::ResampleCreateDesc resample;
    resample.volume = state->owner;
    resample.artifact = binary.data();
    resample.artifactBytes = binary.size();
    p::ResampleHandle op;
    check(p::createResample(resample, &op), "Resample operator");
    p::ResampleRequestDesc request;
    request.sourceData = m_source.data();
    request.sourceBytes = m_source.size() * sizeof(float);
    request.sourceResolution = {m_configuration.sourceResolution.x, m_configuration.sourceResolution.y,
                                m_configuration.sourceResolution.z};
    std::copy(m_configuration.transform.begin(), m_configuration.transform.end(), request.transformRows);
    request.preserveOutside = 0;
    check(op->prepare(request, {}, &ticket), "Resample prepare");
    state->publish(ticket);
    check(state->owner->getSnapshot(&state->visible), "Resample snapshot");
    m_state = std::move(state);
    m_active = std::move(active);
    m_coarseValues = std::move(coarse);
    m_sparse = sparse;
    m_halo = halo;
}
std::vector<float> loadRawBytes(const std::filesystem::path& path, UInt4 resolution) {
    const uint64_t count = uint64_t(resolution.x) * resolution.y * resolution.z;
    if (!resolution.x || !resolution.y || !resolution.z || uint64_t(resolution.x) * resolution.y > UINT32_MAX ||
        count > UINT32_MAX / 4)
        throw std::invalid_argument("RAW scalar dimensions exceed portable storage");
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        throw std::runtime_error("Cannot open RAW volume: " + path.string());
    if (file.tellg() != std::streamoff(count))
        throw std::runtime_error("RAW volume byte count does not match its dimensions");
    file.seekg(0);
    std::vector<uint8_t> bytes(size_t(count), 0);
    file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(count));
    if (!file)
        throw std::runtime_error("Cannot read complete RAW volume");
    std::vector<float> values(bytes.size());
    for (size_t i = 0; i < bytes.size(); ++i)
        values[i] = float(bytes[i]) / 256.f;
    return values;
}
std::vector<Float4> resampleTransferFunction() {
    struct Stop {
        float position;
        Float4 value;
    };
    const Stop stops[] = {
        {0,    {0, 0, 0, 0}         },
        {.15f, {0, 0, 0, 0}         },
        {.25f, {0, 0, 1, .01f}      },
        {.5f,  {1, 0, 0, .02f}      },
        {.75f, {.2f, .2f, .2f, .02f}},
        {1,    {.1f, .1f, .1f, .1f} }
    };
    std::vector<Float4> values(256);
    size_t segment = 0;
    for (size_t i = 0; i < values.size(); ++i) {
        const float x = float(i) / 255;
        while (segment + 2 < std::size(stops) && x > stops[segment + 1].position)
            ++segment;
        const auto& a = stops[segment];
        const auto& b = stops[segment + 1];
        const float t = (x - a.position) / (b.position - a.position);
        values[i] = {a.value.x + (b.value.x - a.value.x) * t, a.value.y + (b.value.y - a.value.y) * t,
                     a.value.z + (b.value.z - a.value.z) * t, a.value.w + (b.value.w - a.value.w) * t};
    }
    return values;
}
}  // namespace gvdb::samples
