// SPDX-License-Identifier: Apache-2.0
// Public-interface fixture: never includes concrete HostVolume/NvrhiVolume headers.
#include "GVDBNvrhiReductionApi.h"
#include "GVDBNvrhiSerializer.h"
#include "GVDBNvrhiVoxelApi.h"
#include "slang/GVDBPortableVbx.h"
#include "../../tests/PortableFixtures.h"
#include "GVDBNvrhiForeignVolume.h"
#include <donut/app/ApplicationBase.h>
#include <donut/app/DeviceManager.h>
#include <donut/core/log.h>
#include <nvrhi/core/foundation.h>
#include <nvrhi/core/datablob.h>
#if ETHEREAL_TEST_CUDA
#include <nvrhi/cuda.h>
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>
#include <set>
#include <cmath>
#include <cfloat>
using namespace nvrhi;
namespace g = gvdb::portable;
namespace {
std::atomic_uint problems{0};
void require(bool condition, const char* text) {
    if (!condition)
        throw std::runtime_error(text);
}
void check(FRESULT status, const char* text) {
    if (NVRHI_FAILED(status))
        throw std::runtime_error(std::string(text) + ": " + std::to_string(status));
}
struct Messages : ObjectImpl<IMessageCallback> {
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Messages)
    NVRHI_IMPLEMENTS_INTERFACE(IMessageCallback)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    void message(MessageSeverity severity, const char* text) noexcept override {
        if (severity >= MessageSeverity::Warning)
            ++problems;
        std::fprintf(stderr, "%s\n", text);
    }
};
struct Device {
    AutoPtr<donut::app::DeviceManager> manager;
    DeviceHandle device;
    AutoPtr<IMessageCallback> callback;
    Device(int argc, const char** argv) {
        bool useCuda = false, validation = false;
        for (int i = 1; i < argc; ++i) {
            useCuda |= !std::strcmp(argv[i], "-cuda");
            validation |= !std::strcmp(argv[i], "--validation");
        }
        if (useCuda) {
#if ETHEREAL_TEST_CUDA
            cuda::DeviceDesc desc;
            desc.enableOptiX = false;
            callback = MAKE_RC_OBJ_PTR(Messages);
            desc.errorCB = callback;
            device = cuda::createDevice(desc);
#else
            throw std::runtime_error("CUDA backend unavailable");
#endif
        } else {
            manager = TakeOver(
                donut::app::DeviceManager::Create(donut::app::GetGraphicsAPIFromCommandLine(argc, argv)));
            require(bool(manager), "create graphics manager");
            donut::app::DeviceCreationParameters desc;
            desc.enableNvrhiValidationLayer = true;
            desc.enableDebugRuntime = validation;
            require(manager->CreateHeadlessDevice(desc), "create graphics device");
            device = manager->GetDevice();
        }
        require(bool(device), "create test device");
    }
    ~Device() {
        if (device)
            device->waitForIdle();
        device = nullptr;
        if (manager)
            manager->Shutdown();
    }
};
template <class T>
void queryIdentity(T* object) {
    const auto count = object->AddRef();
    object->Release();
    AutoPtr<T> same;
    check(object->QueryInterface(uuid_of<T>(), reinterpret_cast<void**>(same.GetAddressOf())),
          "self QueryInterface");
    require(same.Get() == object, "self identity");
    same = nullptr;
    AutoPtr<IObject> identity, again;
    check(object->QueryInterface(uuid_of<IObject>(), reinterpret_cast<void**>(identity.GetAddressOf())),
          "base identity");
    check(identity->QueryInterface(uuid_of<IObject>(), reinterpret_cast<void**>(again.GetAddressOf())),
          "repeat base identity");
    require(identity == again, "canonical IObject identity");
    identity = nullptr;
    again = nullptr;
    AutoPtr<IRHIObject> resource;
    check(object->QueryInterface(uuid_of<IRHIObject>(), reinterpret_cast<void**>(resource.GetAddressOf())),
          "RHI ancestor");
    resource = nullptr;
    void* missing = reinterpret_cast<void*>(uintptr_t(1));
    require(object->QueryInterface(uuid_of<IDevice>(), &missing) == FE_NOINTERFACE && !missing,
            "unsupported IID clears output");
    require(object->QueryInterface(uuid_of<T>(), nullptr) == FS_OK, "query-only IID probe");
    require(object->AddRef() == count, "queries preserve reference balance");
    object->Release();
}

struct Harness {
    IDevice* device;
    CommandListHandle commands;
    explicit Harness(IDevice* d)
        : device(d) {
        check(device->createCommandList({}, &commands), "create command list");
    }
    g::RecordingContext context() const { return {device, commands}; }
    void begin() { commands->open(); }
    void submit() {
        commands->close();
        const auto serial = device->executeCommandList(commands);
        require(serial || device->getGraphicsAPI() == GraphicsAPI::D3D11, "submit commands");
        require(device->waitForIdle(), "caller GPU completion");
        device->runGarbageCollection();
    }
    std::vector<uint8_t> read(IBuffer* buffer, uint64_t size) {
        if (!size)
            return {};
        BufferHandle staging;
        check(device->createBuffer(BufferDesc()
                                       .setByteSize(size)
                                       .setCpuAccess(CpuAccessMode::Read)
                                       .setInitialState(ResourceStates::CopyDest)
                                       .setKeepInitialState(true),
                                   &staging),
              "create test readback");
        begin();
        commands->copyBuffer(staging, 0, buffer, 0, size);
        submit();
        const void* data = device->mapBuffer(staging, CpuAccessMode::Read);
        require(data, "map canonical GPU stream");
        std::vector<uint8_t> result(size_t(size), uint8_t(0));
        std::memcpy(result.data(), data, result.size());
        device->unmapBuffer(staging);
        return result;
    }
};

namespace s = gvdb::slang;
g::VolumeHandle makeVolume(IDevice* device, const std::vector<char>& remap) {
    g::VolumeCreateDesc desc;
    desc.device = device;
    desc.configuration.levelCount = 3;
    desc.configuration.logDimension = {1, 1, 1};
    desc.remapArtifact = remap.data();
    desc.remapArtifactBytes = remap.size();
    g::VolumeHandle result;
    check(g::createVolume(desc, &result), "create reduction volume");
    return result;
}
g::SnapshotHandle snapshot(g::IGVDBVolume* volume) {
    g::SnapshotHandle result;
    check(volume->getSnapshot(&result), "get reduction snapshot");
    return result;
}
g::ReductionHandle reduction(g::IGVDBVolume* volume, const std::vector<char>& artifact) {
    g::ReductionCreateDesc desc;
    desc.volume = volume;
    desc.artifact = artifact.data();
    desc.artifactBytes = artifact.size();
    g::ReductionHandle result;
    check(g::createReduction(desc, &result), "create public reduction");
    return result;
}
void publish(Harness& h, g::IGVDBVolume* volume, g::IGVDBGenerationTicket* ticket) {
    h.begin();
    check(volume->recordAndPublish(h.context(), ticket), "publish reduction fixture generation");
    h.submit();
}
s::VbxRawVolume importedFixture() {
    auto source = s::fixtures::twoBrickVolume();
    source.volume.atlasResolutionAndApron = {12, 12, 18, 2};
    source.volume.atlasBrickCountAndWidth = {2, 2, 3, 6};
    source.volume.boundsMin = {-2, 0, 0, 0};
    source.volume.boundsMax = {2, 2, 2, 0};
    source.nodes[1] = uint32_t(-2);
    source.nodes[17] = 0;
    source.nodes[33] = uint32_t(-2);
    source.nodes[36] = source.nodes[37] = source.nodes[38] = UINT32_MAX;
    source.nodes[4] = 2;
    source.nodes[5] = 2;
    source.nodes[6] = 14;  // Leaf0 in slot8.
    source.nodes[20] = 2;
    source.nodes[21] = 2;
    source.nodes[22] = 2;  // Leaf1 in slot0.
    source.atlasMap.assign(12, s::AtlasNode{-1, -1, -1, -1});
    source.atlasMap[8] = {-2, 0, 0, 0};
    source.atlasMap[0] = {0, 0, 0, 1};
    s::VbxRawVolume result;
    std::string error;
    require(s::packVolume(s::fixtures::volumeSource(source), result.packed, error), error.c_str());
    const s::AtlasFormat formats[] = {s::AtlasFormat::R32_FLOAT, s::AtlasFormat::R8_UINT,
                                      s::AtlasFormat::RGBA8_UINT, s::AtlasFormat::RGB32_FLOAT,
                                      s::AtlasFormat::RGBA32_FLOAT};
    for (auto format : formats) {
        s::VbxChannel channel;
        channel.format = format;
        channel.bytes.resize(12 * 12 * 18 * s::atlasStride(format), 0xe7);
        result.channels.push_back(std::move(channel));
    }
    auto& bytes = result.channels[0].bytes;
    for (size_t i = 0; i < bytes.size() / 4; ++i) {
        const uint32_t bits = i % 2 ? 0x7f800000u : 0x7fc12345u;
        std::memcpy(bytes.data() + i * 4, &bits, 4);
    }
    // These are four independent legacy column sequences in physical z order.
    // Float additions yield 3,26,4,.9375, rather than exact-real 4,26,4,.9375.
    const float columns[4][4] = {
        {16777216.f, 1.f,  -16777216.f, 3.f   },
        {5,          6,    7,           8     },
        {-2,         4,    -6,          8     },
        {.5f,        .25f, .125f,       .0625f}
    };
    for (uint32_t y = 0; y < 2; ++y)
        for (uint32_t x = 0; x < 2; ++x)
            for (uint32_t element = 0; element < 4; ++element) {
                const uint32_t z = (element < 2 ? 2 : 14) + (element % 2);
                const uint32_t address = ((z * 12 + y + 2) * 12 + x + 2) * 4;
                std::memcpy(bytes.data() + address, &columns[y * 2 + x][element], 4);
            }
    return result;
}
void import(Harness& h, g::IGVDBVolume* volume, const s::VbxRawVolume& raw) {
    std::vector<uint8_t> bytes;
    std::string error;
    require(s::saveRawVBX(raw, bytes, error), error.c_str());
    AutoPtr<IDataBlob> blob;
    check(CreateBlob(bytes.size(), &blob), "create VBX input blob");
    std::memcpy(blob->GetDataPtr(), bytes.data(), bytes.size());
    g::SerializerCreateDesc desc;
    desc.volume = volume;
    g::SerializerHandle serializer;
    check(g::createSerializer(desc, &serializer), "create public fixture importer");
    g::GenerationTicketHandle ticket;
    check(serializer->prepareLoadVBX(blob, {}, &ticket), "prepare exact non-dense import");
    publish(h, volume, ticket);
}
g::ReductionTicketHandle prepare(g::IGVDBReduction* operation, g::IGVDBSnapshot* source, uint32_t channel = 0,
                                 g::ReductionPreparationBudget budget = {}) {
    g::ReductionTicketHandle result;
    check(operation->prepare(source, channel, budget, &result), "prepare public reduction");
    return result;
}
void expected(g::IGVDBReductionTicket* ticket, float sum, uint64_t count) {
    g::ReductionValue result{123, 17, 999};
    check(ticket->finishAfterCallerCompletion(result), "finish reduction after caller completion");
    require(result.voxelCount == count && result.reserved == 0,
            "reduction active voxel count or reserved output");
    require(std::isnan(sum) ? std::isnan(result.sum) : result.sum == sum,
            "reduction sum differs from independent column oracle");
    const g::ReductionValue sentinel{43, 19, 83};
    result = sentinel;
    require(NVRHI_FAILED(ticket->finishAfterCallerCompletion(result)) &&
                !std::memcmp(&result, &sentinel, sizeof(result)),
            "repeated finish must reject and preserve value output");
}
class ForeignReductionTicket final : public ObjectImpl<g::IGVDBReductionTicket> {
    IDevice* m_device;

 public:
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(ForeignReductionTicket)
    NVRHI_IMPLEMENTS_INTERFACE(g::IGVDBReductionTicket)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    explicit ForeignReductionTicket(IDevice* device)
        : m_device(device) {}
    IDevice* getDevice() const noexcept override { return m_device; }
    FRESULT finishAfterCallerCompletion(g::ReductionValue&) noexcept override { return FE_UNSUPPORTED; }
};
void run(IDevice* device, const std::vector<char>& remap, const std::vector<char>& artifact,
         const std::vector<char>& mutation) {
    Harness h(device);
    auto owner = makeVolume(device, remap);
    import(h, owner, importedFixture());
    auto original = snapshot(owner);
    auto operation = reduction(owner, artifact);
    queryIdentity(operation.Get());
    require(operation->getVolume() == owner.Get(), "reduction retains exact public volume");
    require(
        original->getDesc().activeLeafCount == 2 && original->getDesc().volume.atlasBrickCountAndWidth.z == 3,
        "fixture needs two active non-dense slots across three z layers");
    g::ChannelSnapshotDesc originalChannel;
    check(original->getChannel(0, originalChannel), "inspect immutable input channel");
    const auto originalRevision = original->getDesc().revision;
    auto ticket = prepare(operation, original);
    queryIdentity(ticket.Get());
    g::ReductionValue sentinel{31, 7, 99}, value = sentinel;
    require(NVRHI_FAILED(ticket->finishAfterCallerCompletion(value)) &&
                !std::memcmp(&value, &sentinel, sizeof(value)),
            "reduction finish before record accepted or changed output");
    auto rejects = [&](g::IGVDBSnapshot* snap, uint32_t channel, g::ReductionPreparationBudget budget = {}) {
        auto* out = reinterpret_cast<g::IGVDBReductionTicket*>(uintptr_t(1));
        require(NVRHI_FAILED(operation->prepare(snap, channel, budget, &out)) && !out,
                "invalid reduction preparation accepted");
        const auto after = snapshot(owner);
        g::ChannelSnapshotDesc channelAfter;
        check(after->getChannel(0, channelAfter), "failure leaves channel");
        require(after->getDesc().revision.epoch == originalRevision.epoch &&
                    after->getDesc().revision.topology == originalRevision.topology &&
                    after->getDesc().revision.placement == originalRevision.placement &&
                    channelAfter.raw.buffer == originalChannel.raw.buffer &&
                    channelAfter.revision.content == originalChannel.revision.content,
                "read-only reduction preparation mutated owner");
    };
    rejects(nullptr, 0);
    rejects(original, 32);
    rejects(original, 30);
    for (uint32_t channel = 1; channel < 5; ++channel)
        rejects(original, channel);
    g::ReductionPreparationBudget budget;
    budget.reserved = 1;
    rejects(original, 0, budget);
    budget = {};
    budget.maxBuffers = 2;
    rejects(original, 0, budget);
    budget = {};
    budget.maxAllocatedBytes = 383;
    rejects(original, 0, budget);
    budget.maxAllocatedBytes = 384;
    auto exactBudget = prepare(operation, original, 0, budget);
    auto foreignOwner = makeVolume(device, remap);
    check(foreignOwner->setChannel(0, {g::AtlasFormat::R32_FLOAT, {}}), "foreign empty channel");
    g::GenerationDesc generation;
    generation.placement.minimumBrickCount = {1, 1, 1};
    g::GenerationTicketHandle generationTicket;
    check(foreignOwner->prepareGeneration(generation, &generationTicket), "foreign publication");
    publish(h, foreignOwner, generationTicket);
    auto foreignSnapshot = snapshot(foreignOwner);
    rejects(foreignSnapshot, 0);
    require(operation->prepare(original, 0, {}, nullptr) == FE_INVALID_ARGS, "null reduction output");
    auto second = reduction(owner, artifact);
    auto foreignTicket = MAKE_RC_OBJ_PTR(ForeignReductionTicket, device);
    h.begin();
    require(NVRHI_FAILED(second->record(h.context(), ticket)), "another operator accepted reduction ticket");
    require(NVRHI_FAILED(operation->record(h.context(), foreignTicket)),
            "foreign ticket implementation accepted");
    require(NVRHI_FAILED(operation->record({nullptr, h.commands}, ticket)),
            "invalid reduction device accepted");
    require(NVRHI_FAILED(operation->record({device, nullptr}, ticket)), "null reduction list accepted");
    check(operation->record(h.context(), ticket), "record exact imported reduction");
    require(NVRHI_FAILED(operation->record(h.context(), ticket)), "reduction ticket recorded twice");
    check(operation->record(h.context(), exactBudget), "record exact memory budget reduction");
    h.submit();
    expected(ticket, 33.9375f, 16);
    expected(exactBudget, 33.9375f, 16);
    // Repacking changes the legacy addition order. The old snapshot and query
    // retain the original z order; the new column sequence is [-2^24,3,2^24,1].
    auto retained = prepare(operation, original);
    generation.placement.minimumBrickCount = {1, 1, 2};
    generation.placement.apron = 2;
    check(owner->prepareGeneration(generation, &generationTicket), "repack original active interiors");
    publish(h, owner, generationTicket);
    auto repacked = snapshot(owner);
    auto changedOrder = prepare(operation, repacked);
    h.begin();
    check(operation->record(h.context(), retained), "record old snapshot after repack");
    check(operation->record(h.context(), changedOrder), "record repacked column order");
    h.submit();
    expected(retained, 33.9375f, 16);
    expected(changedOrder, 34.9375f, 16);
    // Channel31 exercises the high bit and count is independent of stored values.
    check(owner->setChannel(31,
                            {
                                g::AtlasFormat::R32_FLOAT, {2, 0, 0, 0}
    }),
          "configure channel31");
    check(owner->prepareGeneration(generation, &generationTicket), "publish channel31");
    publish(h, owner, generationTicket);
    auto high = snapshot(owner);
    auto highTicket = prepare(operation, high, 31);
    h.begin();
    check(operation->record(h.context(), highTicket), "record channel31 reduction");
    h.submit();
    expected(highTicket, 32, 16);
    g::VoxelOpsCreateDesc voxelDesc;
    voxelDesc.volume = owner;
    voxelDesc.artifact = mutation.data();
    voxelDesc.artifactBytes = mutation.size();
    g::VoxelOpsHandle voxel;
    check(g::createVoxelOps(voxelDesc, &voxel), "create fill operator");
    auto beforeFill = prepare(operation, repacked);
    check(voxel->prepareFill(0, {2, 0, 0, 0}, {}, &generationTicket), "prepare fill after reduction snapshot");
    publish(h, owner, generationTicket);
    auto filled = snapshot(owner);
    auto afterFill = prepare(operation, filled);
    h.begin();
    check(operation->record(h.context(), beforeFill), "record preserved pre-fill source");
    check(operation->record(h.context(), afterFill), "record new filled allocation");
    h.submit();
    expected(beforeFill, 34.9375f, 16);
    expected(afterFill, 32, 16);
    // Active IEEE nonfinites propagate; the poisoned inactive/apron values in the
    // original import above were excluded completely, despite containing both.
    const float inf = std::numeric_limits<float>::infinity(), nan = std::numeric_limits<float>::quiet_NaN();
    const float cases[][3] = {
        {nan,  0,    nan },
        {inf,  0,    inf },
        {-inf, 0,    -inf},
        {inf,  -inf, nan }
    };
    std::vector<g::ReductionTicketHandle> ieee;
    std::vector<g::SnapshotHandle> ieeeSources;
    for (const auto& values : cases) {
        std::vector<float> atlas(6 * 6 * 12, 0);
        atlas[(2 * 6 + 2) * 6 + 2] = values[0];
        atlas[(8 * 6 + 2) * 6 + 2] = values[1];
        g::ChannelUploadDesc upload{0, 0, atlas.data(), atlas.size() * 4};
        generation.uploads = &upload;
        generation.uploadCount = 1;
        check(owner->prepareGeneration(generation, &generationTicket), "prepare active IEEE upload");
        publish(h, owner, generationTicket);
        ieeeSources.push_back(snapshot(owner));
        ieee.push_back(prepare(operation, ieeeSources.back()));
    }
    generation.uploads = nullptr;
    generation.uploadCount = 0;
    auto oldAfterMutations = prepare(operation, original);
    check(owner->clearTopology(), "clear topology before empty reduction");
    check(owner->finishTopology(), "finish empty topology");
    check(owner->prepareGeneration(generation, &generationTicket), "publish empty retaining channel mask");
    publish(h, owner, generationTicket);
    auto empty = snapshot(owner);
    budget = {};
    budget.maxAllocatedBytes = 0;
    budget.maxBuffers = 0;
    auto emptyTicket = prepare(operation, empty, 31, budget);
    h.begin();
    for (auto& item : ieee)
        check(operation->record(h.context(), item), "record active IEEE reduction");
    check(operation->record(h.context(), oldAfterMutations), "record old snapshot after later clears");
    check(operation->record(h.context(), emptyTicket), "record empty reduction without buffers");
    operation = nullptr;
    second = nullptr;
    voxel = nullptr;
    owner = nullptr;
    foreignOwner = nullptr;
    generationTicket = nullptr;
    h.submit();
    for (uint32_t i = 0; i < ieee.size(); ++i)
        expected(ieee[i], cases[i][2], 16);
    expected(oldAfterMutations, 33.9375f, 16);
    expected(emptyTicket, 0, 0);
    std::puts(
        "PASS public reduction: non-dense poisoned import, independent column arithmetic/count, "
        "repack/fill/old snapshots, IEEE values, channel31, empty and lifetimes");
}
std::vector<char> readArtifact(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(bool(file), "open reduction artifact");
    return std::vector<char>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}
}  // namespace
int main(int argc, const char** argv) {
    donut::log::ConsoleApplicationMode();
    donut::log::SetCallback([](donut::log::Severity severity, const char* text) {
        if (severity >= donut::log::Severity::Warning)
            ++problems;
        std::fprintf(stderr, "%s\n", text);
    });
    try {
        {
            Device device(argc, argv);
            const auto type = donut::app::GetShaderTypeName(device.device->getGraphicsAPI());
            const auto base = donut::app::GetDirectoryWithExecutable() / "shaders";
            auto remap = base / "gvdb_owner" / type / "GVDBAtlasRepack_relocateAtlas.bin";
            auto reductionPath = base / "gvdb_reduction" / type / "GVDBAtlasReduction_reduceActiveColumns.bin";
            auto mutation = base / "gvdb_voxel" / type / "GVDBAtlasMutation_mutateAtlas.bin";
            for (int i = 1; i + 1 < argc; ++i) {
                if (!std::strcmp(argv[i], "--owner-artifact"))
                    remap = argv[i + 1];
                if (!std::strcmp(argv[i], "--reduction-artifact"))
                    reductionPath = argv[i + 1];
                if (!std::strcmp(argv[i], "--mutation-artifact"))
                    mutation = argv[i + 1];
            }
            const auto ownerBytes = readArtifact(remap), reductionBytes = readArtifact(reductionPath),
                       mutationBytes = readArtifact(mutation);
            run(device.device, ownerBytes, reductionBytes, mutationBytes);
        }
        require(problems == 0, "validation warnings/errors");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
