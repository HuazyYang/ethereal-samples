// SPDX-License-Identifier: Apache-2.0
// Test-only device harness. The public owner has no Donut or native SDK includes.
#include "GVDBNvrhiVolume.h"
#include <ShaderToolArtifact.h>
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
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void check(FRESULT result, const char* message) {
    if (NVRHI_FAILED(result)) throw std::runtime_error(std::string(message) + ": " + std::to_string(result));
}
#if ETHEREAL_TEST_CUDA
struct Messages : ObjectImpl<IMessageCallback> {
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Messages)
    NVRHI_IMPLEMENTS_INTERFACE(IMessageCallback)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    void message(MessageSeverity severity, const char* text) noexcept override {
        if (severity >= MessageSeverity::Warning) ++problems;
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
            manager = TakeOver(donut::app::DeviceManager::Create(donut::app::GetGraphicsAPIFromCommandLine(argc, argv)));
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
    ~Device() { if (device) device->waitForIdle(); device = nullptr; if (manager) manager->Shutdown(); }
};
struct Harness {
    IDevice* device;
    CommandListHandle commands;
    explicit Harness(IDevice* value) : device(value) { check(device->createCommandList({}, &commands), "create command list"); }
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
        if (!view.byteSize) return {};
        BufferHandle staging;
        check(device->createBuffer(BufferDesc().setByteSize(view.byteSize).setCpuAccess(CpuAccessMode::Read)
            .setInitialState(ResourceStates::CopyDest).setKeepInitialState(true), &staging), "staging");
        begin(); commands->copyBuffer(staging, 0, view.buffer, view.byteOffset, view.byteSize); submit();
        const void* data = device->mapBuffer(staging, CpuAccessMode::Read);
        require(data != nullptr, "map staging");
        std::vector<uint8_t> result(size_t(view.byteSize));
        std::memcpy(result.data(), data, result.size()); device->unmapBuffer(staging);
        return result;
    }
};
using Key = std::tuple<int, int, int>;
using Content = std::map<Key, int>;
constexpr uint32_t mask = 31;
const g::ChannelDesc descriptors[] = {
    {g::AtlasFormat::R8_UINT, {23, 0, 0, 0}},
    {g::AtlasFormat::RGBA8_UINT, {153, 51, 204, 117}},
    {g::AtlasFormat::R32_FLOAT, {-3.5f, 0, 0, 0}},
    {g::AtlasFormat::RGB32_FLOAT, {-1.25f, 2.5f, 7.75f, 0}},
    {g::AtlasFormat::RGBA32_FLOAT, {3.25f, -4.5f, 1.75f, 9.5f}}
};
int floorCell(int value) { return value >= 0 ? (value / 2) * 2 : ((value - 1) / 2) * 2; }
std::vector<uint8_t> valueBytes(uint32_t channel, int x, int y, int z, int seed) {
    const auto& descriptor = descriptors[channel];
    const uint32_t stride = gvdb::slang::atlasStride(descriptor.format);
    std::vector<uint8_t> bytes(stride);
    if (seed < 0) {
        if (channel == 0) bytes[0] = uint8_t(descriptor.voidValue.x);
        else if (channel == 1) {
            bytes[0] = uint8_t(descriptor.voidValue.x); bytes[1] = uint8_t(descriptor.voidValue.y);
            bytes[2] = uint8_t(descriptor.voidValue.z); bytes[3] = uint8_t(descriptor.voidValue.w);
        } else std::memcpy(bytes.data(), &descriptor.voidValue, stride);
    } else if (channel <= 1) {
        for (uint32_t component = 0; component < stride; ++component)
            bytes[component] = uint8_t(x * 13 + y * 7 + z * 3 + seed + int(component) * 53 + int(channel) * 29);
    } else {
        for (uint32_t component = 0; component < stride / 4; ++component) {
            const float value = float(x * 13 + y * 7 + z * 3 + seed + int(component) * 23 + int(channel) * 19) / 8.f;
            uint32_t bits;
            std::memcpy(&bits, &value, 4);
            const uint32_t selector = (uint32_t(x) + uint32_t(y) * 3 + uint32_t(z) * 5 + component) % 7;
            if (selector == 0) bits = 0x80000000u; // Negative zero must not become positive zero.
            else if (selector == 1) bits = 0x7fc12345u ^ (component << 8); // Preserve NaN payload bits.
            std::memcpy(bytes.data() + component * 4, &bits, 4);
        }
    }
    return bytes;
}
std::vector<gvdb::slang::AtlasNode> readMap(Harness& h, const g::SnapshotBindings& bindings) {
    const auto bytes = h.read(bindings.atlasMap);
    std::vector<gvdb::slang::AtlasNode> map(bytes.size() / sizeof(gvdb::slang::AtlasNode));
    if (!bytes.empty()) std::memcpy(map.data(), bytes.data(), bytes.size());
    return map;
}
void verify(Harness& harness, const g::SnapshotBindings& bindings, const g::ChannelReadback& data,
            const Content& content, uint32_t freshChannels = 0) {
    require(data.channelMask == bindings.channelMask, "readback mask");
    const auto atlas = readMap(harness, bindings);
    const auto res = bindings.volume.atlasResolutionAndApron;
    const auto grid = bindings.volume.atlasBrickCountAndWidth;
    uint32_t active = 0;
    for (const auto& slot : atlas) if (slot.leaf >= 0) {
        require(content.count({slot.x, slot.y, slot.z}) != 0, "atlas map has an unexpected spatial leaf"); ++active;
    }
    require(active == content.size() && active == bindings.activeLeafCount, "atlas active count");
    for (uint32_t channel = 0; channel < 5; ++channel) {
        if (!(bindings.channelMask & (1u << channel))) continue;
        const auto& storage = bindings.channels[channel].storage;
        require(data.channels[channel].size() == storage.logicalBytes, "logical channel length");
        for (uint32_t z = 0; z < res.z; ++z) for (uint32_t y = 0; y < res.y; ++y) for (uint32_t x = 0; x < res.x; ++x) {
            const auto& slot = atlas[((z / grid.w) * grid.y + y / grid.w) * grid.x + x / grid.w];
            std::vector<uint8_t> expected(storage.voxelStride, 0);
            if (slot.leaf >= 0) {
                const int wx = slot.x + int(x % grid.w) - int(res.w);
                const int wy = slot.y + int(y % grid.w) - int(res.w);
                const int wz = slot.z + int(z % grid.w) - int(res.w);
                const auto found = content.find({floorCell(wx), floorCell(wy), floorCell(wz)});
                const int seed = found == content.end() || (freshChannels & (1u << channel)) ? -1 : found->second;
                expected = valueBytes(channel, wx, wy, wz, seed);
            }
            const size_t address = ((size_t(z) * res.y + y) * res.x + x) * storage.voxelStride;
            if (std::memcmp(expected.data(), data.channels[channel].data() + address, expected.size())) {
                std::fprintf(stderr, "channel %u atlas (%u,%u,%u), byte %zu mismatch\n", channel, x, y, z, address);
                throw std::runtime_error("atlas interior/apron byte mismatch");
            }
        }
        // Physical padding is independently checked via the public raw view.
        const auto physical = harness.read(bindings.channels[channel].raw);
        for (size_t byte = size_t(storage.logicalBytes); byte < physical.size(); ++byte)
            require(physical[byte] == 0, "physical tail not zero");
    }
}
g::ChannelReadback readChannels(Harness& h, g::NvrhiVolume& owner, std::shared_ptr<const g::VolumeGeneration> generation) {
    std::unique_ptr<g::PreparedReadback> readback;
    check(owner.prepareReadback(generation, generation->bindings().channelMask, readback), "prepare readback");
    h.begin(); check(owner.recordReadback(h.context(), *readback), "record readback"); h.submit();
    g::ChannelReadback result;
    check(g::NvrhiVolume::finishReadbackAfterCompletion(*readback, result), "finish readback");
    require(NVRHI_FAILED(g::NvrhiVolume::finishReadbackAfterCompletion(*readback, result)), "readback single use");
    return result;
}
void publish(Harness& h, g::NvrhiVolume& owner, const g::GenerationRequest& request) {
    std::unique_ptr<g::PreparedGeneration> prepared;
    check(owner.prepareGeneration(request, prepared), "prepare generation");
    const auto prior = owner.snapshot();
    require(NVRHI_FAILED(owner.recordAndPublish({nullptr, h.commands}, *prepared)), "wrong context rejected");
    require(owner.snapshot() == prior, "failed context preserved publication");
    h.begin(); check(owner.recordAndPublish(h.context(), *prepared), "publish generation");
    require(owner.recordAndPublish(h.context(), *prepared) == FE_NOT_ALIVE_OBJECT, "ticket single use"); h.submit();
}
std::vector<uint8_t> makeUpload(const g::SnapshotBindings& bindings, const std::vector<gvdb::slang::AtlasNode>& atlas,
                                uint32_t channel, int seed) {
    const auto res = bindings.volume.atlasResolutionAndApron;
    const auto grid = bindings.volume.atlasBrickCountAndWidth;
    const uint32_t stride = bindings.channels[channel].storage.voxelStride;
    std::vector<uint8_t> bytes(size_t(bindings.channels[channel].storage.logicalBytes), 0xd7);
    for (size_t slot = 0; slot < atlas.size(); ++slot) {
        if (atlas[slot].leaf < 0) continue;
        const uint32_t sx = uint32_t(slot % grid.x) * grid.w + res.w;
        const uint32_t sy = uint32_t((slot / grid.x) % grid.y) * grid.w + res.w;
        const uint32_t sz = uint32_t(slot / (size_t(grid.x) * grid.y)) * grid.w + res.w;
        for (uint32_t z = 0; z < 2; ++z) for (uint32_t y = 0; y < 2; ++y) for (uint32_t x = 0; x < 2; ++x) {
            const auto value = valueBytes(channel, atlas[slot].x + int(x), atlas[slot].y + int(y), atlas[slot].z + int(z), seed);
            std::memcpy(bytes.data() + ((size_t(sz + z) * res.y + sy + y) * res.x + sx + x) * stride, value.data(), stride);
        }
    }
    return bytes;
}
void activate(g::HostVolume& host, int x, int y, int z) {
    g::NodeId id; check(host.activate(x, y, z, id), "activate"); require(!id.isNull(), "activation ID");
}
std::unique_ptr<g::NvrhiVolume> create(IDevice* device, const std::vector<char>& artifact) {
    g::VolumeOwnerDesc desc;
    desc.device = device; desc.configuration.levelCount = 5;
    desc.configuration.logDimension.fill(1);
    desc.remapArtifact = artifact.data(); desc.remapArtifactBytes = artifact.size();
    std::unique_ptr<g::NvrhiVolume> result;
    check(g::NvrhiVolume::create(desc, result), "create owner"); return result;
}
void run(IDevice* device, IDevice* foreignDevice, const std::vector<char>& artifact) {
    std::fprintf(stderr, "owner fixture: artifact and creation\n");
    Harness h(device);
    // Valid artifact container with a deliberately incompatible reflected ABI.
    ShaderToolArtifactView artifactView{};
    require(ShaderToolArtifactOpen(artifact.data(), artifact.size(), &artifactView) == 0, "open test artifact");
    ShaderToolArtifactInfo info{}; ShaderToolArtifactEntry entry{};
    require(ShaderToolArtifactGetInfo(&artifactView, &info) == 0 &&
            ShaderToolArtifactGetEntry(&artifactView, 0, &entry) == 0, "read artifact description");
    std::vector<ShaderToolArtifactBinding> reflection(info.BindingCount);
    for (uint32_t i = 0; i < info.BindingCount; ++i)
        require(ShaderToolArtifactGetBinding(&artifactView, i, &reflection[i]) == 0, "read artifact binding");
    bool changed = false;
    for (auto& binding : reflection) if (binding.Kind == ST_BINDING_STRUCTURED_BUFFER && binding.RegisterIndex == 2) {
        binding.ElementStride = 48; changed = true;
    }
    require(changed, "found layout mutation target");
    ShaderToolArtifactDesc corrupted{};
    corrupted.Target = info.Target; corrupted.Compiler = info.Compiler; corrupted.Entries = &entry; corrupted.EntryCount = 1;
    corrupted.Bindings = reflection.data(); corrupted.BindingCount = uint32_t(reflection.size());
    size_t malformedSize = 0;
    require(ShaderToolArtifactWrite(&corrupted, nullptr, 0, &malformedSize) == 0, "size malformed ABI artifact");
    std::vector<char> malformed(malformedSize);
    require(ShaderToolArtifactWrite(&corrupted, malformed.data(), malformed.size(), &malformedSize) == 0,
        "write malformed ABI artifact");
    auto owner = create(device, artifact);
    g::VolumeOwnerDesc malformedDesc; malformedDesc.device = device;
    malformedDesc.configuration = owner->host().configuration();
    malformedDesc.remapArtifact = malformed.data(); malformedDesc.remapArtifactBytes = malformed.size();
    auto* preservedOwner = owner.get();
    require(g::NvrhiVolume::create(malformedDesc, owner) == FE_UNSUPPORTED && owner.get() == preservedOwner,
        "incompatible artifact reflection rejected before shader creation");
    for (uint32_t i = 0; i < 5; ++i) check(owner->setChannel(i, descriptors[i]), "add channel");
    require(owner->setChannel(32, descriptors[0]) == FE_INVALID_ARGS, "channel range");
    auto invalid = descriptors[1]; invalid.voidValue.z = .5f;
    require(owner->setChannel(5, invalid) == FE_INVALID_ARGS, "integer background range");
    activate(owner->host(), -2, 0, 0); activate(owner->host(), 0, 0, 0); activate(owner->host(), 0, 2, 0);
    check(owner->host().finishTopology(), "finish topology");
    g::GenerationRequest request; request.placement.minimumBrickCount = {3, 1, 1}; request.placement.apron = 2;
    publish(h, *owner, request);
    std::fprintf(stderr, "owner fixture: initial generation\n");
    Content content{{{-2, 0, 0}, -1}, {{0, 0, 0}, -1}, {{0, 2, 0}, -1}};
    verify(h, owner->snapshot()->bindings(), readChannels(h, *owner, owner->snapshot()), content);
    auto initial = owner->snapshot();
    const auto atlas = readMap(h, initial->bindings());
    std::array<std::vector<uint8_t>, 5> uploads;
    std::array<g::ChannelUpload, 5> uploadDescs;
    for (uint32_t channel = 0; channel < 5; ++channel) {
        uploads[channel] = makeUpload(initial->bindings(), atlas, channel, 17);
        uploadDescs[channel] = {channel, uploads[channel].data(), uploads[channel].size()};
    }
    request.uploads = uploadDescs.data(); request.uploadCount = uploadDescs.size();
    std::unique_ptr<g::PreparedGeneration> uploadTicket;
    check(owner->prepareGeneration(request, uploadTicket), "prepare copied upload");
    for (auto& bytes : uploads) std::fill(bytes.begin(), bytes.end(), 0); // Inputs are already owned by ticket.
    h.begin(); check(owner->recordAndPublish(h.context(), *uploadTicket), "publish upload"); h.submit();
    request.uploads = nullptr; request.uploadCount = 0;
    for (auto& pair : content) pair.second = 17;
    auto retained = owner->snapshot();
    verify(h, retained->bindings(), readChannels(h, *owner, retained), content);
    std::fprintf(stderr, "owner fixture: five-format upload verified\n");
    // Preparation failures, including allocation after partial progress, leave output and publication intact.
    std::unique_ptr<g::PreparedGeneration> preserved;
    check(owner->prepareGeneration(request, preserved), "valid prepared output");
    auto* originalTicket = preserved.get();
    CommandListHandle foreignCommands;
    check(foreignDevice->createCommandList({}, &foreignCommands), "create foreign list");
    foreignCommands->open();
    require(owner->recordAndPublish({device, foreignCommands}, *preserved) == FE_INVALID_ARGS,
        "same declared device but foreign command list rejected");
    foreignCommands->close();
    const auto foreignSubmission = foreignDevice->executeCommandList(foreignCommands);
    require(foreignSubmission || foreignDevice->getGraphicsAPI() == GraphicsAPI::D3D11, "submit empty foreign list");
    require(foreignDevice->waitForIdle(), "finish empty foreign list"); foreignDevice->runGarbageCollection();
    if (device->queryFeatureSupport(Feature::CopyQueue)) {
        CommandListHandle copyCommands;
        check(device->createCommandList(CommandListParameters().setQueueType(CommandQueue::Copy), &copyCommands), "copy list");
        copyCommands->open();
        require(owner->recordAndPublish({device, copyCommands}, *preserved) == FE_UNSUPPORTED,
            "compute work rejected on copy queue");
        copyCommands->close();
        require(device->executeCommandList(copyCommands, CommandQueue::Copy) != 0, "submit empty copy list");
        require(device->waitForIdle(), "finish empty copy list"); device->runGarbageCollection();
    }
    auto limited = request; limited.budget.maxBuffers = 9;
    require(owner->prepareGeneration(limited, preserved) == FE_OUT_OF_MEMORY && preserved.get() == originalTicket,
        "partial preparation budget rollback");
    require(owner->snapshot() == retained, "preparation preserved generation");
    auto other = create(device, artifact);
    require(NVRHI_FAILED(other->recordAndPublish(h.context(), *preserved)), "wrong owner ticket rejected");
    // Mutating and finishing topology invalidates prepared tickets, even if active count is unchanged.
    check(owner->host().finishTopology(), "advance host revision");
    require(NVRHI_FAILED(owner->recordAndPublish(h.context(), *preserved)), "stale host revision rejected");
    // Growth changes atlas Z. Removal must refill both removed neighbors and old aprons.
    const auto removed = owner->host().findLeaf({0, 0, 0});
    check(owner->host().setActive(removed, false), "remove middle leaf"); content.erase({0, 0, 0});
    activate(owner->host(), 2, 2, 0); activate(owner->host(), -4, 0, 0);
    content[{2, 2, 0}] = -1; content[{-4, 0, 0}] = -1;
    check(owner->host().finishTopology(), "finish growth");
    publish(h, *owner, request);
    require(owner->snapshot()->bindings().volume.atlasBrickCountAndWidth.z == 2, "atlas Z grew");
    verify(h, owner->snapshot()->bindings(), readChannels(h, *owner, owner->snapshot()), content);
    Content oldContent{{{-2, 0, 0}, 17}, {{0, 0, 0}, 17}, {{0, 2, 0}, 17}};
    verify(h, retained->bindings(), readChannels(h, *owner, retained), oldContent);
    // Full CPU rebuild changes node IDs/order, but surviving spatial data remains intact.
    check(owner->host().clear(), "clear graph for reorder");
    for (auto it = content.rbegin(); it != content.rend(); ++it)
        activate(owner->host(), std::get<0>(it->first), std::get<1>(it->first), std::get<2>(it->first));
    check(owner->host().finishTopology(), "finish reorder");
    request.placement.minimumBrickCount = {1, 3, 1}; request.placement.apron = 1;
    publish(h, *owner, request);
    verify(h, owner->snapshot()->bindings(), readChannels(h, *owner, owner->snapshot()), content);
    // Same logical channel/format after removal is a fresh incarnation.
    const auto colorRevision = owner->snapshot()->bindings().channels[1].revision.resource;
    check(owner->removeChannel(1), "remove color"); publish(h, *owner, request);
    require(!(owner->snapshot()->bindings().channelMask & 2), "removed channel publication");
    check(owner->setChannel(1, descriptors[1]), "readd color"); publish(h, *owner, request);
    require(owner->snapshot()->bindings().channels[1].revision.resource > colorRevision,
        "channel resource revision survives absent publication");
    verify(h, owner->snapshot()->bindings(), readChannels(h, *owner, owner->snapshot()), content, 2);
    // Single-level graph exercises null child buffer/dummy binding and coordinate limits.
    auto edge = create(device, artifact);
    g::LevelConfig edgeConfig; edgeConfig.levelCount = 1; edgeConfig.logDimension[0] = 1;
    check(edge->host().configure(edgeConfig), "edge configuration");
    check(edge->setChannel(0, descriptors[0]), "edge channel");
    activate(edge->host(), -8388604, 0, 0); check(edge->host().finishTopology(), "edge finish");
    g::GenerationRequest edgeRequest; edgeRequest.placement.minimumBrickCount = {1,1,1};
    publish(h, *edge, edgeRequest);
    Content edgeContent{{{-8388604, 0, 0}, 31}};
    const auto edgeAtlas = readMap(h, edge->snapshot()->bindings());
    auto edgeBytes = makeUpload(edge->snapshot()->bindings(), edgeAtlas, 0, 31);
    g::ChannelUpload edgeUpload{0, edgeBytes.data(), edgeBytes.size()};
    edgeRequest.uploads = &edgeUpload; edgeRequest.uploadCount = 1;
    publish(h, *edge, edgeRequest);
    edgeRequest.uploads = nullptr; edgeRequest.uploadCount = 0;
    edgeRequest.placement.apron = 2;
    publish(h, *edge, edgeRequest);
    verify(h, edge->snapshot()->bindings(), readChannels(h, *edge, edge->snapshot()), edgeContent);
    check(edge->host().clear(), "clear populated volume"); check(edge->host().finishTopology(), "finish cleared volume");
    publish(h, *edge, edgeRequest);
    require(!edge->snapshot()->bindings().channels[0].raw.buffer, "empty publication releases live atlas");
    activate(edge->host(), -8388604, 0, 0); check(edge->host().finishTopology(), "reactivate empty volume");
    publish(h, *edge, edgeRequest); edgeContent.begin()->second = -1;
    verify(h, edge->snapshot()->bindings(), readChannels(h, *edge, edge->snapshot()), edgeContent);
    // A changed leaf interpretation requires explicit discard, never implicit resampling.
    edgeConfig.logDimension[0] = 2; check(edge->host().configure(edgeConfig), "change leaf width");
    activate(edge->host(), 0,0,0); check(edge->host().finishTopology(), "finish incompatible geometry");
    std::unique_ptr<g::PreparedGeneration> incompatible;
    require(edge->prepareGeneration(edgeRequest, incompatible) == FE_UNSUPPORTED, "preserve rejects changed leaf width");
    edgeRequest.placement.preserve = false; publish(h, *edge, edgeRequest);
    edgeConfig.logDimension[0] = 1; check(edge->host().configure(edgeConfig), "restore edge width");
    activate(edge->host(), 8388606, 0, 0);
    check(edge->host().finishTopology(), "large CPU geometry remains valid");
    std::unique_ptr<g::PreparedGeneration> rejected;
    require(edge->prepareGeneration(edgeRequest, rejected) == FE_UNSUPPORTED, "inexact GPU domain rejected");
    std::fprintf(stderr, "owner fixture: spatial edits and edge coordinates verified\n");
    // Capture old and new snapshots/readbacks in one unsubmitted command list. Drop
    // every owner and preparation handle before submission; tickets retain the device.
    request.placement.minimumBrickCount = {2,1,1}; request.placement.apron = 2;
    std::unique_ptr<g::PreparedGeneration> finalRepack, competing;
    check(owner->prepareGeneration(request, finalRepack), "prepare unsubmitted final repack");
    check(owner->prepareGeneration(request, competing), "prepare competing same-base generation");
    std::unique_ptr<g::PreparedReadback> oldRead, newRead;
    check(owner->prepareReadback(retained, mask, oldRead), "retained old readback");
    h.begin(); check(owner->recordAndPublish(h.context(), *finalRepack), "record unsubmitted final repack");
    require(NVRHI_FAILED(owner->recordAndPublish(h.context(), *competing)), "stale base generation rejected");
    auto latest = owner->snapshot();
    check(owner->prepareReadback(latest, mask, newRead), "retained latest readback");
    check(owner->recordReadback(h.context(), *oldRead), "record old retained read");
    check(owner->recordReadback(h.context(), *newRead), "record new retained read");
    owner.reset(); preserved.reset(); uploadTicket.reset(); finalRepack.reset(); competing.reset();
    initial.reset(); other.reset(); edge.reset();
    h.submit();
    g::ChannelReadback oldData, newData;
    check(g::NvrhiVolume::finishReadbackAfterCompletion(*oldRead, oldData), "finish after owner destruction");
    check(g::NvrhiVolume::finishReadbackAfterCompletion(*newRead, newData), "finish new after destruction");
    verify(h, retained->bindings(), oldData, oldContent);
    verify(h, latest->bindings(), newData, content, 2);
    // Empty configured and allocated-but-inactive graphs publish no live atlas.
    auto empty = create(device, artifact); check(empty->setChannel(0, descriptors[0]), "empty channel");
    check(empty->host().finishTopology(), "empty finish"); publish(h, *empty, edgeRequest);
    require(!empty->snapshot()->bindings().channels[0].raw.buffer && !empty->snapshot()->bindings().activeLeafCount,
        "empty volume has no atlas allocation");
    activate(empty->host(), 0,0,0); check(empty->host().setActive(empty->host().findLeaf({0,0,0}), false), "inactive leaf");
    check(empty->host().finishTopology(), "inactive graph finish"); publish(h, *empty, edgeRequest);
    require(!empty->snapshot()->bindings().channels[0].raw.buffer && bool(empty->snapshot()->bindings().nodes.buffer),
        "inactive graph retains topology but has no atlas");
    check(empty->host().setActive(empty->host().findLeaf({0,0,0}), true), "reuse inactive leaf");
    check(empty->host().finishTopology(), "reuse finish"); publish(h, *empty, edgeRequest);
    Content reused{{{0,0,0},-1}};
    verify(h, empty->snapshot()->bindings(), readChannels(h, *empty, empty->snapshot()), reused);
    check(empty->setChannel(31, descriptors[0]), "add highest channel index");
    publish(h, *empty, edgeRequest);
    require(empty->snapshot()->bindings().channelMask == 0x80000001u, "channel31 mask");
    const auto highSize = empty->snapshot()->bindings().channels[31].storage.logicalBytes;
    std::vector<uint8_t> highUpload(size_t(highSize), 77);
    g::ChannelUpload highDesc{31, highUpload.data(), highSize};
    edgeRequest.uploads = &highDesc; edgeRequest.uploadCount = 1;
    publish(h, *empty, edgeRequest);
    edgeRequest.uploads = nullptr; edgeRequest.uploadCount = 0;
    const auto highData = readChannels(h, *empty, empty->snapshot());
    const auto highRes = empty->snapshot()->bindings().volume.atlasResolutionAndApron;
    for (uint32_t z = 0; z < highRes.z; ++z) for (uint32_t y = 0; y < highRes.y; ++y) for (uint32_t x = 0; x < highRes.x; ++x) {
        const bool interior = x >= highRes.w && x < highRes.w + 2 && y >= highRes.w && y < highRes.w + 2 &&
            z >= highRes.w && z < highRes.w + 2;
        require(highData.channels[31][(size_t(z) * highRes.y + y) * highRes.x + x] == (interior ? 77 : 23),
            "highest channel interior/apron bytes");
    }
    check(empty->removeChannel(31), "remove highest channel"); publish(h, *empty, edgeRequest);
    require(empty->snapshot()->bindings().channelMask == 1, "highest channel removal mask");
    std::fprintf(stderr, "owner fixture: lifetime, empty reuse and channel31 verified\n");
    std::puts("M5 owner: five raw formats, signed placement, grow/remove/reorder, aprons, tickets, retained lifetimes PASS");
}
}
int main(int argc, const char** argv) {
    donut::log::ConsoleApplicationMode();
    donut::log::SetCallback([](donut::log::Severity severity, const char* text) {
        if (severity >= donut::log::Severity::Warning) ++problems;
        std::fprintf(stderr, "%s\n", text);
    });
    try {
        {
            Device device(argc, argv);
            auto path = donut::app::GetDirectoryWithExecutable() / "shaders/gvdb_owner" /
                donut::app::GetShaderTypeName(device.device->getGraphicsAPI()) / "GVDBAtlasRepack_relocateAtlas.bin";
            for (int i = 1; i + 1 < argc; ++i) if (!std::strcmp(argv[i], "--artifact")) path = argv[i + 1];
            std::ifstream file(path, std::ios::binary);
            require(bool(file), "open repack artifact");
            std::vector<char> artifact((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            Device foreign(argc, argv);
            run(device.device, foreign.device, artifact);
        }
        require(problems == 0, "native/NVRHI validation warnings or errors");
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1; }
}
