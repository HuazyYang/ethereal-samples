// SPDX-License-Identifier: Apache-2.0
#include "GVDBNvrhiApi.h"
#include "GVDBNvrhiVolume.h"
#include <nvrhi/core/foundation.h>
#include <new>
#include <stdexcept>

namespace gvdb::portable {
namespace {
using namespace nvrhi;
struct Failure { FRESULT result; };
void require(bool condition, FRESULT result = FE_INVALID_ARGS) { if (!condition) throw Failure{result}; }
void check(FRESULT result) { require(NVRHI_SUCCEEDED(result), result); }
template<class F> FRESULT guarded(F&& function) noexcept {
    try { return function(); }
    catch (const Failure& failure) { return failure.result; }
    catch (const std::bad_alloc&) { return FE_OUT_OF_MEMORY; }
    catch (const std::length_error&) { return FE_OUT_OF_MEMORY; }
    catch (...) { return FE_GENERIC_ERROR; }
}
CellExtents extents(const HostExtents& value) {
    return {value.level,value.cellWidth,value.minimum,value.maximum,value.resolution,value.count};
}
AtlasPlacement placement(const PlacementDesc& value) {
    require(value.preserve <= 1);
    return {value.minimumBrickCount,value.apron,value.preserve != 0};
}
BorrowedBufferBinding borrowed(const BufferView& value) {
    return {value.buffer.Get(),value.byteOffset,value.byteSize};
}
template<class T, class I> AutoPtr<T> implementation(I* object) {
    require(object != nullptr);
    AutoPtr<T> result;
    require(NVRHI_SUCCEEDED(object->QueryInterface(uuid_of<T>(), reinterpret_cast<void**>(result.GetAddressOf()))) && result.Get());
    return result;
}

NVRHI_CLASS_CLSID(Snapshot, "d6349407-141b-4431-adf2-62d271705ea3")
class Snapshot final : public ObjectImpl<IGVDBSnapshot> {
 public:
    NVRHI_DECLARE_UUID_TRAITS(Snapshot)
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Snapshot)
        NVRHI_IMPLEMENTS_INTERFACE(IGVDBSnapshot)
        NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(Snapshot)
    NVRHI_END_INTERFACE_TABLE()
    std::shared_ptr<const VolumeGeneration> generation;
    explicit Snapshot(std::shared_ptr<const VolumeGeneration> value) noexcept : generation(std::move(value)) {}
    IDevice* getDevice() const noexcept override { return generation->bindings().device.Get(); }
    SnapshotDesc getDesc() const noexcept override {
        const auto& bindings = generation->bindings();
        SnapshotDesc result;
        result.volume = bindings.volume; result.revision = bindings.revision;
        result.activeLeafCount = bindings.activeLeafCount; result.levelCount = bindings.volume.hierarchy.z;
        result.channelMask = bindings.channelMask;
        return result;
    }
    FRESULT getLevel(uint32_t level, LevelInfo& output) const noexcept override {
        output = {};
        const auto& bindings = generation->bindings();
        if (level >= bindings.volume.hierarchy.z || level >= c_MaxLevels) return FE_INVALID_ARGS;
        output = bindings.levels[level]; return FS_OK;
    }
    FRESULT getTopologyBuffer(TopologyBuffer type, BorrowedBufferBinding& output) const noexcept override {
        output = {};
        const auto& bindings = generation->bindings();
        switch (type) {
        case TopologyBuffer::Nodes: output = borrowed(bindings.nodes); break;
        case TopologyBuffer::Children: output = borrowed(bindings.children); break;
        case TopologyBuffer::Levels: output = borrowed(bindings.levelMetadata); break;
        case TopologyBuffer::AtlasMap: output = borrowed(bindings.atlasMap); break;
        default: return FE_INVALID_ARGS;
        }
        return FS_OK;
    }
    FRESULT getChannel(uint32_t channel, ChannelSnapshotDesc& output) const noexcept override {
        output = {};
        if (channel >= c_MaxChannels) return FE_INVALID_ARGS;
        const auto& bindings = generation->bindings();
        if (!(bindings.channelMask & (uint32_t(1) << channel))) return FE_NOT_FOUND;
        const auto& value = bindings.channels[channel];
        output = {value.desc,value.storage,value.revision,borrowed(value.raw)};
        return FS_OK;
    }
};
NVRHI_CLASS_CLSID(GenerationTicket, "ebc4d0d5-173b-464c-885a-74de7393d701")
class GenerationTicket final : public ObjectImpl<IGVDBGenerationTicket> {
    IDevice* m_device; // Borrowed from the retained underlying preparation.
 public:
    NVRHI_DECLARE_UUID_TRAITS(GenerationTicket)
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(GenerationTicket)
        NVRHI_IMPLEMENTS_INTERFACE(IGVDBGenerationTicket)
        NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(GenerationTicket)
    NVRHI_END_INTERFACE_TABLE()
    std::unique_ptr<PreparedGeneration> prepared;
    GenerationTicket(IDevice* device, std::unique_ptr<PreparedGeneration> value) noexcept
        : m_device(device), prepared(std::move(value)) {}
    IDevice* getDevice() const noexcept override { return m_device; }
};
NVRHI_CLASS_CLSID(Readback, "b016d63f-4c5a-4964-b288-c7219ef0bbab")
class Readback final : public ObjectImpl<IGVDBReadback> {
    IDevice* m_device; // PreparedReadback retains it until this wrapper is destroyed.
    uint32_t m_mask;
    ChannelReadback m_data;
    bool m_completed = false;
 public:
    NVRHI_DECLARE_UUID_TRAITS(Readback)
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Readback)
        NVRHI_IMPLEMENTS_INTERFACE(IGVDBReadback)
        NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(Readback)
    NVRHI_END_INTERFACE_TABLE()
    std::unique_ptr<PreparedReadback> prepared;
    Readback(IDevice* device, uint32_t mask, std::unique_ptr<PreparedReadback> value) noexcept
        : m_device(device), m_mask(mask), prepared(std::move(value)) {}
    IDevice* getDevice() const noexcept override { return m_device; }
    uint32_t getChannelMask() const noexcept override { return m_mask; }
    FRESULT finishAfterCallerCompletion() noexcept override {
        const auto result = NvrhiVolume::finishReadbackAfterCompletion(*prepared, m_data);
        if (NVRHI_SUCCEEDED(result)) m_completed = true;
        return result;
    }
    FRESULT getChannelData(uint32_t channel, BorrowedBytes& output) const noexcept override {
        output = {};
        if (channel >= c_MaxChannels) return FE_INVALID_ARGS;
        if (!m_completed) return FE_NOT_ALIVE_OBJECT;
        if (!(m_data.channelMask & (uint32_t(1) << channel))) return FE_NOT_FOUND;
        const auto& data = m_data.channels[channel];
        output = {data.empty() ? nullptr : data.data(),data.size()};
        return FS_OK;
    }
};
class Volume final : public ObjectImpl<IGVDBVolume> {
    IDevice* m_device; // The composed NvrhiVolume owns the device reference.
    std::unique_ptr<NvrhiVolume> m_volume;
    HostVolume& host() noexcept { return m_volume->host(); }
    const HostVolume& host() const noexcept { return static_cast<const NvrhiVolume&>(*m_volume).host(); }
 public:
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Volume)
        NVRHI_IMPLEMENTS_INTERFACE(IGVDBVolume)
        NVRHI_IMPLEMENTS_INTERFACE(IGVDBHostVolume)
        NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    Volume(IDevice* device, std::unique_ptr<NvrhiVolume> value) noexcept : m_device(device), m_volume(std::move(value)) {}
    IDevice* getDevice() const noexcept override { return m_device; }
    FRESULT configure(const LevelConfig& config) noexcept override { return host().configure(config); }
    FRESULT clearTopology() noexcept override { return host().clear(); }
    const LevelConfig& getConfiguration() const noexcept override { return host().configuration(); }
    TopologyRevision getTopologyRevision() const noexcept override { return host().revision(); }
    bool isTopologyDirty() const noexcept override { return host().dirty(); }
    NodeId getRootId() const noexcept override { return host().root(); }
    uint32_t getRange(int32_t level) const noexcept override { return host().range(level); }
    uint32_t getWidth(uint32_t level) const noexcept override { return host().width(level); }
    uint64_t getPoolCount(uint32_t group, uint32_t level) const noexcept override { return host().count(group,level); }
    uint64_t getPoolCapacity(uint32_t group, uint32_t level) const noexcept override { return host().capacity(group,level); }
    uint32_t getPoolStride(uint32_t group, uint32_t level) const noexcept override { return host().stride(group,level); }
    HostPoolView getPool(uint32_t group, uint32_t level) const noexcept override { return host().pool(group,level); }
    MutableHostPoolView editPool(uint32_t group, uint32_t level) noexcept override { return host().editPool(group,level); }
    const Node* getNode(NodeId id) const noexcept override { return host().node(id); }
    Node* editNode(NodeId id) noexcept override { return host().editNode(id); }
    const NodeId* getChildList(NodeId id) const noexcept override { return host().childList(id); }
    NodeId* editChildList(NodeId id) noexcept override { return host().editChildList(id); }
    FRESULT reservePool(uint32_t group, uint32_t level, uint64_t elements) noexcept override { return host().reserve(group,level,elements); }
    FRESULT allocateNode(uint32_t level, Int3 origin, bool active, NodeId& output) noexcept override { return host().allocateNode(level,origin,active,output); }
    FRESULT allocateChildList(uint32_t level, NodeId& output) noexcept override { return host().allocateChildList(level,output); }
    FRESULT setRoot(NodeId id) noexcept override { return host().setRoot(id); }
    FRESULT setActive(NodeId id, bool active) noexcept override { return host().setActive(id,active); }
    FRESULT activate(double x, double y, double z, NodeId& output) noexcept override { return host().activate(x,y,z,output); }
    FRESULT activateAtLevel(uint32_t level, Int3 position, NodeId& output) noexcept override { return host().activateAtLevel(level,position,output); }
    NodeId findLeaf(Int3 position) const noexcept override { return host().findLeaf(position); }
    bool isAllocated(Int3 position) const noexcept override { return host().isAllocated(position); }
    bool isActive(Int3 position) const noexcept override { return host().isActive(position); }
    FRESULT computeExtents(uint32_t level, Float4 minimum, Float4 maximum, HostExtents& output) const noexcept override {
        output = {};
        CellExtents result;
        const auto status = host().computeExtents(level,minimum,maximum,result);
        if (NVRHI_SUCCEEDED(status)) output = {result.level,result.cellWidth,result.minimum,result.maximum,result.resolution,result.count};
        return status;
    }
    FRESULT activateRegion(const HostExtents& value, uint64_t& selected) noexcept override { return host().activateRegion(extents(value),selected); }
    FRESULT activateRegionFromMasks(const HostExtents& value, const uint8_t* data, uint64_t count, uint64_t& selected) noexcept override {
        return host().activateRegionFromMasks(extents(value),data,count,selected);
    }
    FRESULT activateRegionFromValues(const HostExtents& value, const float* data, uint64_t count, float threshold, uint64_t& selected) noexcept override {
        return host().activateRegionFromValues(extents(value),data,count,threshold,selected);
    }
    FRESULT activateHalo(const HostExtents& value, uint64_t& selected) noexcept override { return host().activateHalo(extents(value),selected); }
    FRESULT validateTopology() const noexcept override { return host().validate(); }
    FRESULT finishTopology() noexcept override { return host().finishTopology(); }
    uint32_t getActiveLeafCount() const noexcept override { return host().activeLeafCount(); }
    HostBounds getBounds() const noexcept override {
        const auto& value = host().bounds(); return {value.lower,value.upper,uint32_t(value.empty)};
    }
    FRESULT setChannel(uint32_t channel, const ChannelDesc& desc) noexcept override { return m_volume->setChannel(channel,desc); }
    FRESULT removeChannel(uint32_t channel) noexcept override { return m_volume->removeChannel(channel); }
    FRESULT describePlacement(const PlacementDesc& desc, VolumeInfo& output) const noexcept override {
        return guarded([&] { return m_volume->describePlacement(placement(desc),output); });
    }
    FRESULT prepareGeneration(const GenerationDesc& desc, IGVDBGenerationTicket** output) noexcept override {
        if (!output) return FE_INVALID_ARGS;
        *output = nullptr;
        return guarded([&] {
            require(!desc.reserved && !desc.reserved2 && desc.uploadCount <= c_MaxChannels);
            require(desc.uploadCount == 0 || desc.uploads);
            GenerationRequest request;
            request.placement = placement(desc.placement);
            request.budget = {desc.maxAllocatedBytes,desc.maxBuffers};
            std::vector<ChannelUpload> uploads;
            uploads.reserve(size_t(desc.uploadCount));
            for (uint64_t i = 0; i < desc.uploadCount; ++i) {
                const auto& upload = desc.uploads[i];
                require(!upload.reserved);
                uploads.push_back({upload.channel,upload.data,upload.byteCount});
            }
            request.uploads = uploads.data(); request.uploadCount = uploads.size();
            std::unique_ptr<PreparedGeneration> prepared;
            check(m_volume->prepareGeneration(request,prepared));
            auto result = MAKE_RC_OBJ_PTR(GenerationTicket,m_device,std::move(prepared));
            *output = result.Detach(); return FS_OK;
        });
    }
    FRESULT recordAndPublish(RecordingContext context, IGVDBGenerationTicket* ticket) noexcept override {
        return guarded([&] {
            const auto impl = implementation<GenerationTicket>(ticket);
            return m_volume->recordAndPublish(context,*impl->prepared);
        });
    }
    FRESULT getSnapshot(IGVDBSnapshot** output) const noexcept override {
        if (!output) return FE_INVALID_ARGS;
        *output = nullptr;
        return guarded([&] {
            auto generation = m_volume->snapshot();
            require(bool(generation),FE_NOT_FOUND);
            auto result = MAKE_RC_OBJ_PTR(Snapshot,std::move(generation));
            *output = result.Detach(); return FS_OK;
        });
    }
    FRESULT prepareReadback(IGVDBSnapshot* snapshot, uint32_t mask, IGVDBReadback** output) noexcept override {
        if (!output) return FE_INVALID_ARGS;
        *output = nullptr;
        return guarded([&] {
            const auto impl = implementation<Snapshot>(snapshot);
            std::unique_ptr<PreparedReadback> prepared;
            check(m_volume->prepareReadback(impl->generation,mask,prepared));
            auto result = MAKE_RC_OBJ_PTR(Readback,m_device,mask,std::move(prepared));
            *output = result.Detach(); return FS_OK;
        });
    }
    FRESULT recordReadback(RecordingContext context, IGVDBReadback* readback) noexcept override {
        return guarded([&] {
            const auto impl = implementation<Readback>(readback);
            return m_volume->recordReadback(context,*impl->prepared);
        });
    }
};
}
uint32_t gvdbGetNvrhiApiVersion() noexcept { return c_GVDBNvrhiApiVersion; }
nvrhi::FRESULT gvdbCreateNvrhiVolume(uint32_t apiVersion, const VolumeCreateDesc* desc, IGVDBVolume** output) noexcept {
    if (!output) return nvrhi::FE_INVALID_ARGS;
    *output = nullptr;
    return guarded([&] {
        require(apiVersion == c_GVDBNvrhiApiVersion,nvrhi::FE_UNSUPPORTED);
        require(desc && desc->structSize == sizeof(VolumeCreateDesc));
        require(!desc->reserved && desc->remapArtifactBytes <= SIZE_MAX);
        VolumeOwnerDesc owner;
        owner.device = desc->device; owner.configuration = desc->configuration;
        owner.hostLimits.maxPoolBytes = desc->maxHostPoolBytes;
        owner.remapArtifact = desc->remapArtifact; owner.remapArtifactBytes = size_t(desc->remapArtifactBytes);
        std::unique_ptr<NvrhiVolume> volume;
        check(NvrhiVolume::create(owner,volume));
        auto result = MAKE_RC_OBJ_PTR(Volume,desc->device,std::move(volume));
        *output = result.Detach(); return nvrhi::FS_OK;
    });
}
} // namespace gvdb::portable
