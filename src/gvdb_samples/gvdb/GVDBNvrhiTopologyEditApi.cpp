// SPDX-License-Identifier: Apache-2.0
#include "GVDBNvrhiTopologyEditApi.h"
#include "GVDBNvrhiApiInternal.h"
#include "GVDBNvrhiAdoptionInternal.h"
#include "GVDBHostImport.h"
#include <nvrhi/core/foundation.h>
#include <cstring>
#include <memory>
#include <stdexcept>
namespace gvdb::portable {
namespace {
using namespace nvrhi;
struct Failure {
    FRESULT result;
};
void require(bool value, FRESULT result = FE_INVALID_ARGS) {
    if (!value)
        throw Failure{result};
}
void check(FRESULT result) { require(NVRHI_SUCCEEDED(result), result); }
template <class F>
FRESULT guarded(F&& fn) noexcept {
    try {
        fn();
        return FS_OK;
    } catch (const Failure& e) {
        return e.result;
    } catch (const std::bad_alloc&) {
        return FE_OUT_OF_MEMORY;
    } catch (const std::length_error&) {
        return FE_OUT_OF_MEMORY;
    } catch (...) {
        return FE_GENERIC_ERROR;
    }
}
struct Identity {};
NVRHI_CLASS_CLSID(TopologyEditTicket, "82e3eb14-2eaa-45a9-b2be-d9ecc371569f")
class TopologyEditTicket final : public ObjectImpl<IGVDBTopologyEditTicket> {
 public:
    DeviceHandle device;
    VolumeHandle owner;
    SnapshotHandle source;
    std::shared_ptr<const VolumeGeneration> generation;
    std::shared_ptr<Identity> identity;
    std::unique_ptr<detail::AccumulationCandidate> baseline;
    TopologyEditDesc options;
    BufferHandle nodes, children, nodeStaging, childStaging;
    uint64_t nodeBytes = 0, childBytes = 0;
    uint32_t phase = 0;  // prepared, editing, fetch recorded, completed
    NVRHI_DECLARE_UUID_TRAITS(TopologyEditTicket)
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(TopologyEditTicket)
    NVRHI_IMPLEMENTS_INTERFACE(IGVDBTopologyEditTicket)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_IMPLEMENTS_CLASS(TopologyEditTicket)
    NVRHI_END_INTERFACE_TABLE()
    IDevice* getDevice() const noexcept override { return device.Get(); }
    SnapshotDesc getSourceDesc() const noexcept override { return source->getDesc(); }
    FRESULT getLevel(uint32_t level, LevelInfo& output) const noexcept override {
        return source->getLevel(level, output);
    }
    FRESULT getTopologyBuffer(TopologyBuffer which, BorrowedBufferBinding& output) const noexcept override {
        if (phase != 1)
            return FE_INVALID_ARGS;
        if (which == TopologyBuffer::Levels)
            return source->getTopologyBuffer(which, output);
        if (which == TopologyBuffer::Nodes)
            output = {nodes.Get(), 0, nodeBytes};
        else if (which == TopologyBuffer::Children)
            output = {children.Get(), 0, childBytes};
        else
            return FE_UNSUPPORTED;
        return FS_OK;
    }
    FRESULT finishAfterCallerCompletion(IGVDBGenerationTicket** output) noexcept override {
        if (!output)
            return FE_INVALID_ARGS;
        *output = nullptr;
        return guarded([&] {
            require(phase == 2);
            NvrhiVolume* core = nullptr;
            check(detail::resolveVolume(owner, &core));
            check(detail::VolumeAdoptionBuilder::validateAccumulationBase(*core, *baseline));
            PackedHostTopology packed;
            const auto& bindings = generation->bindings();
            packed.volume = bindings.volume;
            packed.levels.assign(bindings.levels.begin(), bindings.levels.begin() + packed.volume.hierarchy.z);
            packed.nodes.resize(size_t(nodeBytes / 4));
            packed.children.resize(size_t(childBytes / 4));
            auto read = [&](IBuffer* buffer, std::vector<uint32_t>& data) {
                if (data.empty())
                    return;
                const void* mapped = device->mapBuffer(buffer, CpuAccessMode::Read);
                require(mapped, FE_GENERIC_ERROR);
                std::memcpy(data.data(), mapped, data.size() * 4);
                device->unmapBuffer(buffer);
            };
            read(nodeStaging, packed.nodes);
            read(childStaging, packed.children);
            std::unique_ptr<HostVolume> candidate;
            check(importHostTopology(packed, candidate, detail::VolumeAdoptionBuilder::hostLimits(*core)));
            GenerationRequest request;
            request.placement = {options.placement.minimumBrickCount, options.placement.apron,
                                 options.placement.preserve != 0};
            request.budget = {options.maxPublicationBytes, options.maxPublicationBuffers};
            std::unique_ptr<PreparedGeneration> publication;
            check(detail::VolumeAdoptionBuilder::prepareFetchedTopology(
                *core, *baseline, *candidate, request, options.preserveMetadata != 0, publication));
            check(detail::wrapGeneration(device, std::move(publication), output));
            phase = 3;
        });
    }
};
class TopologyEdit final : public ObjectImpl<IGVDBTopologyEdit> {
 public:
    VolumeHandle owner;
    DeviceHandle device;
    std::shared_ptr<Identity> identity = std::make_shared<Identity>();
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(TopologyEdit)
    NVRHI_IMPLEMENTS_INTERFACE(IGVDBTopologyEdit)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    IGVDBVolume* getVolume() const noexcept override { return owner.Get(); }
    FRESULT prepare(const TopologyEditDesc& d, IGVDBTopologyEditTicket** output) noexcept override {
        if (!output)
            return FE_INVALID_ARGS;
        *output = nullptr;
        return guarded([&] {
            require(d.structSize == sizeof(d) && !d.reserved && d.preserveMetadata <= 1 &&
                    d.placement.preserve <= 1);
            NvrhiVolume* core = nullptr;
            check(detail::resolveVolume(owner, &core));
            auto t = MAKE_RC_OBJ_PTR(TopologyEditTicket);
            t->device = device;
            t->owner = owner;
            t->identity = identity;
            t->options = d;
            check(owner->getSnapshot(&t->source));
            require(bool(t->source));
            check(detail::resolveSnapshot(t->source, t->generation));
            const auto& binding = t->generation->bindings();
            const auto revision = core->host().revision();
            require(!core->host().dirty() && revision.epoch == binding.revision.epoch &&
                    revision.topology == binding.revision.topology);
            t->nodeBytes = binding.nodes.byteSize;
            t->childBytes = binding.children.byteSize;
            const uint64_t bytes = t->nodeBytes + t->childBytes;
            const uint32_t buffers = (t->nodeBytes ? 2u : 0u) + (t->childBytes ? 2u : 0u);
            require(bytes <= d.maxReadbackBytes && bytes * 2 <= d.maxAllocatedBytes && buffers <= d.maxBuffers,
                    FE_OUT_OF_MEMORY);
            // Validate placement before allocating private copies. The eventual
            // fetched graph is validated again before actual publication planning.
            VolumeInfo ignored;
            check(core->describePlacement(
                {d.placement.minimumBrickCount, d.placement.apron, d.placement.preserve != 0}, ignored));
            check(detail::VolumeAdoptionBuilder::beginAccumulation(*core, t->baseline));
            auto allocate = [&](uint64_t count, bool staging) {
                BufferHandle buffer;
                if (!count)
                    return buffer;
                BufferDesc desc;
                desc.byteSize = count;
                desc.canHaveRawViews = !staging;
                desc.canHaveUAVs = !staging;
                desc.cpuAccess = staging ? CpuAccessMode::Read : CpuAccessMode::None;
                desc.initialState = ResourceStates::CopyDest;
                desc.keepInitialState = true;
                desc.debugName = staging ? "GVDB topology fetch staging" : "GVDB private topology edit";
                check(device->createBuffer(desc, &buffer));
                require(bool(buffer), FE_OUT_OF_MEMORY);
                return buffer;
            };
            t->nodes = allocate(t->nodeBytes, false);
            t->children = allocate(t->childBytes, false);
            t->nodeStaging = allocate(t->nodeBytes, true);
            t->childStaging = allocate(t->childBytes, true);
            *output = t.Detach();
        });
    }
    FRESULT record(RecordingContext context, IGVDBTopologyEditTicket* ticket, uint32_t phase) noexcept {
        return guarded([&] {
            require(ticket != nullptr);
            AutoPtr<TopologyEditTicket> concrete;
            require(NVRHI_SUCCEEDED(ticket->QueryInterface(
                        uuid_of<TopologyEditTicket>(), reinterpret_cast<void**>(concrete.GetAddressOf()))) &&
                    concrete.Get());
            auto& t = *concrete;
            require(t.identity == identity && t.phase == phase);
            require(context.device == device.Get() && context.commands &&
                    context.commands->getDevice() == context.device);
            require(context.commands->getDesc().queueType != CommandQueue::Copy, FE_UNSUPPORTED);
            NvrhiVolume* core = nullptr;
            check(detail::resolveVolume(owner, &core));
            check(detail::VolumeAdoptionBuilder::validateAccumulationBase(*core, *t.baseline));
            ++t.phase;
            const auto& source = t.generation->bindings();
            if (phase == 0) {
                if (t.nodeBytes)
                    context.commands->copyBuffer(t.nodes, 0, source.nodes.buffer, source.nodes.byteOffset,
                                                 t.nodeBytes);
                if (t.childBytes)
                    context.commands->copyBuffer(t.children, 0, source.children.buffer,
                                                 source.children.byteOffset, t.childBytes);
            } else {
                if (t.nodeBytes)
                    context.commands->copyBuffer(t.nodeStaging, 0, t.nodes, 0, t.nodeBytes);
                if (t.childBytes)
                    context.commands->copyBuffer(t.childStaging, 0, t.children, 0, t.childBytes);
            }
        });
    }
    FRESULT recordBegin(RecordingContext c, IGVDBTopologyEditTicket* t) noexcept override {
        return record(c, t, 0);
    }
    FRESULT recordFetch(RecordingContext c, IGVDBTopologyEditTicket* t) noexcept override {
        return record(c, t, 1);
    }
};
}  // namespace
FRESULT gvdbCreateNvrhiTopologyEdit(uint32_t version, const TopologyEditCreateDesc* desc,
                                    IGVDBTopologyEdit** output) noexcept {
    if (!output)
        return FE_INVALID_ARGS;
    *output = nullptr;
    return guarded([&] {
        require(version == c_GVDBNvrhiApiVersion, FE_UNSUPPORTED);
        require(desc && desc->structSize == sizeof(*desc) && !desc->reserved && desc->volume);
        NvrhiVolume* core = nullptr;
        check(detail::resolveVolume(desc->volume, &core));
        auto result = MAKE_RC_OBJ_PTR(TopologyEdit);
        result->owner = desc->volume;
        result->device = desc->volume->getDevice();
        *output = result.Detach();
    });
}
}  // namespace gvdb::portable
