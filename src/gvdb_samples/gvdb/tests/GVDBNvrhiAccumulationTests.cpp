// SPDX-License-Identifier: Apache-2.0
// Test-only device harness. The public owner has no Donut or native SDK includes.
#include "GVDBNvrhiVolume.h"
#include "GVDBNvrhiAdoptionInternal.h"
#include <limits>
#include <cmath>
#include <donut/app/ApplicationBase.h>
#include <donut/app/DeviceManager.h>
#include <donut/core/log.h>
#include <nvrhi/core/foundation.h>
#if ETHEREAL_TEST_CUDA
#include <nvrhi/cuda.h>
#endif
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>
#include <tuple>
using namespace nvrhi;
namespace g = gvdb::portable;
namespace {
std::atomic_uint problems{0};
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
void check(FRESULT result, const char* message) {
    if (NVRHI_FAILED(result))
        throw std::runtime_error(std::string(message) + ": " + std::to_string(result));
}
#if ETHEREAL_TEST_CUDA
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
#endif
struct Device {
    AutoPtr<donut::app::DeviceManager> manager;
    DeviceHandle device;
    AutoPtr<IMessageCallback> callback;
    Device(int argc, const char** argv) {
        bool cuda = false, validation = false;
        for (int i = 1; i < argc; ++i) {
            cuda |= std::strcmp(argv[i], "-cuda") == 0;
            validation |= std::strcmp(argv[i], "--validation") == 0;
        }
        if (cuda) {
#if ETHEREAL_TEST_CUDA
            cuda::DeviceDesc desc;
            desc.enableOptiX = false;
            callback = MAKE_RC_OBJ_PTR(Messages);
            desc.errorCB = callback;
            device = cuda::createDevice(desc);
#else
            throw std::runtime_error("CUDA backend unavailable in this build");
#endif
        } else {
            manager = TakeOver(
                donut::app::DeviceManager::Create(donut::app::GetGraphicsAPIFromCommandLine(argc, argv)));
            require(bool(manager), "requested graphics backend unavailable");
            donut::app::DeviceCreationParameters parameters;
            parameters.enableNvrhiValidationLayer = true;
            parameters.enableCopyQueue = true;
            parameters.enableDebugRuntime = validation;
            require(manager->CreateHeadlessDevice(parameters), "create graphics device");
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
struct Harness {
    IDevice* device;
    CommandListHandle commands;
    explicit Harness(IDevice* value)
        : device(value) {
        check(device->createCommandList({}, &commands), "create command list");
    }
    g::RecordingContext context() const { return {device, commands}; }
    void begin() { commands->open(); }
    void submit() {
        commands->close();
        const auto id = device->executeCommandList(commands);
        require(id || device->getGraphicsAPI() == GraphicsAPI::D3D11, "submit");
        require(device->waitForIdle(), "GPU completion");
        device->runGarbageCollection();
    }
    std::vector<uint8_t> read(const g::BufferView& view) {
        if (!view.byteSize)
            return {};
        BufferHandle staging;
        check(device->createBuffer(BufferDesc()
                                       .setByteSize(view.byteSize)
                                       .setCpuAccess(CpuAccessMode::Read)
                                       .setInitialState(ResourceStates::CopyDest)
                                       .setKeepInitialState(true),
                                   &staging),
              "staging");
        begin();
        commands->copyBuffer(staging, 0, view.buffer, view.byteOffset, view.byteSize);
        submit();
        const void* data = device->mapBuffer(staging, CpuAccessMode::Read);
        require(data != nullptr, "map staging");
        std::vector<uint8_t> result(size_t(view.byteSize));
        std::memcpy(result.data(), data, result.size());
        device->unmapBuffer(staging);
        return result;
    }
};

namespace detail = g::detail;
using Channels = std::array<std::vector<uint8_t>, 5>;
constexpr uint32_t strides[] = {1, 4, 4, 12, 16};
const g::ChannelDesc descriptors[] = {
    {g::AtlasFormat::R8_UINT,      {23, 0, 0, 0}              },
    {g::AtlasFormat::RGBA8_UINT,   {153, 51, 204, 117}        },
    {g::AtlasFormat::R32_FLOAT,    {-3.5f, 0, 0, 0}           },
    {g::AtlasFormat::RGB32_FLOAT,  {-1.25f, 2.5f, 7.75f, 0}   },
    {g::AtlasFormat::RGBA32_FLOAT, {3.25f, -4.5f, 1.75f, 9.5f}}
};
struct Tag final : ObjectImpl<IRHIObject> {
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Tag)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
};
g::LevelConfig configuration() {
    g::LevelConfig config;
    config.levelCount = 5;
    config.logDimension = {1, 1, 1, 1, 1};
    return config;
}
g::NodeId id(uint32_t group, uint32_t level, uint64_t index) {
    g::NodeId result;
    require(g::NodeId::encode(group, level, index, result), "fixture ID encoding");
    return result;
}
std::unique_ptr<g::NvrhiVolume> createOwner(IDevice* device, const std::vector<char>& artifact) {
    g::VolumeOwnerDesc desc;
    desc.device = device;
    desc.configuration = configuration();
    desc.remapArtifact = artifact.data();
    desc.remapArtifactBytes = artifact.size();
    std::unique_ptr<g::NvrhiVolume> result;
    check(g::NvrhiVolume::create(desc, result), "create accumulation owner");
    return result;
}
// The CPU oracle stores interior values by world voxel coordinate. It does not
// read owner placement maps, packed nodes, or the shader's spatial lookup.
using Coordinate = std::array<int32_t, 3>;
using Value = std::array<uint8_t, 16>;
using Field = std::array<std::map<Coordinate, Value>, 5>;
Value outside(uint32_t c) {
    Value result{};
    if (c == 0)
        result[0] = 23;
    else if (c == 1) {
        result[0] = 153;
        result[1] = 51;
        result[2] = 204;
        result[3] = 117;
    } else {
        const float scalar[] = {-3.5f};
        const float rgb[] = {-1.25f, 2.5f, 7.75f};
        const float rgba[] = {3.25f, -4.5f, 1.75f, 9.5f};
        std::memcpy(result.data(), c == 2 ? scalar : c == 3 ? rgb : rgba, strides[c]);
    }
    return result;
}
uint64_t voxel(g::UInt3 grid, uint32_t pitch, uint32_t slot, uint32_t x, uint32_t y, uint32_t z) {
    const uint64_t ax = (slot % grid.x) * pitch + x;
    const uint64_t ay = ((slot / grid.x) % grid.y) * pitch + y;
    const uint64_t az = (slot / (grid.x * grid.y)) * pitch + z;
    return (az * grid.y * pitch + ay) * grid.x * pitch + ax;
}
Channels importedBytes(Field& field) {
    Channels bytes;
    const uint64_t count = 18 * 12 * 6;
    for (uint32_t c = 0; c < 5; ++c) {
        bytes[c].resize(size_t(count * strides[c]));
        for (size_t b = 0; b < bytes[c].size(); ++b)
            bytes[c][b] = uint8_t(31 * b + 7 * (b >> 4) + 19 * c + 73);
        for (uint32_t z = 0; z < 2; ++z)
            for (uint32_t y = 0; y < 2; ++y)
                for (uint32_t x = 0; x < 2; ++x) {
                    Value value{};
                    const uint32_t ordinal = x + 2 * y + 4 * z;
                    if (c < 2)
                        for (uint32_t k = 0; k < strides[c]; ++k)
                            value[k] = uint8_t(31 + 7 * ordinal + k * 41 + c * 13);
                    else
                        for (uint32_t k = 0; k < strides[c] / 4; ++k) {
                            // Preserve payload bits, signed zero, and denormals exactly.
                            const uint32_t bits = ordinal % 3 == 0   ? 0x7fc12345u + k
                                                  : ordinal % 3 == 1 ? 0x80000000u
                                                                     : 1u + ordinal + k;
                            std::memcpy(value.data() + k * 4, &bits, 4);
                        }
                    field[c][{int32_t(x), int32_t(y), int32_t(z)}] = value;
                    std::memcpy(bytes[c].data() + voxel({3, 2, 1}, 6, 5, x + 2, y + 2, z + 2) * strides[c],
                                value.data(), strides[c]);
                }
    }
    return bytes;
}
Channels expectedAtlas(const std::vector<Coordinate>& origins, g::UInt3 grid, uint32_t apron,
                       const Field& field) {
    Channels result;
    if (origins.empty())
        return result;
    const uint32_t pitch = 2 + 2 * apron;
    for (uint32_t c = 0; c < 5; ++c) {
        result[c].resize(size_t(grid.x) * grid.y * grid.z * pitch * pitch * pitch * strides[c], 0);
        const Value absent = outside(c);
        for (uint32_t s = 0; s < origins.size(); ++s)
            for (uint32_t z = 0; z < pitch; ++z)
                for (uint32_t y = 0; y < pitch; ++y)
                    for (uint32_t x = 0; x < pitch; ++x) {
                        const Coordinate world{origins[s][0] + int32_t(x) - int32_t(apron),
                                               origins[s][1] + int32_t(y) - int32_t(apron),
                                               origins[s][2] + int32_t(z) - int32_t(apron)};
                        // All new active interiors and exterior aprons use the declared void.
                        auto found = field[c].find(world);
                        const Value& value = found == field[c].end() ? absent : found->second;
                        std::memcpy(result[c].data() + voxel(grid, pitch, s, x, y, z) * strides[c],
                                    value.data(), strides[c]);
                    }
    }
    return result;
}
struct HostState {
    g::NodeId root;
    g::TopologyRevision revision;
    std::array<std::vector<uint8_t>, 10> pools;
};
HostState capture(const g::HostVolume& host) {
    HostState state{host.root(), host.revision(), {}};
    for (uint32_t level = 0; level < 5; ++level)
        for (uint32_t group = 0; group < 2; ++group) {
            const auto view = host.pool(group, level);
            auto& bytes = state.pools[level * 2 + group];
            bytes.resize(size_t(view.count) * view.stride);
            if (!bytes.empty())
                std::memcpy(bytes.data(), view.data, bytes.size());
        }
    return state;
}
void unchanged(const g::HostVolume& host, const HostState& before) {
    const auto after = capture(host);
    require(after.root == before.root && after.revision.epoch == before.revision.epoch &&
                after.revision.topology == before.revision.topology && after.pools == before.pools,
            "failed append changed live host bytes/IDs/revision");
}
std::unique_ptr<detail::AccumulationCandidate> begin(g::NvrhiVolume& owner) {
    std::unique_ptr<detail::AccumulationCandidate> result;
    check(detail::VolumeAdoptionBuilder::beginAccumulation(owner, result), "begin authenticated accumulation");
    return result;
}
void activate(g::HostVolume& host, int32_t x, int32_t y = 0, int32_t z = 0) {
    g::NodeId added;
    check(host.activate(x, y, z, added), "append signed leaf");
    check(host.setActive(added, true), "activate allocated inactive leaf");
}
void publish(Harness& h, g::NvrhiVolume& owner, g::PreparedGeneration& ticket) {
    h.begin();
    check(owner.recordAndPublish(h.context(), ticket), "publish accumulation");
    h.submit();
}
std::unique_ptr<g::PreparedReadback> recordRead(Harness& h, g::NvrhiVolume& owner,
                                                std::shared_ptr<const g::VolumeGeneration> snapshot) {
    std::unique_ptr<g::PreparedReadback> result;
    check(owner.prepareReadback(snapshot, 31, result), "prepare retained readback");
    check(owner.recordReadback(h.context(), *result), "record retained readback");
    return result;
}
void verify(g::PreparedReadback& ticket, const Channels& expected, const char* label) {
    g::ChannelReadback actual;
    check(g::NvrhiVolume::finishReadbackAfterCompletion(ticket, actual), "finish retained readback");
    require(actual.channelMask == 31, "accumulation lost channel descriptors");
    for (uint32_t c = 0; c < 5; ++c)
        if (actual.channels[c] != expected[c]) {
            std::fprintf(stderr, "%s channel %u got %zu bytes expected %zu\n", label, c,
                         actual.channels[c].size(), expected[c].size());
            for (size_t b = 0; b < std::min(actual.channels[c].size(), expected[c].size()); ++b)
                if (actual.channels[c][b] != expected[c][b]) {
                    std::fprintf(stderr, "first mismatch byte %zu got %u expected %u\n", b,
                                 actual.channels[c][b], expected[c][b]);
                    break;
                }
            throw std::runtime_error(label);
        }
}
void seed(Harness& h, g::NvrhiVolume& owner, const Channels& bytes, IRHIObject* tag) {
    g::HostVolume host;
    check(host.configure(configuration()), "seed configure");
    activate(host, 0);
    activate(host, 2);
    check(host.setActive(host.findLeaf({2, 0, 0}), false), "seed inactive leaf");
    auto* leaf = host.editNode(host.findLeaf({0, 0, 0}));
    leaf->priority = 17;
    leaf->reserved = 29;
    leaf->range[0] = -1.5f;
    leaf->range[1] = 2.75f;
    leaf->range[2] = 4.25f;
    leaf->mask = 0x0123456789abcdefull;
    check(host.finishTopology(), "finish seed graph");
    detail::ImportedLeafSlot slot{0, 5};
    std::array<detail::ImportedChannel, 5> channels;
    for (uint32_t c = 0; c < 5; ++c)
        channels[c] = {c, descriptors[c], bytes[c].data(), bytes[c].size()};
    detail::VolumeImportRequest request;
    request.candidate = &host;
    request.placement = {
        {3, 2, 1},
        2, &slot, 1
    };
    request.channels = channels.data();
    request.channelCount = 5;
    request.attachment = tag;
    std::unique_ptr<g::PreparedGeneration> ticket;
    check(detail::VolumeAdoptionBuilder::prepareImport(owner, request, ticket),
          "prepare initial poison import");
    publish(h, owner, *ticket);
}
void run(IDevice* device, const std::vector<char>& artifact) {
    Harness h(device);
    Field field;
    const Channels original = importedBytes(field);
    auto owner = createOwner(device, artifact);
    auto tag = MAKE_RC_OBJ_PTR(Tag);
    seed(h, *owner, original, tag);
    auto* hostAddress = &owner->host();
    const auto initial = owner->snapshot();
    const auto before = capture(owner->host());
    const auto oldRoot = owner->host().root();
    const auto oldLeaf = owner->host().findLeaf({0, 0, 0});
    const auto inactiveLeaf = owner->host().findLeaf({2, 0, 0});
    const g::Node oldRecord = *owner->host().node(oldLeaf);
    require(oldLeaf == id(0, 0, 0) && inactiveLeaf == id(0, 0, 1) && oldRoot == id(0, 1, 0),
            "independent initial pool IDs");
    auto candidate = begin(*owner);
    activate(candidate->host(), 2);  // Reactivation without replacing the allocated ID.
    activate(candidate->host(), -2);
    activate(candidate->host(), 10);
    check(candidate->host().finishTopology(), "finish signed root growth");
    require(candidate->host().findLeaf({-2, 0, 0}) == id(0, 0, 2) &&
                candidate->host().findLeaf({10, 0, 0}) == id(0, 0, 3),
            "new leaves must append in operation order");
    require(candidate->host().root().level() > oldRoot.level() &&
                candidate->host().node(oldRoot)->parent != g::NodeId{},
            "fixture must grow and reparent old root");
    g::GenerationRequest request;
    request.placement.minimumBrickCount = {1, 2, 1};
    request.placement.apron = 1;
    std::unique_ptr<g::PreparedGeneration> ticket;
    check(detail::VolumeAdoptionBuilder::prepareAccumulation(*owner, *candidate, request, ticket),
          "prepare valid accumulation");
    auto* savedOutput = ticket.get();
    auto rejects = [&](const detail::AccumulationCandidate& invalid, const g::GenerationRequest& desc) {
        require(NVRHI_FAILED(detail::VolumeAdoptionBuilder::prepareAccumulation(*owner, invalid, desc, ticket)),
                "destructive/stale/invalid accumulation accepted");
        require(ticket.get() == savedOutput && owner->snapshot() == initial,
                "failed append changed ticket/publication");
        unchanged(owner->host(), before);
    };
    auto bad = request;
    bad.budget.maxBuffers = 1;
    rejects(*candidate, bad);
    bad = request;
    bad.budget.maxAllocatedBytes = 32;
    rejects(*candidate, bad);
    bad = request;
    bad.placement.preserve = false;
    rejects(*candidate, bad);
    g::ChannelUpload upload{0, original[0].data(), original[0].size()};
    bad = request;
    bad.uploads = &upload;
    bad.uploadCount = 1;
    rejects(*candidate, bad);
    auto foreignOwner = createOwner(device, artifact);
    auto foreign = begin(*foreignOwner);
    rejects(*foreign, request);
    auto destructive = begin(*owner);
    check(destructive->host().clear(), "clear private candidate");
    rejects(*destructive, request);
    destructive = begin(*owner);
    check(destructive->host().configure(configuration()), "reconfigure private candidate");
    rejects(*destructive, request);
    destructive = begin(*owner);
    check(destructive->host().setActive(oldLeaf, false), "deactivate private old leaf");
    check(destructive->host().finishTopology(), "finish destructive deactivation");
    rejects(*destructive, request);
    destructive = begin(*owner);
    for (uint32_t level = 0; level < 5; ++level)
        for (uint64_t i = 0; i < destructive->host().count(0, level); ++i)
            destructive->host().editNode(id(0, level, i))->position.x += 4;
    check(destructive->host().finishTopology(), "finish valid translated graph");
    rejects(*destructive, request);
    destructive = begin(*owner);
    auto* first = destructive->host().editNode(oldLeaf);
    auto* second = destructive->host().editNode(inactiveLeaf);
    std::swap(first->position, second->position);
    auto* list = destructive->host().editChildList(destructive->host().node(oldRoot)->children);
    std::swap(list[0], list[1]);
    check(destructive->host().finishTopology(), "finish valid reordered leaf graph");
    rejects(*destructive, request);
    for (uint32_t fieldIndex = 0; fieldIndex < 4; ++fieldIndex) {
        destructive = begin(*owner);
        auto* changed = destructive->host().editNode(oldLeaf);
        if (fieldIndex == 0)
            ++changed->priority;
        if (fieldIndex == 1)
            ++changed->reserved;
        if (fieldIndex == 2)
            changed->range[1] += 1;
        if (fieldIndex == 3)
            changed->mask ^= 1;
        check(destructive->host().finishTopology(), "finish changed semantic metadata");
        rejects(*destructive, request);
    }
    destructive = begin(*owner);
    destructive->host().editChildList(destructive->host().node(oldRoot)->children)[0] = {};
    require(NVRHI_FAILED(destructive->host().finishTopology()),
            "malformed disconnected graph passed CPU validation");
    rejects(*destructive, request);
    // A prepared ticket owns a second immutable copy. Later candidate edits must
    // neither be published by this ticket nor consume the reusable candidate.
    std::unique_ptr<g::PreparedGeneration> secondTicket;
    check(detail::VolumeAdoptionBuilder::prepareAccumulation(*owner, *candidate, request, secondTicket),
          "reuse candidate before publication");
    activate(candidate->host(), 4);
    check(candidate->host().finishTopology(), "modify private candidate after preparation");
    publish(h, *owner, *ticket);
    auto grown = owner->snapshot();
    require(&owner->host() == hostAddress && owner->host().revision().epoch == before.revision.epoch &&
                owner->host().revision().topology > before.revision.topology &&
                owner->host().activeLeafCount() == 4 && !owner->host().isAllocated({4, 0, 0}) &&
                owner->host().findLeaf({0, 0, 0}) == oldLeaf &&
                owner->host().findLeaf({2, 0, 0}) == inactiveLeaf && owner->host().isActive({2, 0, 0}),
            "append changed epoch/IDs or leaked later private edits");
    const auto* kept = owner->host().node(oldLeaf);
    require(kept->priority == oldRecord.priority && kept->reserved == oldRecord.reserved &&
                kept->mask == oldRecord.mask &&
                std::memcmp(kept->range, oldRecord.range, sizeof(kept->range)) == 0,
            "append lost semantic node metadata");
    require(detail::VolumeAdoptionBuilder::attachment(*grown) == tag.Get(), "append lost same-epoch metadata");
    h.begin();
    require(NVRHI_FAILED(owner->recordAndPublish(h.context(), *secondTicket)) && owner->snapshot() == grown,
            "second prepared append failed to reject stale base");
    h.submit();
    require(
        NVRHI_FAILED(detail::VolumeAdoptionBuilder::prepareAccumulation(*owner, *candidate, request, ticket)),
        "published owner did not invalidate candidate token");
    const auto expectedGrown = expectedAtlas(
        {
            {0,  0, 0},
            {2,  0, 0},
            {-2, 0, 0},
            {10, 0, 0}
    },
        {1, 2, 2}, 1, field);
    // Repacking without any addition must preserve IDs and all original payloads.
    candidate = begin(*owner);
    request.placement.minimumBrickCount = {3, 2, 1};
    request.placement.apron = 2;
    check(detail::VolumeAdoptionBuilder::prepareAccumulation(*owner, *candidate, request, ticket),
          "prepare no-addition repack");
    candidate.reset();
    publish(h, *owner, *ticket);
    auto repacked = owner->snapshot();
    require(owner->host().revision().epoch == before.revision.epoch &&
                owner->host().findLeaf({0, 0, 0}) == oldLeaf && owner->host().activeLeafCount() == 4,
            "no-addition repack changed stable IDs");
    const auto expectedRepacked = expectedAtlas(
        {
            {0,  0, 0},
            {2,  0, 0},
            {-2, 0, 0},
            {10, 0, 0}
    },
        {3, 2, 1}, 2, field);
    // A higher minimum root level adds ancestors only; it must keep every leaf.
    candidate = begin(*owner);
    g::NodeId promoted;
    check(candidate->host().activateAtLevel(4, {0, 0, 0}, promoted), "promote root without adding leaves");
    check(candidate->host().finishTopology(), "finish root-only promotion");
    check(detail::VolumeAdoptionBuilder::prepareAccumulation(*owner, *candidate, request, ticket),
          "prepare root-only promotion");
    candidate.reset();
    publish(h, *owner, *ticket);
    auto promotedSnapshot = owner->snapshot();
    require(owner->host().root().level() == 4 && owner->host().activeLeafCount() == 4 &&
                owner->host().revision().epoch == before.revision.epoch &&
                owner->host().findLeaf({0, 0, 0}) == oldLeaf,
            "root promotion replaced IDs or created unexpected leaves");
    // Host edits invalidate both candidate authentication and prepared tickets.
    candidate = begin(*owner);
    check(detail::VolumeAdoptionBuilder::prepareAccumulation(*owner, *candidate, request, ticket),
          "prepare stale host fixture");
    check(owner->host().finishTopology(), "advance live host revision");
    auto* outputBefore = ticket.get();
    require(
        NVRHI_FAILED(detail::VolumeAdoptionBuilder::prepareAccumulation(*owner, *candidate, request, ticket)) &&
            ticket.get() == outputBefore,
        "stale host candidate accepted");
    h.begin();
    require(
        NVRHI_FAILED(owner->recordAndPublish(h.context(), *ticket)) && owner->snapshot() == promotedSnapshot,
        "stale host append ticket changed publication");
    h.submit();
    candidate = begin(*owner);
    check(detail::VolumeAdoptionBuilder::prepareAccumulation(*owner, *candidate, request, ticket),
          "prepare stale channels fixture");
    check(owner->setChannel(0, descriptors[0]), "advance channel incarnation");
    require(NVRHI_FAILED(
                detail::VolumeAdoptionBuilder::prepareAccumulation(*owner, *candidate, request, secondTicket)),
            "stale channel candidate accepted");
    h.begin();
    require(
        NVRHI_FAILED(owner->recordAndPublish(h.context(), *ticket)) && owner->snapshot() == promotedSnapshot,
        "stale channel append ticket changed publication");
    auto readOriginal = recordRead(h, *owner, initial), readGrown = recordRead(h, *owner, grown);
    auto readRepacked = recordRead(h, *owner, repacked), readPromoted = recordRead(h, *owner, promotedSnapshot);
    ticket.reset();
    secondTicket.reset();
    candidate.reset();
    destructive.reset();
    foreign.reset();
    foreignOwner.reset();
    owner.reset();
    tag = nullptr;
    h.submit();
    verify(*readOriginal, original, "original non-dense poisoned import survived owner destruction");
    verify(*readGrown, expectedGrown, "five-format signed append/reactivation spatial oracle");
    verify(*readRepacked, expectedRepacked, "five-format no-addition remap/apron/unused oracle");
    verify(*readPromoted, expectedRepacked, "root-only promotion preserved raw channel bytes");
    require(detail::VolumeAdoptionBuilder::attachment(*initial) &&
                detail::VolumeAdoptionBuilder::attachment(*promotedSnapshot) ==
                    detail::VolumeAdoptionBuilder::attachment(*initial),
            "retained generation metadata lost after owner destruction");
    std::puts(
        "PASS authenticated accumulation: signed growth, stable IDs, reactivation, five formats, rollback, "
        "stale guards and retained lifetimes");
}
void runEmpty(IDevice* device, const std::vector<char>& artifact) {
    Harness h(device);
    auto owner = createOwner(device, artifact);
    for (uint32_t c = 0; c < 5; ++c)
        check(owner->setChannel(c, descriptors[c]), "empty channel configuration");
    const auto epoch = owner->host().revision().epoch;
    auto candidate = begin(*owner);
    g::GenerationRequest request;
    request.placement.minimumBrickCount = {1, 1, 1};
    std::unique_ptr<g::PreparedGeneration> ticket;
    check(detail::VolumeAdoptionBuilder::prepareAccumulation(*owner, *candidate, request, ticket),
          "empty append prepare");
    publish(h, *owner, *ticket);
    auto empty = owner->snapshot();
    require(owner->host().root().isNull() && owner->host().revision().epoch == epoch &&
                empty->bindings().channelMask == 31 && !empty->bindings().activeLeafCount,
            "empty accumulation fabricated topology or lost descriptors");
    candidate = begin(*owner);
    activate(candidate->host(), -2, -2, -2);
    check(candidate->host().finishTopology(), "finish first signed append");
    check(detail::VolumeAdoptionBuilder::prepareAccumulation(*owner, *candidate, request, ticket),
          "first append prepare");
    publish(h, *owner, *ticket);
    auto filled = owner->snapshot();
    require(owner->host().findLeaf({-2, -2, -2}) == id(0, 0, 0) && owner->host().revision().epoch == epoch,
            "first append from empty changed epoch or ID");
    h.begin();
    auto emptyRead = recordRead(h, *owner, empty), filledRead = recordRead(h, *owner, filled);
    owner.reset();
    candidate.reset();
    ticket.reset();
    h.submit();
    verify(*emptyRead, Channels{}, "empty generation readback");
    verify(*filledRead,
           expectedAtlas(
               {
                   {-2, -2, -2}
    },
               {1, 1, 1}, 1, Field{}),
           "first append initializes all five void channels");
    std::puts("PASS empty/no-addition accumulation and first signed leaf in the original epoch");
}
std::vector<char> readArtifact(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(bool(file), "open owner artifact");
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
            auto path = donut::app::GetDirectoryWithExecutable() / "shaders/gvdb_owner" / type /
                        "GVDBAtlasRepack_relocateAtlas.bin";
            for (int i = 1; i + 1 < argc; ++i)
                if (!std::strcmp(argv[i], "--owner-artifact"))
                    path = argv[i + 1];
            const auto artifact = readArtifact(path);
            run(device.device, artifact);
            runEmpty(device.device, artifact);
        }
        require(problems == 0, "validation warnings/errors");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
