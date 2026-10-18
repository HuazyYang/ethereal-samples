// SPDX-License-Identifier: Apache-2.0
#include "GVDBNvrhiVolume.h"
#include "GVDBNvrhiMutationInternal.h"
#include "GVDBNvrhiAdoptionInternal.h"
#include <nvrhi/shader-program.h>
#include <algorithm>
#include <cstring>
#include <map>
#include <new>
#include <stdexcept>
#include <tuple>

namespace gvdb::portable {
namespace {
using namespace nvrhi;
struct Failure { FRESULT result; };
void require(bool condition, FRESULT result = FE_INVALID_ARGS) { if (!condition) throw Failure{result}; }
void check(FRESULT result) { require(NVRHI_SUCCEEDED(result), result); }
template<class Function> FRESULT guarded(Function&& function) noexcept {
    try { function(); return FS_OK; }
    catch (const Failure& failure) { return failure.result; }
    catch (const std::bad_alloc&) { return FE_OUT_OF_MEMORY; }
    catch (const std::length_error&) { return FE_OUT_OF_MEMORY; }
    catch (...) { return FE_GENERIC_ERROR; }
}
uint64_t advance(uint64_t value) { uint64_t next; require(nextRevision(value, next), FE_UNSUPPORTED); return next; }
bool sameRevision(const TopologyRevision& a, const TopologyRevision& b) {
    return a.epoch == b.epoch && a.topology == b.topology && a.placement == b.placement;
}
struct OwnerIdentity {};
struct Upload { BufferHandle buffer; std::vector<uint8_t> bytes; };
struct Dispatch { ComputePipelineHandle pipeline; BindingSetHandle bindings; uint32_t x = 0, y = 0, z = 1; };
struct Placement {
    PackedHostTopology topology;
    std::vector<slang::AtlasNode> map;
};
Node readNode(const PackedHostTopology& topology, uint32_t level, uint32_t index) {
    Node node;
    const auto& info = topology.levels.at(level);
    const uint64_t address = uint64_t(info.nodes.x) + uint64_t(index) * info.nodes.y;
    require(info.nodes.y == sizeof(Node) && address + sizeof(Node) <= topology.nodes.size() * sizeof(uint32_t));
    std::memcpy(&node, reinterpret_cast<const uint8_t*>(topology.nodes.data()) + address, sizeof(node));
    return node;
}
void setInterior(PackedHostTopology& topology, uint32_t index, Int3 interior) {
    const uint64_t address = uint64_t(topology.levels[0].nodes.x) + uint64_t(index) * sizeof(Node) + offsetof(Node, atlasInterior);
    std::memcpy(reinterpret_cast<uint8_t*>(topology.nodes.data()) + address, &interior, sizeof(interior));
}
Placement place(const HostVolume& host, const AtlasPlacement& request) {
    Placement output;
    check(host.packTopology(output.topology));
    require(request.minimumBrickCount.x && request.minimumBrickCount.y && request.minimumBrickCount.z);
    // Integer traversal is used by the remap. The published float metadata remains
    // compatible with existing kernels only within this exact voxel-center domain.
    constexpr int64_t limit = (int64_t(1) << 23) - 1;
    for (uint32_t level = 0; level < output.topology.levels.size(); ++level) {
        const auto& info = output.topology.levels[level];
        for (uint32_t index = 0; index < info.nodes.z; ++index) {
            const Node node = readNode(output.topology, level, index);
            const int64_t origins[] = {node.position.x, node.position.y, node.position.z};
            const int64_t extents[] = {info.nodeExtent.x, info.nodeExtent.y, info.nodeExtent.z};
            const int64_t apron = level == 0 ? request.apron : 0;
            for (uint32_t axis = 0; axis < 3; ++axis)
                require(origins[axis] - apron >= -limit && origins[axis] + extents[axis] + apron <= limit,
                        FE_UNSUPPORTED);
            if (level == 0) setInterior(output.topology, index, {-1, -1, -1});
        }
    }
    auto& volume = output.topology.volume;
    volume.atlasResolutionAndApron = {};
    volume.atlasBrickCountAndWidth = {};
    if (!output.topology.activeLeafCount) return output;
    const uint64_t width = uint64_t(host.range(0)) + uint64_t(request.apron) * 2;
    require(width && width <= INT32_MAX, FE_UNSUPPORTED);
    UInt3 grid = request.minimumBrickCount;
    const uint64_t plane = uint64_t(grid.x) * grid.y;
    require(plane <= UINT32_MAX, FE_UNSUPPORTED);
    const uint64_t neededZ = (uint64_t(output.topology.activeLeafCount) + plane - 1) / plane;
    require(neededZ <= UINT32_MAX, FE_UNSUPPORTED);
    grid.z = std::max(grid.z, uint32_t(neededZ));
    require(grid.z <= UINT32_MAX / plane, FE_UNSUPPORTED);
    const uint64_t slots = plane * grid.z;
    require(slots <= c_AddressSpaceBytes / sizeof(slang::AtlasNode), FE_UNSUPPORTED);
    require(width * grid.x <= INT32_MAX && width * grid.y <= INT32_MAX && width * grid.z <= INT32_MAX,
            FE_UNSUPPORTED);
    const UInt3 resolution{uint32_t(width * grid.x), uint32_t(width * grid.y), uint32_t(width * grid.z)};
    ChannelStorage checked;
    check(describeChannel(AtlasFormat::R8_UINT, resolution, checked).result);
    volume.atlasResolutionAndApron = {resolution.x, resolution.y, resolution.z, request.apron};
    volume.atlasBrickCountAndWidth = {grid.x, grid.y, grid.z, uint32_t(width)};
    output.map.assign(size_t(slots), {0, 0, 0, -1});
    uint32_t slot = 0;
    for (uint32_t index = 0; index < output.topology.levels[0].nodes.z; ++index) {
        const Node node = readNode(output.topology, 0, index);
        if (!node.flags) continue;
        require(slot < slots && index <= INT32_MAX);
        const uint32_t x = slot % grid.x, y = (slot / grid.x) % grid.y, z = slot / uint32_t(plane);
        setInterior(output.topology, index, {int32_t(x * width + request.apron),
            int32_t(y * width + request.apron), int32_t(z * width + request.apron)});
        output.map[slot++] = {node.position.x, node.position.y, node.position.z, int32_t(index)};
    }
    require(slot == output.topology.activeLeafCount);
    return output;
}
using Origin = std::tuple<int32_t, int32_t, int32_t>;
Origin origin(const Node& node) { return {node.position.x, node.position.y, node.position.z}; }
struct alignas(16) RepackParameters {
    VolumeInfo volume;
    slang::UInt4 sourceResolutionAndStride;
    slang::UInt4 work;
    slang::UInt4 voidBits;
};
static_assert(sizeof(RepackParameters) == 128);
slang::UInt4 background(const ChannelDesc& desc) {
    slang::UInt4 output{};
    if (desc.format == AtlasFormat::R8_UINT) output.x = uint32_t(desc.voidValue.x);
    else if (desc.format == AtlasFormat::RGBA8_UINT)
        output.x = uint32_t(desc.voidValue.x) | (uint32_t(desc.voidValue.y) << 8) |
            (uint32_t(desc.voidValue.z) << 16) | (uint32_t(desc.voidValue.w) << 24);
    else std::memcpy(&output, &desc.voidValue, sizeof(output));
    return output;
}
struct Allocator {
    IDevice* device;
    PreparationBudget budget;
    uint64_t bytes = 0;
    uint32_t count = 0;
    std::vector<Upload>* uploads;
    BufferHandle create(uint64_t size, uint32_t stride, bool raw, bool constant = false, bool readback = false) {
        require(size && size <= c_AddressSpaceBytes, FE_UNSUPPORTED);
        require(count < budget.maxBuffers && size <= budget.maxAllocatedBytes - bytes, FE_OUT_OF_MEMORY);
        BufferDesc desc;
        desc.byteSize = size;
        desc.structStride = stride;
        desc.canHaveRawViews = raw;
        desc.canHaveUAVs = !constant && !readback;
        desc.isConstantBuffer = constant;
        desc.cpuAccess = readback ? CpuAccessMode::Read : CpuAccessMode::None;
        desc.initialState = constant ? ResourceStates::ConstantBuffer : ResourceStates::CopyDest;
        desc.keepInitialState = true;
        desc.debugName = "GVDB volume generation";
        BufferHandle buffer;
        check(device->createBuffer(desc, &buffer));
        require(bool(buffer), FE_OUT_OF_MEMORY);
        bytes += size;
        ++count;
        return buffer;
    }
    BufferHandle upload(const void* data, uint64_t logical, uint32_t stride, bool raw, bool constant = false) {
        require(logical && data && logical <= c_AddressSpaceBytes);
        const uint64_t padded = constant ? ((logical + 255) & ~uint64_t(255)) : ((logical + 3) & ~uint64_t(3));
        auto buffer = create(padded, stride, raw, constant);
        Upload operation{buffer, std::vector<uint8_t>(size_t(padded), 0)};
        std::memcpy(operation.bytes.data(), data, size_t(logical));
        uploads->push_back(std::move(operation));
        return buffer;
    }
};
BufferView view(BufferHandle buffer) {
    BufferView result;
    result.byteSize = buffer ? buffer->getDesc().byteSize : 0;
    result.buffer = std::move(buffer);
    return result;
}
FRESULT validateContext(RecordingContext context, IDevice* expected, bool compute) {
    if (!context.device || !context.commands || context.device != expected || context.commands->getDevice() != expected)
        return FE_INVALID_ARGS;
    if (compute && context.commands->getDesc().queueType == CommandQueue::Copy) return FE_UNSUPPORTED;
    return FS_OK;
}
}
struct VolumeGeneration::Impl {
    // Destruction releases the retained device last through SnapshotBindings.
    SnapshotBindings bindings;
    std::shared_ptr<OwnerIdentity> owner;
    std::shared_ptr<const Placement> placement;
    AutoPtr<IRHIObject> attachment;
    std::array<uint64_t, c_MaxChannels> incarnations{};
    uint64_t serial = 0;
};
struct HostAdoption {
    std::unique_ptr<HostVolume> host;
    std::array<ChannelDesc, c_MaxChannels> channels{};
    uint64_t channelRevision = 0, incarnationSerial = 0;
};
struct PreparedGeneration::Impl {
    DeviceHandle device;
    std::shared_ptr<OwnerIdentity> owner;
    TopologyRevision hostRevision;
    uint64_t baseSerial = 0, channelRevision = 0;
    std::shared_ptr<VolumeGeneration> generation;
    std::vector<Upload> uploads;
    std::vector<Dispatch> dispatches;
    std::unique_ptr<HostAdoption> adoption;
    bool consumed = false;
};
struct PreparedReadback::Impl {
    DeviceHandle device;
    std::shared_ptr<OwnerIdentity> owner;
    std::shared_ptr<const VolumeGeneration> source;
    std::array<BufferHandle, c_MaxChannels> staging;
    uint32_t mask = 0;
    bool recorded = false, completed = false;
};
struct NvrhiVolume::Impl {
    DeviceHandle device;
    HostVolume host;
    std::shared_ptr<OwnerIdentity> owner = std::make_shared<OwnerIdentity>();
    ShaderProgramHandle program;
    BindingLayoutDesc layoutDesc;
    BindingLayoutHandle layout;
    ComputePipelineHandle pipeline;
    std::array<ChannelDesc, c_MaxChannels> channels{};
    std::array<uint64_t, c_MaxChannels> incarnations{};
    uint32_t mask = 0;
    uint64_t channelRevision = 0, serial = 0, incarnationSerial = 0;
    std::shared_ptr<VolumeGeneration> current;
    explicit Impl(const VolumeOwnerDesc& desc) : device(desc.device), host(desc.hostLimits) {}
};
PreparedGeneration::PreparedGeneration(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}
PreparedGeneration::~PreparedGeneration() = default;
VolumeGeneration::VolumeGeneration(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}
VolumeGeneration::~VolumeGeneration() = default;
const SnapshotBindings& VolumeGeneration::bindings() const noexcept { return m_impl->bindings; }
PreparedReadback::PreparedReadback(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}
PreparedReadback::~PreparedReadback() = default;
NvrhiVolume::NvrhiVolume(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}
NvrhiVolume::~NvrhiVolume() = default;
HostVolume& NvrhiVolume::host() noexcept { return m_impl->host; }
const HostVolume& NvrhiVolume::host() const noexcept { return m_impl->host; }
std::shared_ptr<const VolumeGeneration> NvrhiVolume::snapshot() const noexcept { return m_impl->current; }
FRESULT NvrhiVolume::create(const VolumeOwnerDesc& desc, std::unique_ptr<NvrhiVolume>& output) noexcept {
    return guarded([&] {
        require(desc.device && desc.remapArtifact && desc.remapArtifactBytes);
        check(validateLevelConfig(desc.configuration).result);
        auto impl = std::make_unique<Impl>(desc);
        check(impl->host.configure(desc.configuration));
        check(createShaderProgram(desc.device, desc.remapArtifact, desc.remapArtifactBytes, &impl->program));
        uint32_t entry;
        check(impl->program->findEntryPoint("relocateAtlas", ShaderType::Compute, &entry));
        ShaderEntryPointDesc entryDesc;
        check(impl->program->getEntryPointDesc(entry, entryDesc));
        require(entryDesc.threadGroupSize[0] == 256 && entryDesc.threadGroupSize[1] == 1 &&
            entryDesc.threadGroupSize[2] == 1, FE_UNSUPPORTED);
        check(impl->program->getBindingLayoutDesc(entry, 0, impl->layoutDesc));
        // Require the exact buffer ABI before any generation can be prepared.
        const ResourceType types[] = {ResourceType::ConstantBuffer, ResourceType::RawBuffer_SRV,
            ResourceType::RawBuffer_SRV, ResourceType::StructuredBuffer_SRV, ResourceType::StructuredBuffer_SRV,
            ResourceType::StructuredBuffer_SRV, ResourceType::RawBuffer_SRV, ResourceType::RawBuffer_UAV};
        const uint32_t slots[] = {0, 0, 1, 2, 3, 4, 5, 0};
        require(impl->layoutDesc.bindings.size() == 8, FE_UNSUPPORTED);
        for (uint32_t i = 0; i < 8; ++i)
            require(std::count_if(impl->layoutDesc.bindings.begin(), impl->layoutDesc.bindings.end(),
                [&](const auto& item) { return item.type == types[i] && item.slot == slots[i]; }) == 1, FE_UNSUPPORTED);
        uint32_t globals = 0, members = 0, memberMask = 0, constantIndex = UINT32_MAX;
        for (uint32_t i = 0; i < impl->program->getParameterCount(entry); ++i) {
            ShaderParameterDesc parameter;
            check(impl->program->getParameterDesc(entry, i, parameter));
            require(parameter.registerSpace == 0 && parameter.count == 1, FE_UNSUPPORTED);
            if (parameter.scope == ShaderParameterScope::Global) {
                ++globals;
                require(parameter.parentParameter == UINT32_MAX, FE_UNSUPPORTED);
                if (parameter.resourceType == ResourceType::ConstantBuffer) {
                    require(parameter.registerIndex == 0 && parameter.elementStride == sizeof(RepackParameters), FE_UNSUPPORTED);
                    constantIndex = i;
                } else if (parameter.resourceType == ResourceType::StructuredBuffer_SRV) {
                    const uint32_t expected = parameter.registerIndex == 2 ? sizeof(LevelInfo) : sizeof(slang::AtlasNode);
                    require(parameter.registerIndex >= 2 && parameter.registerIndex <= 4 && parameter.elementStride == expected,
                            FE_UNSUPPORTED);
                } else require((parameter.resourceType == ResourceType::RawBuffer_SRV ||
                                parameter.resourceType == ResourceType::RawBuffer_UAV) && parameter.elementStride == 1,
                                FE_UNSUPPORTED);
            } else {
                require(parameter.scope == ShaderParameterScope::ConstantBuffer && parameter.resourceType == ResourceType::None &&
                    parameter.size == 16 && parameter.elementStride == 16 && parameter.offset < sizeof(RepackParameters) &&
                    parameter.offset % 16 == 0, FE_UNSUPPORTED);
                const uint32_t bit = 1u << (parameter.offset / 16);
                require(!(memberMask & bit), FE_UNSUPPORTED);
                memberMask |= bit;
                ++members;
            }
        }
        require(globals == 8 && members == 8 && memberMask == 255 && constantIndex != UINT32_MAX, FE_UNSUPPORTED);
        for (uint32_t i = 0; i < impl->program->getParameterCount(entry); ++i) {
            ShaderParameterDesc parameter;
            check(impl->program->getParameterDesc(entry, i, parameter));
            if (parameter.scope == ShaderParameterScope::ConstantBuffer)
                require(parameter.parentParameter == constantIndex, FE_UNSUPPORTED);
        }
        check(desc.device->createBindingLayout(impl->layoutDesc, &impl->layout));
        ShaderHandle shader;
        check(impl->program->createShader(entry, &shader));
        check(desc.device->createComputePipeline(ComputePipelineDesc().setComputeShader(shader).addBindingLayout(impl->layout),
                                                &impl->pipeline));
        auto candidate = std::unique_ptr<NvrhiVolume>(new NvrhiVolume(std::move(impl)));
        output = std::move(candidate);
    });
}
FRESULT NvrhiVolume::setChannel(uint32_t channel, const ChannelDesc& desc) noexcept {
    return guarded([&] {
        require(channel < c_MaxChannels);
        check(validateChannelDesc(desc).result);
        auto& state = *m_impl;
        const uint64_t nextConfig = advance(state.channelRevision);
        const uint64_t nextIncarnation = advance(state.incarnationSerial);
        // Setting an existing descriptor is an explicit fresh channel incarnation.
        // Editing background/format must not silently preserve an incompatible field.
        state.channels[channel] = desc;
        state.incarnations[channel] = nextIncarnation;
        state.incarnationSerial = nextIncarnation;
        state.channelRevision = nextConfig;
        state.mask |= uint32_t(1) << channel;
    });
}
FRESULT NvrhiVolume::removeChannel(uint32_t channel) noexcept {
    return guarded([&] {
        require(channel < c_MaxChannels);
        auto& state = *m_impl;
        require((state.mask & (uint32_t(1) << channel)) != 0, FE_NOT_FOUND);
        const uint64_t next = advance(state.channelRevision);
        state.mask &= ~(uint32_t(1) << channel);
        state.incarnations[channel] = 0;
        state.channelRevision = next;
    });
}
FRESULT NvrhiVolume::describePlacement(const AtlasPlacement& placement, VolumeInfo& output) const noexcept {
    return guarded([&] { auto candidate = place(m_impl->host, placement); output = candidate.topology.volume; });
}
FRESULT NvrhiVolume::prepareGeneration(const GenerationRequest& request, std::unique_ptr<PreparedGeneration>& output) noexcept {
    return prepareGenerationForHost(m_impl->host, request, output);
}
FRESULT NvrhiVolume::prepareGenerationForHost(const HostVolume& host, const GenerationRequest& request,
                                            std::unique_ptr<PreparedGeneration>& output) noexcept {
    return guarded([&] {
        auto& state = *m_impl;
        require(request.uploadCount <= c_MaxChannels && (!request.uploadCount || request.uploads));
        auto ticket = std::make_unique<PreparedGeneration::Impl>();
        ticket->device = state.device;
        ticket->owner = state.owner;
        ticket->baseSerial = state.serial;
        ticket->channelRevision = state.channelRevision;
        ticket->hostRevision = state.host.revision();
        auto generation = std::make_unique<VolumeGeneration::Impl>();
        generation->owner = state.owner;
        generation->serial = advance(state.serial);
        generation->incarnations = state.incarnations;
        auto placedStorage = std::make_shared<Placement>(place(host, request.placement));
        generation->placement = placedStorage;
        auto& placed = *placedStorage;
        if (state.current && state.current->bindings().revision.epoch == placed.topology.revision.epoch)
            generation->attachment = state.current->m_impl->attachment;
        if (request.placement.preserve && state.current && state.current->bindings().activeLeafCount &&
            placed.topology.activeLeafCount)
            require(state.current->m_impl->placement->topology.levels[0].nodeExtent.x == placed.topology.levels[0].nodeExtent.x,
                FE_UNSUPPORTED);
        auto& bindings = generation->bindings;
        bindings.device = state.device;
        bindings.volume = placed.topology.volume;
        bindings.activeLeafCount = placed.topology.activeLeafCount;
        bindings.revision = placed.topology.revision;
        bindings.revision.placement = state.current ? advance(state.current->bindings().revision.placement) : 1;
        std::copy(placed.topology.levels.begin(), placed.topology.levels.end(), bindings.levels.begin());
        bindings.channelMask = state.mask;
        const UInt3 resolution{bindings.volume.atlasResolutionAndApron.x, bindings.volume.atlasResolutionAndApron.y,
                               bindings.volume.atlasResolutionAndApron.z};
        std::array<const ChannelUpload*, c_MaxChannels> inputs{};
        for (size_t i = 0; i < request.uploadCount; ++i) {
            const auto& input = request.uploads[i];
            require(input.channel < c_MaxChannels && !inputs[input.channel] && (state.mask & (uint32_t(1) << input.channel)));
            inputs[input.channel] = &input;
        }
        for (uint32_t channel = 0; channel < c_MaxChannels; ++channel) {
            if (!(state.mask & (uint32_t(1) << channel))) continue;
            auto& binding = bindings.channels[channel];
            binding.desc = state.channels[channel];
            check(describeChannel(binding.desc.format, resolution, binding.storage).result);
            const auto* input = inputs[channel];
            if (input) require(input->byteCount == binding.storage.logicalBytes && (!input->byteCount || input->data));
            // Generation serials never reset when a logical channel is absent.
            binding.revision = {generation->serial, generation->serial};
        }
        Allocator allocator{state.device, request.budget, 0, 0, &ticket->uploads};
        if (!placed.topology.nodes.empty()) bindings.nodes = view(allocator.upload(placed.topology.nodes.data(),
            placed.topology.nodes.size() * sizeof(uint32_t), 0, true));
        if (!placed.topology.children.empty()) bindings.children = view(allocator.upload(placed.topology.children.data(),
            placed.topology.children.size() * sizeof(uint32_t), 0, true));
        bindings.levelMetadata = view(allocator.upload(placed.topology.levels.data(),
            placed.topology.levels.size() * sizeof(LevelInfo), sizeof(LevelInfo), false));
        if (!placed.map.empty()) bindings.atlasMap = view(allocator.upload(placed.map.data(),
            placed.map.size() * sizeof(slang::AtlasNode), sizeof(slang::AtlasNode), false));
        if (bindings.activeLeafCount && state.mask) {
            const uint32_t zero = 0;
            const auto dummy = allocator.upload(&zero, sizeof(zero), 0, true);
            const auto nodes = bindings.nodes.buffer ? bindings.nodes.buffer : dummy;
            const auto children = bindings.children.buffer ? bindings.children.buffer : dummy;
            const uint32_t leafCount = placed.topology.levels[0].nodes.z;
            std::vector<slang::AtlasNode> oldInteriors(leafCount, {0, 0, 0, -1});
            std::vector<slang::AtlasNode> newInteriors(leafCount, {0, 0, 0, -1});
            std::map<Origin, Int3> oldByOrigin;
            bool compatible = false;
            if (request.placement.preserve && state.current && state.current->bindings().activeLeafCount) {
                const auto& old = state.current->m_impl->placement->topology;
                compatible = old.levels[0].nodeExtent.x == placed.topology.levels[0].nodeExtent.x;
                if (compatible) for (uint32_t index = 0; index < old.levels[0].nodes.z; ++index) {
                    const Node node = readNode(old, 0, index);
                    if (node.flags) oldByOrigin.emplace(origin(node), node.atlasInterior);
                }
            }
            for (uint32_t index = 0; index < leafCount; ++index) {
                const Node node = readNode(placed.topology, 0, index);
                if (!node.flags) continue;
                newInteriors[index] = {node.atlasInterior.x, node.atlasInterior.y, node.atlasInterior.z, 1};
                const auto found = oldByOrigin.find(origin(node));
                if (found != oldByOrigin.end()) oldInteriors[index] = {found->second.x, found->second.y, found->second.z, 1};
            }
            const auto oldMap = allocator.upload(oldInteriors.data(), oldInteriors.size() * sizeof(slang::AtlasNode),
                                                  sizeof(slang::AtlasNode), false);
            BufferHandle newMap;
            if (request.uploadCount) newMap = allocator.upload(newInteriors.data(), newInteriors.size() * sizeof(slang::AtlasNode),
                                                                sizeof(slang::AtlasNode), false);
            for (uint32_t channel = 0; channel < c_MaxChannels; ++channel) {
                if (!(state.mask & (uint32_t(1) << channel))) continue;
                auto& binding = bindings.channels[channel];
                RepackParameters parameters{};
                parameters.volume = bindings.volume;
                parameters.sourceResolutionAndStride.w = binding.storage.voxelStride;
                parameters.work = {binding.storage.voxelCount, uint32_t(binding.storage.allocatedBytes / 4),
                    uint32_t((binding.storage.logicalBytes - 1) % 4 + 1), bindings.activeLeafCount};
                parameters.voidBits = background(binding.desc);
                BufferHandle source = dummy, map = oldMap;
                if (inputs[channel]) {
                    source = allocator.upload(inputs[channel]->data, inputs[channel]->byteCount, 0, true);
                    map = newMap;
                    parameters.sourceResolutionAndStride = {resolution.x, resolution.y, resolution.z, binding.storage.voxelStride};
                } else if (compatible && state.current->m_impl->incarnations[channel] == state.incarnations[channel] &&
                           (state.current->bindings().channelMask & (uint32_t(1) << channel))) {
                    source = state.current->bindings().channels[channel].raw.buffer;
                    const auto& oldResolution = state.current->bindings().volume.atlasResolutionAndApron;
                    parameters.sourceResolutionAndStride = {oldResolution.x, oldResolution.y, oldResolution.z, binding.storage.voxelStride};
                }
                binding.raw = view(allocator.create(binding.storage.allocatedBytes, 0, true));
                const auto constants = allocator.upload(&parameters, sizeof(parameters), 0, false, true);
                BindingSetDesc desc;
                desc.trackLiveness = true;
                desc.addItem(BindingSetItem::ConstantBuffer(0, constants));
                desc.addItem(BindingSetItem::RawBuffer_SRV(0, nodes));
                desc.addItem(BindingSetItem::RawBuffer_SRV(1, children));
                desc.addItem(BindingSetItem::StructuredBuffer_SRV(2, bindings.levelMetadata.buffer));
                desc.addItem(BindingSetItem::StructuredBuffer_SRV(3, bindings.atlasMap.buffer));
                desc.addItem(BindingSetItem::StructuredBuffer_SRV(4, map));
                desc.addItem(BindingSetItem::RawBuffer_SRV(5, source));
                desc.addItem(BindingSetItem::RawBuffer_UAV(0, binding.raw.buffer));
                Dispatch dispatch;
                dispatch.pipeline = state.pipeline;
                check(state.device->createBindingSet(desc, state.layout, &dispatch.bindings));
                const uint32_t groups = (parameters.work.y + 255u) / 256u;
                dispatch.x = std::min(groups, 65535u);
                dispatch.y = (groups + 65534u) / 65535u;
                ticket->dispatches.push_back(std::move(dispatch));
            }
        }
        ticket->generation = std::shared_ptr<VolumeGeneration>(new VolumeGeneration(std::move(generation)));
        auto candidate = std::unique_ptr<PreparedGeneration>(new PreparedGeneration(std::move(ticket)));
        output = std::move(candidate);
    });
}
FRESULT NvrhiVolume::recordAndPublish(RecordingContext context, PreparedGeneration& prepared) noexcept {
    return guarded([&] {
        auto& state = *m_impl;
        auto& ticket = *prepared.m_impl;
        require(!ticket.consumed, FE_NOT_ALIVE_OBJECT);
        require(ticket.owner == state.owner && ticket.baseSerial == state.serial && ticket.channelRevision == state.channelRevision &&
            !state.host.dirty() && sameRevision(ticket.hostRevision, state.host.revision()));
        check(validateContext(context, state.device, true));
        ticket.consumed = true;
        for (const auto& upload : ticket.uploads)
            context.commands->writeBuffer(upload.buffer, upload.bytes.data(), upload.bytes.size());
        for (const auto& dispatch : ticket.dispatches) {
            context.commands->setComputeState(ComputeState().setPipeline(dispatch.pipeline).addBindingSet(dispatch.bindings));
            context.commands->dispatch(dispatch.x, dispatch.y, dispatch.z);
        }
        if (ticket.adoption) {
            // All potentially failing work preceded recording. Swap storage inside
            // the stable host object, invalidating borrowed node/pool pointers.
            state.host.swap(*ticket.adoption->host);
            state.channels = ticket.adoption->channels;
            state.mask = ticket.generation->bindings().channelMask;
            state.incarnations = ticket.generation->m_impl->incarnations;
            state.channelRevision = ticket.adoption->channelRevision;
            state.incarnationSerial = ticket.adoption->incarnationSerial;
        }
        state.current = ticket.generation;
        state.serial = ticket.generation->m_impl->serial;
    });
}
FRESULT NvrhiVolume::prepareReadback(std::shared_ptr<const VolumeGeneration> generation, uint32_t channelMask,
                                   std::unique_ptr<PreparedReadback>& output) noexcept {
    return guarded([&] {
        require(generation && generation->m_impl->owner == m_impl->owner);
        require(!(channelMask & ~generation->bindings().channelMask));
        auto impl = std::make_unique<PreparedReadback::Impl>();
        impl->device = m_impl->device;
        impl->owner = m_impl->owner;
        impl->source = std::move(generation);
        impl->mask = channelMask;
        Allocator allocator{m_impl->device, {}, 0, 0, nullptr};
        for (uint32_t channel = 0; channel < c_MaxChannels; ++channel) {
            if (!(channelMask & (uint32_t(1) << channel))) continue;
            const auto bytes = impl->source->bindings().channels[channel].storage.allocatedBytes;
            if (bytes) impl->staging[channel] = allocator.create(bytes, 0, false, false, true);
        }
        auto candidate = std::unique_ptr<PreparedReadback>(new PreparedReadback(std::move(impl)));
        output = std::move(candidate);
    });
}
FRESULT NvrhiVolume::recordReadback(RecordingContext context, PreparedReadback& prepared) noexcept {
    return guarded([&] {
        auto& impl = *prepared.m_impl;
        require(impl.owner == m_impl->owner && !impl.recorded && !impl.completed);
        check(validateContext(context, m_impl->device, false));
        impl.recorded = true;
        for (uint32_t channel = 0; channel < c_MaxChannels; ++channel) {
            if (!impl.staging[channel]) continue;
            const auto& source = impl.source->bindings().channels[channel].raw;
            context.commands->copyBuffer(impl.staging[channel], 0, source.buffer, source.byteOffset, source.byteSize);
        }
    });
}
FRESULT NvrhiVolume::finishReadbackAfterCompletion(PreparedReadback& prepared, ChannelReadback& output) noexcept {
    return guarded([&] {
        auto& impl = *prepared.m_impl;
        require(impl.recorded && !impl.completed);
        ChannelReadback candidate;
        candidate.channelMask = impl.mask;
        for (uint32_t channel = 0; channel < c_MaxChannels; ++channel) {
            if (!impl.staging[channel]) continue;
            auto& bytes = candidate.channels[channel];
            bytes.resize(size_t(impl.source->bindings().channels[channel].storage.logicalBytes));
            const void* mapped = impl.device->mapBuffer(impl.staging[channel], CpuAccessMode::Read);
            require(mapped, FE_GENERIC_ERROR);
            std::memcpy(bytes.data(), mapped, bytes.size());
            impl.device->unmapBuffer(impl.staging[channel]);
        }
        output = std::move(candidate);
        impl.completed = true;
    });
}

// Private builder for trusted operator implementations. Source/destination buffers
// are always authenticated owner allocations; publication stays in recordAndPublish.
struct detail::VolumeMutationBuilder::Impl {
    std::shared_ptr<const VolumeGeneration> source;
    std::unique_ptr<PreparedGeneration::Impl> ticket;
    std::unique_ptr<Allocator> allocator;
    bool work = false;
};
detail::VolumeMutationBuilder::VolumeMutationBuilder(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}
detail::VolumeMutationBuilder::~VolumeMutationBuilder() = default;
IDevice* detail::VolumeMutationBuilder::device(const NvrhiVolume& volume) noexcept { return volume.m_impl->device; }
FRESULT detail::VolumeMutationBuilder::begin(NvrhiVolume& volume, uint32_t writeMask, const PreparationBudget& budget,
                                            std::unique_ptr<VolumeMutationBuilder>& output) noexcept {
    return guarded([&] {
        auto& state = *volume.m_impl;
        require(bool(state.current), FE_NOT_ALIVE_OBJECT);
        require(!state.host.dirty() && sameRevision(state.host.revision(), state.current->m_impl->placement->topology.revision));
        require(state.mask == state.current->bindings().channelMask && state.incarnations == state.current->m_impl->incarnations);
        require(!(writeMask & ~state.mask), FE_NOT_FOUND);
        auto impl = std::make_unique<Impl>();
        impl->source = state.current;
        impl->ticket = std::make_unique<PreparedGeneration::Impl>();
        auto& ticket = *impl->ticket;
        ticket.device = state.device;
        ticket.owner = state.owner;
        ticket.baseSerial = state.serial;
        ticket.channelRevision = state.channelRevision;
        ticket.hostRevision = state.host.revision();
        impl->allocator = std::make_unique<Allocator>(Allocator{state.device, budget, 0, 0, &ticket.uploads});
        impl->work = writeMask != 0 && state.current->bindings().activeLeafCount != 0;
        if (impl->work) {
            auto generation = std::make_unique<VolumeGeneration::Impl>(*state.current->m_impl);
            generation->serial = advance(state.serial);
            // Shared immutable placement avoids both full CPU topology copies and
            // old-generation chains during long sequences of channel-only edits.
            for (uint32_t channel = 0; channel < c_MaxChannels; ++channel) {
                if (!(writeMask & (uint32_t(1) << channel))) continue;
                auto& binding = generation->bindings.channels[channel];
                binding.raw = view(impl->allocator->create(binding.storage.allocatedBytes, 0, true));
                binding.revision = {generation->serial, generation->serial};
            }
            ticket.generation = std::shared_ptr<VolumeGeneration>(new VolumeGeneration(std::move(generation)));
        } else ticket.generation = state.current;
        auto candidate = std::unique_ptr<VolumeMutationBuilder>(new VolumeMutationBuilder(std::move(impl)));
        output = std::move(candidate);
    });
}
const SnapshotBindings& detail::VolumeMutationBuilder::sourceBindings() const noexcept { return m_impl->source->bindings(); }
const SnapshotBindings& detail::VolumeMutationBuilder::destinationBindings() const noexcept {
    return m_impl->ticket->generation->bindings();
}
bool detail::VolumeMutationBuilder::hasWork() const noexcept { return m_impl->work; }
FRESULT detail::VolumeMutationBuilder::allocate(uint64_t bytes, uint32_t stride, bool raw, bool constant,
                                               BufferHandle& output) noexcept {
    return guarded([&] {
        require(m_impl->ticket && m_impl->work, FE_NOT_ALIVE_OBJECT);
        auto candidate = m_impl->allocator->create(bytes, stride, raw, constant);
        output = std::move(candidate);
    });
}
FRESULT detail::VolumeMutationBuilder::upload(const void* data, uint64_t bytes, uint32_t stride, bool raw,
                                             bool constant, BufferHandle& output) noexcept {
    return guarded([&] {
        require(m_impl->ticket && m_impl->work, FE_NOT_ALIVE_OBJECT);
        auto candidate = m_impl->allocator->upload(data, bytes, stride, raw, constant);
        output = std::move(candidate);
    });
}
FRESULT detail::VolumeMutationBuilder::appendDispatch(IComputePipeline* pipeline, IBindingSet* bindings,
                                                     uint32_t x, uint32_t y, uint32_t z) noexcept {
    return guarded([&] {
        require(m_impl->ticket && m_impl->work, FE_NOT_ALIVE_OBJECT);
        require(pipeline && bindings && x && y && z && x <= 65535 && y <= 65535 && z <= 65535);
        m_impl->ticket->dispatches.push_back({pipeline, bindings, x, y, z});
    });
}
FRESULT detail::VolumeMutationBuilder::finish(std::unique_ptr<PreparedGeneration>& output) noexcept {
    return guarded([&] {
        require(bool(m_impl->ticket), FE_NOT_ALIVE_OBJECT);
        require(!m_impl->work || !m_impl->ticket->dispatches.empty());
        // Allocate the facade object before moving the ticket so bad_alloc leaves
        // the builder intact. No-op tickets remain unconsumed until recording.
        auto candidate = std::unique_ptr<PreparedGeneration>(new PreparedGeneration(nullptr));
        candidate->m_impl = std::move(m_impl->ticket);
        output = std::move(candidate);
    });
}

FRESULT detail::VolumeAdoptionBuilder::cloneHostForOwner(NvrhiVolume& volume, const HostVolume& source,
                                                        std::unique_ptr<HostVolume>& output) noexcept {
    return guarded([&] {
        auto& state = *volume.m_impl;
        require(!source.dirty() && !state.host.dirty());
        check(source.validate());
        auto copy = std::make_unique<HostVolume>(state.host.m_limits);
        HostVolume& candidate = *copy;
        candidate.m_config = source.m_config;
        candidate.m_ranges = source.m_ranges;
        candidate.m_root = source.m_root;
        // Clone only live pool records, bounded by the target owner's CPU limits.
        // Source aliases never refer to this prepared candidate's storage.
        for (uint32_t level = 0; level < c_MaxLevels; ++level) {
            require(source.m_nodes[level].size() <= state.host.m_limits.maxPoolBytes / sizeof(Node) &&
                    source.m_children[level].size() <= state.host.m_limits.maxPoolBytes / sizeof(NodeId), FE_OUT_OF_MEMORY);
            candidate.m_nodes[level] = source.m_nodes[level];
            candidate.m_children[level] = source.m_children[level];
            for (auto& node : candidate.m_nodes[level]) node.atlasInterior = {-1,-1,-1};
        }
        candidate.m_revision = {advance(state.host.revision().epoch), state.host.revision().topology, 0};
        check(candidate.finishTopology());
        output = std::move(copy);
    });
}
FRESULT detail::VolumeAdoptionBuilder::describeTopologyPlacement(const HostVolume& host, const AtlasPlacement& placement,
                                                                VolumeInfo& output) noexcept {
    return guarded([&] { auto candidate = place(host, placement); output = candidate.topology.volume; });
}
FRESULT detail::VolumeAdoptionBuilder::prepareTopology(NvrhiVolume& volume, const HostVolume& source,
    const GenerationRequest& request, bool preserveAttachment, std::unique_ptr<PreparedGeneration>& output) noexcept {
    return guarded([&] {
        auto& state = *volume.m_impl;
        auto adopted = std::make_unique<HostAdoption>();
        check(cloneHostForOwner(volume, source, adopted->host));
        adopted->channels = state.channels;
        adopted->channelRevision = state.channelRevision;
        adopted->incarnationSerial = state.incarnationSerial;
        std::unique_ptr<PreparedGeneration> candidate;
        check(volume.prepareGenerationForHost(*adopted->host, request, candidate));
        if (preserveAttachment && state.current)
            candidate->m_impl->generation->m_impl->attachment = state.current->m_impl->attachment;
        candidate->m_impl->adoption = std::move(adopted);
        output = std::move(candidate);
    });
}

FRESULT detail::VolumeAdoptionBuilder::prepareImport(NvrhiVolume& volume, const VolumeImportRequest& request,
                                                    std::unique_ptr<PreparedGeneration>& output) noexcept {
    return guarded([&] {
        auto& state = *volume.m_impl;
        require(request.candidate && !request.candidate->dirty() && !state.host.dirty());
        require(request.channelCount <= c_MaxChannels && (!request.channelCount || request.channels));
        auto adopted = std::make_unique<HostAdoption>();
        check(cloneHostForOwner(volume, *request.candidate, adopted->host));
        HostVolume& candidate = *adopted->host;
        adopted->channelRevision = advance(state.channelRevision);
        adopted->incarnationSerial = state.incarnationSerial;
        auto ticket = std::make_unique<PreparedGeneration::Impl>();
        ticket->device = state.device; ticket->owner = state.owner;
        ticket->baseSerial = state.serial; ticket->channelRevision = state.channelRevision;
        ticket->hostRevision = state.host.revision();
        auto generation = std::make_unique<VolumeGeneration::Impl>();
        generation->owner = state.owner; generation->serial = advance(state.serial);
        generation->attachment = request.attachment;
        auto placed = std::make_shared<Placement>();
        check(candidate.packTopology(placed->topology));
        generation->placement = placed;
        const auto& placement = request.placement;
        require(placement.leafCount == candidate.activeLeafCount() && (!placement.leafCount || placement.leaves));
        constexpr int64_t coordinateLimit = (int64_t(1) << 23) - 1;
        for (uint32_t level = 0; level < placed->topology.levels.size(); ++level) {
            const auto& info = placed->topology.levels[level];
            for (uint32_t i = 0; i < info.nodes.z; ++i) {
                const Node node = readNode(placed->topology, level, i);
                const int64_t origin[] = {node.position.x,node.position.y,node.position.z};
                const int64_t extent[] = {info.nodeExtent.x,info.nodeExtent.y,info.nodeExtent.z};
                const int64_t apron = level == 0 ? placement.apron : 0;
                for (uint32_t axis = 0; axis < 3; ++axis)
                    require(origin[axis] - apron >= -coordinateLimit && origin[axis] + extent[axis] + apron <= coordinateLimit,
                            FE_UNSUPPORTED);
            }
        }
        auto& info = placed->topology.volume;
        if (candidate.activeLeafCount()) {
            const auto grid = placement.brickCount;
            require(grid.x && grid.y && grid.z);
            const uint64_t plane = uint64_t(grid.x) * grid.y;
            require(plane <= UINT32_MAX && grid.z <= UINT32_MAX / plane, FE_UNSUPPORTED);
            const uint64_t slots = plane * grid.z;
            require(slots <= c_AddressSpaceBytes / sizeof(slang::AtlasNode), FE_UNSUPPORTED);
            const uint64_t pitch = uint64_t(candidate.range(0)) + uint64_t(placement.apron) * 2;
            require(pitch && pitch <= INT32_MAX && pitch * grid.x <= INT32_MAX && pitch * grid.y <= INT32_MAX &&
                    pitch * grid.z <= INT32_MAX, FE_UNSUPPORTED);
            const UInt3 resolution{uint32_t(pitch * grid.x), uint32_t(pitch * grid.y), uint32_t(pitch * grid.z)};
            ChannelStorage checked; check(describeChannel(AtlasFormat::R8_UINT, resolution, checked).result);
            info.atlasResolutionAndApron = {resolution.x,resolution.y,resolution.z,placement.apron};
            info.atlasBrickCountAndWidth = {grid.x,grid.y,grid.z,uint32_t(pitch)};
            placed->map.assign(size_t(slots), {0,0,0,-1});
            std::vector<uint8_t> seen(size_t(candidate.count(0,0)),0);
            for (size_t i = 0; i < placement.leafCount; ++i) {
                const auto mapping = placement.leaves[i];
                require(mapping.leafIndex < seen.size() && mapping.leafIndex <= INT32_MAX && mapping.slot < slots &&
                    !seen[mapping.leafIndex] && placed->map[mapping.slot].leaf < 0);
                const Node leaf = readNode(placed->topology,0,mapping.leafIndex);
                require(leaf.flags != 0);
                seen[mapping.leafIndex] = 1;
                const uint32_t x = mapping.slot % grid.x, y = (mapping.slot / grid.x) % grid.y,
                               z = mapping.slot / uint32_t(plane);
                setInterior(placed->topology,mapping.leafIndex,{int32_t(x*pitch+placement.apron),
                    int32_t(y*pitch+placement.apron),int32_t(z*pitch+placement.apron)});
                placed->map[mapping.slot] = {leaf.position.x,leaf.position.y,leaf.position.z,int32_t(mapping.leafIndex)};
            }
            // Equal counts plus unique active indices prove complete active coverage.
        } else {
            // An empty owner has no live atlas. Never silently discard a nonempty
            // imported allocation merely because its topology has no active leaves.
            require(!(placement.brickCount.x || placement.brickCount.y || placement.brickCount.z) && !placement.apron);
            info.atlasResolutionAndApron = {}; info.atlasBrickCountAndWidth = {};
        }
        auto& bindings = generation->bindings;
        bindings.device = state.device; bindings.volume = info;
        bindings.activeLeafCount = candidate.activeLeafCount();
        bindings.revision = candidate.revision();
        bindings.revision.placement = state.current ? advance(state.current->bindings().revision.placement) : 1;
        std::copy(placed->topology.levels.begin(),placed->topology.levels.end(),bindings.levels.begin());
        const UInt3 resolution{info.atlasResolutionAndApron.x,info.atlasResolutionAndApron.y,info.atlasResolutionAndApron.z};
        for (size_t i = 0; i < request.channelCount; ++i) {
            const auto& input = request.channels[i];
            require(input.channel < c_MaxChannels && !(bindings.channelMask & (uint32_t(1) << input.channel)));
            check(validateChannelDesc(input.desc).result);
            auto& channel = bindings.channels[input.channel];
            channel.desc = input.desc; check(describeChannel(input.desc.format,resolution,channel.storage).result);
            require(input.byteCount == channel.storage.logicalBytes && (!input.byteCount || input.data));
            channel.revision = {generation->serial,generation->serial};
            adopted->channels[input.channel] = input.desc;
            adopted->incarnationSerial = advance(adopted->incarnationSerial);
            generation->incarnations[input.channel] = adopted->incarnationSerial;
            bindings.channelMask |= uint32_t(1) << input.channel;
        }
        Allocator allocator{state.device,request.budget,0,0,&ticket->uploads};
        if (!placed->topology.nodes.empty()) bindings.nodes = view(allocator.upload(placed->topology.nodes.data(),
            placed->topology.nodes.size()*4ull,0,true));
        if (!placed->topology.children.empty()) bindings.children = view(allocator.upload(placed->topology.children.data(),
            placed->topology.children.size()*4ull,0,true));
        bindings.levelMetadata = view(allocator.upload(placed->topology.levels.data(),
            placed->topology.levels.size()*sizeof(LevelInfo),sizeof(LevelInfo),false));
        if (!placed->map.empty()) bindings.atlasMap = view(allocator.upload(placed->map.data(),
            placed->map.size()*sizeof(slang::AtlasNode),sizeof(slang::AtlasNode),false));
        for (size_t i = 0; i < request.channelCount; ++i) {
            const auto& input = request.channels[i];
            if (input.byteCount) bindings.channels[input.channel].raw = view(allocator.upload(input.data,input.byteCount,0,true));
        }
        ticket->generation = std::shared_ptr<VolumeGeneration>(new VolumeGeneration(std::move(generation)));
        ticket->adoption = std::move(adopted);
        auto candidateTicket = std::unique_ptr<PreparedGeneration>(new PreparedGeneration(std::move(ticket)));
        output = std::move(candidateTicket);
    });
}
FRESULT detail::VolumeAdoptionBuilder::exportTopology(const VolumeGeneration& generation, slang::PackedVolume& output) noexcept {
    return guarded([&] {
        const auto& placement = *generation.m_impl->placement;
        slang::PackedVolume candidate;
        candidate.volume = placement.topology.volume;
        candidate.levels = placement.topology.levels;
        candidate.nodes = placement.topology.nodes;
        candidate.children = placement.topology.children;
        candidate.atlasMap = placement.map;
        output = std::move(candidate);
    });
}
IRHIObject* detail::VolumeAdoptionBuilder::attachment(const VolumeGeneration& generation) noexcept {
    return generation.m_impl->attachment.Get();
}
FRESULT detail::VolumeAdoptionBuilder::validateGeneration(const NvrhiVolume& volume, const VolumeGeneration& generation) noexcept {
    return volume.m_impl->owner == generation.m_impl->owner ? FS_OK : FE_INVALID_ARGS;
}
} // namespace gvdb::portable
