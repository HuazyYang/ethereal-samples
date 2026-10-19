// SPDX-License-Identifier: Apache-2.0
#include "PrintVolume.h"
#include "ComputeRuntime.h"
#include <gvdb/GVDBNvrhiResampleApi.h>
#include <gvdb/GVDBNvrhiVoxelizeApi.h>
#include <donut/app/ApplicationBase.h>
#include <cstring>
#include <fstream>
#include <iterator>
#include <nvrhi/core/autoptr.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>
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
        throw std::runtime_error("Missing print artifact: " + path.string());
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
        throw std::runtime_error("Print submission failed");
    if (wait) {
        if (!device->waitForIdle())
            throw std::runtime_error("Print completion failed");
        device->runGarbageCollection();
    }
}
}  // namespace
struct PrintVolume::State {
    nvrhi::DeviceHandle device;
    nvrhi::CommandListHandle commands;
    p::VolumeHandle owner;
    p::SnapshotHandle visible;
    void publish(p::IGVDBGenerationTicket* ticket) {
        submit(device, commands, [&](p::RecordingContext context) {
            check(owner->recordAndPublish(context, ticket), "Print publication");
        });
    }
};
PrintVolume::~PrintVolume() = default;
p::SnapshotHandle PrintVolume::snapshot() const { return m_state ? m_state->visible : p::SnapshotHandle{}; }
const p::IGVDBHostVolume& PrintVolume::topology() const {
    if (!m_state)
        throw std::logic_error("No print publication");
    return *m_state->owner;
}
std::vector<float> PrintVolume::readAtlas() {
    m_runtime->flush();
    p::ReadbackHandle read;
    check(m_state->owner->prepareReadback(m_state->visible, 1, &read), "Print prepare readback");
    submit(
        m_state->device, m_state->commands,
        [&](p::RecordingContext context) {
            check(m_state->owner->recordReadback(context, read), "Print record readback");
        },
        true);
    check(read->finishAfterCallerCompletion(), "Print finish readback");
    p::BorrowedBytes bytes;
    check(read->getChannelData(0, bytes), "Print channel bytes");
    std::vector<float> result(size_t(bytes.byteCount / 4));
    if (bytes.byteCount)
        std::memcpy(result.data(), bytes.data, size_t(bytes.byteCount));
    return result;
}
std::vector<AtlasNode> PrintVolume::readAtlasMap() {
    m_runtime->flush();
    p::BorrowedBufferBinding binding;
    check(m_state->visible->getTopologyBuffer(p::TopologyBuffer::AtlasMap, binding), "Print atlas map");
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
          "Print atlas map staging");
    submit(
        m_state->device, m_state->commands,
        [&](p::RecordingContext context) {
            context.commands->copyBuffer(staging, 0, binding.buffer, binding.byteOffset, binding.byteSize);
        },
        true);
    const void* data = m_state->device->mapBuffer(staging, nvrhi::CpuAccessMode::Read);
    if (!data)
        throw std::runtime_error("Print map failed");
    std::memcpy(result.data(), data, size_t(binding.byteSize));
    m_state->device->unmapBuffer(staging);
    return result;
}
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
        const double a = std::floor(double(lower[axis]) / 32) * 32,
                     b = std::ceil(double(upper[axis]) / 32) * 32;
        if (!std::isfinite(a) || !std::isfinite(b) || a < INT32_MIN || b > INT32_MAX || b <= a ||
            b - a > INT32_MAX)
            throw std::invalid_argument("Print mesh bounds exceed portable signed coordinates");
        low[axis] = int32_t(a);
        high[axis] = int32_t(b);
        resolution[axis] = uint32_t(b - a);
    }
    portable::VoxelizeRequestDesc p;
    p.boundsMinimum = {float(low[0]), float(low[1]), float(low[2]), 0};
    p.boundsMaximum = {float(high[0]), float(high[1]), float(high[2]), 0};
    p.resolution = {resolution[0], resolution[1], resolution[2]};
    p.values = {1, .5f, 0, 0};
    const auto matrix = dm::affineToHomogeneous(transform);
    for (int axis = 0; axis < 3; ++axis)
        p.transformRows[axis] = {matrix[0][axis], matrix[1][axis], matrix[2][axis], matrix[3][axis]};
    m_runtime->flush();
    auto state = std::make_unique<State>();
    state->device = m_runtime->device();
    check(state->device->createCommandList({}, &state->commands), "Print commands");
    std::array<std::vector<char>, 4> binaries;
    const char* names[] = {"GVDBMeshVoxelize_clearTriangleBins.bin", "GVDBMeshVoxelize_countTriangleBins.bin",
                           "GVDBMeshVoxelize_scatterTriangleBins.bin", "GVDBMeshVoxelize_solidVoxelize.bin"};
    portable::VoxelizeCreateDesc voxelCreate;
    voxelCreate.device = state->device;
    for (unsigned i = 0; i < 4; ++i) {
        binaries[i] = artifact(*m_runtime, "gvdb_voxelize", names[i]);
        voxelCreate.artifacts[i] = {binaries[i].data(), binaries[i].size()};
    }
    portable::VoxelizeHandle voxelizer;
    check(portable::createVoxelize(voxelCreate, &voxelizer), "Print voxelizer");
    p.vertices = m_vertices.data();
    p.vertexCount = m_vertices.size();
    p.indices = m_indices.data();
    p.indexCount = m_indices.size();
    portable::VoxelizeTicketHandle voxelTicket;
    check(voxelizer->prepare(p, {}, &voxelTicket), "Print prepare voxelization");
    submit(
        state->device, state->commands,
        [&](portable::RecordingContext context) {
            check(voxelizer->recordCounts(context, voxelTicket), "Print count triangles");
        },
        true);
    check(voxelTicket->finishCountsAfterCallerCompletion(), "Print size triangle bins");
    submit(
        state->device, state->commands,
        [&](portable::RecordingContext context) {
            check(voxelizer->recordVoxelization(context, voxelTicket), "Print classify voxels");
        },
        true);
    portable::BorrowedBytes classified;
    check(voxelTicket->finishAfterCallerCompletion(classified), "Print complete voxelization");
    std::vector<float> dense(size_t(classified.byteCount / 4));
    std::memcpy(dense.data(), classified.data, size_t(classified.byteCount));
    // Dense CPU classification selects occupied sparse leaves. Release the
    // operator's dense GPU buffers before allocating the sparse generation.
    voxelTicket = nullptr;
    voxelizer = nullptr;
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
    // CPU classification remains explicit; only public ownership is published.
    // Construct a fresh owner so any failed rebuild preserves the visible snapshot.

    auto remap = artifact(*m_runtime, "gvdb_owned", "GVDBAtlasRepack_relocateAtlas.bin");
    portable::VolumeCreateDesc create;
    create.device = state->device;
    create.remapArtifact = remap.data();
    create.remapArtifactBytes = remap.size();
    if (!portable::LevelConfig::fromBranching(3, 3, 3, 3, 5, create.configuration))
        throw std::logic_error("Print branching");
    check(portable::createVolume(create, &state->owner), "Print owner");
    check(state->owner->setChannel(0, {}), "Print scalar channel");
    portable::HostExtents extents;
    check(state->owner->computeExtents(1, p.boundsMinimum, p.boundsMaximum, extents), "Print extents");
    const UInt4 cells{p.resolution.x / 32, p.resolution.y / 32, p.resolution.z / 32, 0};
    if (extents.cellWidth != 32 || extents.count != uint64_t(cells.x) * cells.y * cells.z)
        throw std::logic_error("Print coarse grid");
    std::vector<uint8_t> active(size_t(extents.count), 0);
    for (uint32_t z = 0; z < p.resolution.z; ++z)
        for (uint32_t y = 0; y < p.resolution.y; ++y)
            for (uint32_t x = 0; x < p.resolution.x; ++x)
                if (dense[(size_t(z) * p.resolution.y + y) * p.resolution.x + x] != 0)
                    active[(size_t(z / 32) * cells.y + y / 32) * cells.x + x / 32] = 1;
    uint64_t selected = 0;
    check(state->owner->activateRegionFromMasks(extents, active.data(), active.size(), selected),
          "Print activation");
    check(state->owner->finishTopology(), "Print finish topology");
    portable::GenerationDesc generation;
    generation.placement.minimumBrickCount = {1, 1, 1};
    portable::GenerationTicketHandle ticket;
    check(state->owner->prepareGeneration(generation, &ticket), "Print initial generation");
    state->publish(ticket);
    auto binary = artifact(*m_runtime, "gvdb_resample", "GVDBAtlasResample_resampleOwnedAtlas.bin");
    portable::ResampleCreateDesc resample;
    resample.volume = state->owner;
    resample.artifact = binary.data();
    resample.artifactBytes = binary.size();
    portable::ResampleHandle op;
    check(portable::createResample(resample, &op), "Print resample operator");
    portable::ResampleRequestDesc request;
    request.sourceData = dense.data();
    request.sourceBytes = dense.size() * sizeof(float);
    request.sourceResolution = {p.resolution.x, p.resolution.y, p.resolution.z};
    request.preserveOutside = 0;
    request.transformRows[0].w = -float(low[0]);
    request.transformRows[1].w = -float(low[1]);
    request.transformRows[2].w = -float(low[2]);
    check(op->prepare(request, {}, &ticket), "Print dense upload");
    state->publish(ticket);
    check(state->owner->getSnapshot(&state->visible), "Print snapshot");
    const Int4 origin{low[0], low[1], low[2], 0};
    m_configuration.voxelSize = voxelSize;
    m_state = std::move(state);
    m_dense = std::move(dense);
    m_resolution = {p.resolution.x, p.resolution.y, p.resolution.z, 0};
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
