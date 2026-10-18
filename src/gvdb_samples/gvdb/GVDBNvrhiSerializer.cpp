// SPDX-License-Identifier: Apache-2.0
#include "GVDBNvrhiSerializer.h"
#include "GVDBNvrhiApiInternal.h"
#include "GVDBNvrhiAdoptionInternal.h"
#include "GVDBHostImport.h"
#include "slang/GVDBPortableVbx.h"
#include <nvrhi/core/foundation.h>
#include <nvrhi/core/datablob.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include <stdexcept>
namespace gvdb::portable {
namespace {
using namespace nvrhi;
using detail::VolumeAdoptionBuilder;
struct Failure { FRESULT result; const char* message; };
void require(bool value, FRESULT result = FE_INVALID_ARGS, const char* message = "Invalid serializer argument.") {
    if (!value) throw Failure{result,message};
}
void check(FRESULT result, const char* message) { require(NVRHI_SUCCEEDED(result),result,message); }
void diagnosticBlob(IDataBlob** output, const char* text) noexcept {
    if (!output || !text) return;
    const size_t size = std::strlen(text);
    AutoPtr<IDataBlob> blob;
    if (NVRHI_SUCCEEDED(CreateStringBlob(size,blob.GetAddressOf()))) {
        if (size) std::memcpy(blob->GetDataPtr(),text,size);
        *output = blob.Detach();
    }
}
template<class F> FRESULT guarded(IDataBlob** diagnostic, F&& function) noexcept {
    if (diagnostic) *diagnostic = nullptr;
    try { return function(); }
    catch (const Failure& failure) { diagnosticBlob(diagnostic,failure.message); return failure.result; }
    catch (const std::bad_alloc&) { diagnosticBlob(diagnostic,"Serializer allocation failed."); return FE_OUT_OF_MEMORY; }
    catch (const std::length_error&) { diagnosticBlob(diagnostic,"Serializer allocation exceeds addressable size."); return FE_OUT_OF_MEMORY; }
    catch (...) { diagnosticBlob(diagnostic,"Unexpected serializer failure."); return FE_GENERIC_ERROR; }
}
template<class T, class I> AutoPtr<T> implementation(I* object) {
    require(object != nullptr);
    AutoPtr<T> result;
    require(NVRHI_SUCCEEDED(object->QueryInterface(uuid_of<T>(),reinterpret_cast<void**>(result.GetAddressOf()))) && result.Get());
    return result;
}
bool finite(VbxVector3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
void validateMetadata(const VbxMetadata& value) {
    require(finite(value.preTranslation) && finite(value.angles) && finite(value.scale) && finite(value.translation)
        && finite(value.voxelSize) && value.voxelSize.x > 0 && value.voxelSize.y > 0 && value.voxelSize.z > 0,
        FE_INVALID_ARGS,"VBX transforms must be finite and voxel sizes positive.");
}
VbxVector3 vector(slang::VbxFloat3 value) { return {value.x,value.y,value.z}; }
slang::VbxFloat3 vector(VbxVector3 value) { return {value.x,value.y,value.z}; }
VbxMetadata metadata(const slang::VbxRawVolume& source) {
    VbxMetadata result;
    result.preTranslation = vector(source.transform.preTranslation); result.angles = vector(source.transform.angles);
    result.scale = vector(source.transform.scale); result.translation = vector(source.transform.translation);
    result.voxelSize = vector(source.voxelSize);
    std::memcpy(result.gridName,source.gridName.data(),sizeof(result.gridName));
    return result;
}
void applyMetadata(slang::VbxRawVolume& destination, const VbxMetadata& source) {
    validateMetadata(source);
    destination.transform = {vector(source.preTranslation),vector(source.angles),vector(source.scale),vector(source.translation)};
    destination.voxelSize = vector(source.voxelSize);
    std::memcpy(destination.gridName.data(),source.gridName,sizeof(source.gridName));
}
NVRHI_CLASS_CLSID(Metadata, "3df3271f-d23e-4c4f-8568-a26d8998875b")
class Metadata final : public ObjectImpl<IRHIObject> {
 public:
    NVRHI_DECLARE_UUID_TRAITS(Metadata)
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Metadata)
        NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(Metadata)
    NVRHI_END_INTERFACE_TABLE()
    const VbxMetadata value;
    explicit Metadata(const VbxMetadata& data) noexcept : value(data) {}
};
VbxMetadata metadata(const VolumeGeneration& generation) {
    auto* attachment = VolumeAdoptionBuilder::attachment(generation);
    if (!attachment) return {};
    return implementation<Metadata>(attachment)->value;
}
uint64_t add(uint64_t value, uint64_t amount) {
    require(amount <= UINT64_MAX-value,FE_OUT_OF_MEMORY,"Serialized VBX size overflows.");
    return value+amount;
}
uint64_t serializedBytes(const slang::PackedVolume& topology, const SnapshotBindings& bindings) {
    uint64_t bytes = 62+333+12;
    for (const auto& level : topology.levels) {
        bytes = add(bytes,36);
        bytes = add(bytes,uint64_t(level.nodes.y)*level.nodes.z);
        bytes = add(bytes,uint64_t(level.children.y)*level.children.z);
    }
    for (uint32_t index=0; index<c_MaxChannels; ++index) if (bindings.channelMask & (uint32_t(1)<<index))
        bytes = add(add(bytes,8),bindings.channels[index].storage.logicalBytes);
    return bytes;
}
NVRHI_CLASS_CLSID(SaveTicket, "f5a7cc31-4a22-4bed-b074-11838f29f263")
class SaveTicket final : public ObjectImpl<IGVDBSaveTicket> {
    DeviceHandle m_device;
    slang::VbxRawVolume m_file;
    ChannelReadback m_readback;
    uint64_t m_expectedBytes;
    bool m_readbackCompleted = false, m_completed = false;
 public:
    NVRHI_DECLARE_UUID_TRAITS(SaveTicket)
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(SaveTicket)
        NVRHI_IMPLEMENTS_INTERFACE(IGVDBSaveTicket)
        NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(SaveTicket)
    NVRHI_END_INTERFACE_TABLE()
    std::unique_ptr<PreparedReadback> prepared;
    SaveTicket(IDevice* device, slang::VbxRawVolume file, uint64_t bytes, std::unique_ptr<PreparedReadback> readback) noexcept
        : m_device(device),m_file(std::move(file)),m_expectedBytes(bytes),prepared(std::move(readback)) {}
    IDevice* getDevice() const noexcept override { return m_device.Get(); }
    FRESULT finishAfterCallerCompletion(IDataBlob** output, IDataBlob** diagnostic) noexcept override {
        if (output) *output = nullptr;
        if (diagnostic) *diagnostic = nullptr;
        if (output && reinterpret_cast<void*>(output) == reinterpret_cast<void*>(diagnostic)) return FE_INVALID_ARGS;
        return guarded(diagnostic,[&] {
            require(output && output != diagnostic && !m_completed);
            if (!m_readbackCompleted) {
                check(NvrhiVolume::finishReadbackAfterCompletion(*prepared,m_readback),"Save readback is not recorded, completed, or valid.");
                // Move cached logical bytes once. Subsequent CPU allocation failures may retry serialization.
                for (size_t channel=0; channel<m_file.channels.size(); ++channel)
                    m_file.channels[channel].bytes = std::move(m_readback.channels[channel]);
                m_readbackCompleted = true;
            }
            std::vector<uint8_t> bytes;
            std::string error;
            if (!slang::saveRawVBX(m_file,bytes,error)) {
                diagnosticBlob(diagnostic,error.c_str());
                return FE_INVALID_ARGS;
            }
            require(bytes.size() == m_expectedBytes,FE_GENERIC_ERROR,"Serialized size did not match the prepared VBX layout.");
            AutoPtr<IDataBlob> result;
            check(CreateBlob(bytes.size(),result.GetAddressOf()),"Cannot allocate serialized VBX blob.");
            if (!bytes.empty()) std::memcpy(result->GetDataPtr(),bytes.data(),bytes.size());
            m_completed = true;
            *output = result.Detach();
            return FS_OK;
        });
    }
};
class Serializer final : public ObjectImpl<IGVDBSerializer> {
    VolumeHandle m_volume;
    NvrhiVolume* m_core;
    std::shared_ptr<const VolumeGeneration> resolve(IGVDBSnapshot* snapshot) const {
        std::shared_ptr<const VolumeGeneration> result;
        check(detail::resolveSnapshot(snapshot,result),"Snapshot does not belong to this implementation.");
        check(VolumeAdoptionBuilder::validateGeneration(*m_core,*result),"Snapshot belongs to another volume.");
        return result;
    }
 public:
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Serializer)
        NVRHI_IMPLEMENTS_INTERFACE(IGVDBSerializer)
        NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    Serializer(VolumeHandle volume, NvrhiVolume* core) noexcept : m_volume(std::move(volume)),m_core(core) {}
    IGVDBVolume* getVolume() const noexcept override { return m_volume.Get(); }
    FRESULT getMetadata(IGVDBSnapshot* snapshot, VbxMetadata& output) const noexcept override {
        output = {};
        return guarded(nullptr,[&] { output = metadata(*resolve(snapshot)); return FS_OK; });
    }
    FRESULT prepareLoadVBX(IDataBlob* blob, const SerializationBudget& budget,
        IGVDBGenerationTicket** output, IDataBlob** diagnostic) noexcept override {
        if (output) *output = nullptr;
        if (diagnostic) *diagnostic = nullptr;
        if (output && reinterpret_cast<void*>(output) == reinterpret_cast<void*>(diagnostic)) return FE_INVALID_ARGS;
        return guarded(diagnostic,[&] {
            require(output && reinterpret_cast<void*>(output) != reinterpret_cast<void*>(diagnostic) && blob && !budget.reserved);
            const size_t size = blob->GetSize();
            require(size <= budget.maxBlobBytes,FE_OUT_OF_MEMORY,"VBX input exceeds the blob budget.");
            require(size && blob->GetDataPtr(),FE_INVALID_ARGS,"VBX input is empty.");
            slang::VbxRawVolume file;
            std::string error;
            if (!slang::loadRawVBX(blob->GetDataPtr(),size,file,error)) {
                diagnosticBlob(diagnostic,error.c_str()); return FE_INVALID_ARGS;
            }
            require(!file.channels.empty() && file.channels.size() <= c_MaxChannels,FE_UNSUPPORTED,"VBX requires between one and 32 dense channels.");
            std::unique_ptr<HostVolume> candidate;
            check(importHostTopology(file.packed,candidate),"VBX host topology is invalid or unsupported.");
            require(candidate->activeLeafCount(),FE_UNSUPPORTED,"Empty VBX topology is unsupported.");
            std::vector<detail::ImportedLeafSlot> leaves;
            leaves.reserve(candidate->activeLeafCount());
            for (size_t slot=0; slot<file.packed.atlasMap.size(); ++slot) {
                const auto& node = file.packed.atlasMap[slot];
                if (node.leaf >= 0) {
                    require(slot <= UINT32_MAX);
                    leaves.push_back({uint32_t(node.leaf),uint32_t(slot)});
                }
            }
            std::vector<detail::ImportedChannel> channels;
            channels.reserve(file.channels.size());
            for (uint32_t channel=0; channel<file.channels.size(); ++channel) {
                const auto& data = file.channels[channel];
                channels.push_back({channel,ChannelDesc{data.format,{}},data.bytes.data(),data.bytes.size()});
            }
            const auto& grid = file.packed.volume.atlasBrickCountAndWidth;
            detail::VolumeImportRequest request;
            request.candidate = candidate.get();
            request.placement = {{grid.x,grid.y,grid.z},file.packed.volume.atlasResolutionAndApron.w,leaves.data(),leaves.size()};
            request.channels = channels.data(); request.channelCount = channels.size();
            request.budget = {budget.maxAllocatedBytes,budget.maxBuffers};
            request.attachment = MAKE_RC_OBJ_PTR(Metadata,metadata(file));
            std::unique_ptr<PreparedGeneration> prepared;
            check(VolumeAdoptionBuilder::prepareImport(*m_core,request,prepared),"VBX generation preparation failed.");
            return detail::wrapGeneration(m_volume->getDevice(),std::move(prepared),output);
        });
    }
    FRESULT prepareSaveVBX(IGVDBSnapshot* snapshot, const VbxMetadata* overrideMetadata,
        const SerializationBudget& budget, IGVDBSaveTicket** output, IDataBlob** diagnostic) noexcept override {
        if (output) *output = nullptr;
        if (diagnostic) *diagnostic = nullptr;
        if (output && reinterpret_cast<void*>(output) == reinterpret_cast<void*>(diagnostic)) return FE_INVALID_ARGS;
        return guarded(diagnostic,[&] {
            require(output && reinterpret_cast<void*>(output) != reinterpret_cast<void*>(diagnostic) && !budget.reserved);
            const auto generation = resolve(snapshot);
            const auto& bindings = generation->bindings();
            const uint32_t mask = bindings.channelMask;
            require(bindings.activeLeafCount && mask && (mask & (mask+1u)) == 0,FE_UNSUPPORTED,
                "Saving requires a nonempty volume with dense channels 0..N-1.");
            slang::VbxRawVolume file;
            check(VolumeAdoptionBuilder::exportTopology(*generation,file.packed),"Cannot export the frozen snapshot topology.");
            applyMetadata(file,overrideMetadata ? *overrideMetadata : metadata(*generation));
            const auto bytes = serializedBytes(file.packed,bindings);
            require(bytes <= budget.maxBlobBytes && bytes <= SIZE_MAX,FE_OUT_OF_MEMORY,"Serialized VBX exceeds the blob budget.");
            uint64_t allocated = 0;
            uint32_t buffers = 0;
            for (uint32_t channel=0; channel<c_MaxChannels; ++channel) if (mask & (uint32_t(1)<<channel)) {
                const auto& data = bindings.channels[channel];
                allocated = add(allocated,data.storage.allocatedBytes);
                buffers += data.storage.allocatedBytes != 0;
                file.channels.push_back({data.desc.format,{}});
            }
            require(allocated <= budget.maxAllocatedBytes && buffers <= budget.maxBuffers,FE_OUT_OF_MEMORY,
                "VBX readback exceeds the allocation budget.");
            std::unique_ptr<PreparedReadback> prepared;
            check(m_core->prepareReadback(generation,mask,prepared),"Cannot prepare VBX readback buffers.");
            auto result = MAKE_RC_OBJ_PTR(SaveTicket,m_volume->getDevice(),std::move(file),bytes,std::move(prepared));
            *output = result.Detach(); return FS_OK;
        });
    }
    FRESULT recordSaveReadback(RecordingContext context, IGVDBSaveTicket* ticket) noexcept override {
        return guarded(nullptr,[&] {
            const auto value = implementation<SaveTicket>(ticket);
            return m_core->recordReadback(context,*value->prepared);
        });
    }
};
} // namespace
nvrhi::FRESULT gvdbCreateNvrhiSerializer(uint32_t apiVersion, const SerializerCreateDesc* desc,
                                       IGVDBSerializer** output) noexcept {
    if (!output) return nvrhi::FE_INVALID_ARGS;
    *output = nullptr;
    return guarded(nullptr,[&] {
        require(apiVersion == c_GVDBNvrhiApiVersion,FE_UNSUPPORTED,"Unsupported GVDB API version.");
        require(desc && desc->structSize == sizeof(SerializerCreateDesc) && !desc->reserved && desc->volume);
        VolumeHandle volume = desc->volume;
        NvrhiVolume* core = nullptr;
        check(detail::resolveVolume(volume,&core),"Volume does not belong to this implementation.");
        auto result = MAKE_RC_OBJ_PTR(Serializer,std::move(volume),core);
        *output = result.Detach(); return FS_OK;
    });
}
} // namespace gvdb::portable
