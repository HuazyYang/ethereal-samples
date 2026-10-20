// SPDX-License-Identifier: Apache-2.0
#include "GVDBNvrhiRayQueryApi.h"
#include "GVDBNvrhiApiInternal.h"
#include <cmath>
#include "GVDBNvrhiVolume.h"
#include <nvrhi/core/foundation.h>
#include <nvrhi/shader-program.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>
namespace gvdb::portable {
namespace {
using namespace nvrhi;
struct Failure {
    FRESULT result;
};
void require(bool v, FRESULT r = FE_INVALID_ARGS) {
    if (!v)
        throw Failure{r};
}
void check(FRESULT r) { require(NVRHI_SUCCEEDED(r), r); }
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
struct alignas(16) Parameters {
    VolumeInfo volume;
    slang::UInt4 counts;
    Float4 sampling;
};
static_assert(sizeof(Parameters) == 112);
void validateArtifact(IShaderProgram* program, uint32_t entry, const BindingLayoutDesc& layout) {
    ShaderEntryPointDesc desc;
    check(program->getEntryPointDesc(entry, desc));
    require(desc.entryParamsSize == 0 && desc.threadGroupSize[0] == 256 && desc.threadGroupSize[1] == 1 &&
                desc.threadGroupSize[2] == 1,
            FE_UNSUPPORTED);
    const ResourceType types[] = {
        ResourceType::ConstantBuffer, ResourceType::RawBuffer_SRV,        ResourceType::RawBuffer_SRV,
        ResourceType::RawBuffer_SRV,  ResourceType::StructuredBuffer_SRV, ResourceType::RawBuffer_SRV,
        ResourceType::RawBuffer_UAV,  ResourceType::StructuredBuffer_UAV, ResourceType::StructuredBuffer_UAV};
    const uint32_t slots[] = {0, 0, 1, 2, 3, 4, 0, 1, 2};
    const uint32_t strides[] = {sizeof(Parameters), 1, 1, 1, 64, 1, 1, 16, 16};
    require(layout.bindings.size() == 9, FE_UNSUPPORTED);
    for (uint32_t i = 0; i < 9; ++i)
        require(std::count_if(
                    layout.bindings.begin(), layout.bindings.end(),
                    [&](const auto& item) { return item.type == types[i] && item.slot == slots[i]; }) == 1,
                FE_UNSUPPORTED);
    uint32_t globals = 0, resources = 0, members = 0, mask = 0, constant = UINT32_MAX;
    for (uint32_t i = 0; i < program->getParameterCount(entry); ++i) {
        ShaderParameterDesc item;
        check(program->getParameterDesc(entry, i, item));
        require(item.registerSpace == 0 && item.count == 1, FE_UNSUPPORTED);
        if (item.scope == ShaderParameterScope::Global) {
            ++globals;
            require(item.parentParameter == UINT32_MAX, FE_UNSUPPORTED);
            uint32_t found = 9;
            for (uint32_t j = 0; j < 9; ++j)
                if (item.resourceType == types[j] && item.registerIndex == slots[j])
                    found = j;
            require(found < 9, FE_UNSUPPORTED);
            require(!(resources & (1u << found)) && item.elementStride == strides[found], FE_UNSUPPORTED);
            resources |= 1u << found;
            if (item.resourceType == ResourceType::ConstantBuffer)
                constant = i;
        } else {
            require(item.scope == ShaderParameterScope::ConstantBuffer &&
                        item.resourceType == ResourceType::None && item.offset < sizeof(Parameters) &&
                        item.offset % 16 == 0 && item.size == 16 && item.elementStride == 16,
                    FE_UNSUPPORTED);
            const uint32_t bit = 1u << (item.offset / 16);
            require(!(mask & bit), FE_UNSUPPORTED);
            mask |= bit;
            ++members;
        }
    }
    require(globals == 9 && resources == 511 && members == 7 && mask == 127 && constant != UINT32_MAX,
            FE_UNSUPPORTED);
    for (uint32_t i = 0; i < program->getParameterCount(entry); ++i) {
        ShaderParameterDesc item;
        check(program->getParameterDesc(entry, i, item));
        if (item.scope == ShaderParameterScope::ConstantBuffer)
            require(item.parentParameter == constant, FE_UNSUPPORTED);
    }
}
NVRHI_CLASS_CLSID(RayQueryTicket, "7094a45c-8aa3-4d35-b4d3-1ec091bf2ab6")
class RayQueryTicket final : public ObjectImpl<IGVDBRayQueryTicket> {
 public:
    DeviceHandle device;  // Released after GPU resources and generations.
    std::shared_ptr<Identity> identity;
    std::shared_ptr<const VolumeGeneration> source;
    std::vector<RayRecord> rays;
    ComputePipelineHandle pipeline;
    BindingSetHandle bindings;
    BufferHandle constants, input, output, hits, normals, staging, dummyRaw, dummyLevels;
    std::vector<RayRecord> values;
    std::vector<Float4> hitValues, normalValues;
    Parameters parameters{};
    uint32_t count = 0;
    bool recorded = false, completed = false;
    NVRHI_DECLARE_UUID_TRAITS(RayQueryTicket)
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(RayQueryTicket)
    NVRHI_IMPLEMENTS_INTERFACE(IGVDBRayQueryTicket)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_IMPLEMENTS_CLASS(RayQueryTicket)
    NVRHI_END_INTERFACE_TABLE()
    IDevice* getDevice() const noexcept override { return device.Get(); }
    uint64_t getCount() const noexcept override { return count; }
    FRESULT getOutputBuffer(RayQueryOutput which, BorrowedBufferBinding& out) const noexcept override {
        if (!recorded || uint32_t(which) > 2)
            return FE_INVALID_ARGS;
        out = {which == RayQueryOutput::Records ? output.Get()
               : which == RayQueryOutput::Hits  ? hits.Get()
                                                : normals.Get(),
               0, uint64_t(count) * (which == RayQueryOutput::Records ? 64 : 16)};
        return FS_OK;
    }
    FRESULT getValues(RayQueryOutput which, BorrowedBytes& out) const noexcept override {
        if (!completed || uint32_t(which) > 2)
            return FE_INVALID_ARGS;
        const void* data = which == RayQueryOutput::Records ? static_cast<const void*>(values.data())
                           : which == RayQueryOutput::Hits  ? hitValues.data()
                                                            : normalValues.data();
        out = {count ? data : nullptr, uint64_t(count) * (which == RayQueryOutput::Records ? 64 : 16)};
        return FS_OK;
    }
    FRESULT finishAfterCallerCompletion() noexcept override {
        return guarded([&] {
            require(recorded && !completed);
            std::vector<RayRecord> records(count);
            std::vector<Float4> nextHits(count), nextNormals(count);
            if (count) {
                const auto* data = static_cast<const uint8_t*>(device->mapBuffer(staging, CpuAccessMode::Read));
                require(data, FE_GENERIC_ERROR);
                std::memcpy(records.data(), data, size_t(count) * 64);
                std::memcpy(nextHits.data(), data + size_t(count) * 64, size_t(count) * 16);
                std::memcpy(nextNormals.data(), data + size_t(count) * 80, size_t(count) * 16);
                device->unmapBuffer(staging);
            }
            values.swap(records);
            hitValues.swap(nextHits);
            normalValues.swap(nextNormals);
            completed = true;
        });
    }
};
class RayQuery final : public ObjectImpl<IGVDBRayQuery> {
 public:
    DeviceHandle device;
    std::shared_ptr<Identity> identity = std::make_shared<Identity>();
    ShaderProgramHandle program;
    BindingLayoutHandle layout;
    ComputePipelineHandle pipeline;
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(RayQuery)
    NVRHI_IMPLEMENTS_INTERFACE(IGVDBRayQuery)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    IDevice* getDevice() const noexcept override { return device.Get(); }
    FRESULT prepare(const RayQueryRequestDesc& d, const RayQueryPreparationBudget& budget,
                    IGVDBRayQueryTicket** out) noexcept override {
        if (!out)
            return FE_INVALID_ARGS;
        *out = nullptr;
        return guarded([&] {
            require(d.structSize == sizeof(d) && !d.reserved && !d.reserved2 && !budget.reserved &&
                    d.channel < c_MaxChannels && (!d.count || d.rays) && std::isfinite(d.step) && d.step > 0 &&
                    std::isfinite(d.threshold) && std::isfinite(d.epsilon) && d.epsilon >= 0 &&
                    std::isfinite(d.bias) && d.maxLeafSteps && d.maxLeafVisits);
            auto t = MAKE_RC_OBJ_PTR(RayQueryTicket);
            t->device = device;
            t->identity = identity;
            check(detail::resolveSnapshot(d.snapshot, t->source));
            const auto& s = t->source->bindings();
            require(s.device == device);
            require(s.channelMask & (1u << d.channel), FE_NOT_FOUND);
            const auto& channel = s.channels[d.channel];
            require(channel.desc.format == AtlasFormat::R32_FLOAT, FE_UNSUPPORTED);
            const uint64_t count = d.count;
            require(count <= UINT32_MAX / 96, FE_UNSUPPORTED);
            t->count = uint32_t(count);
            require(count * 160 <= budget.maxHostBytes, FE_OUT_OF_MEMORY);
            if (count)
                require(budget.maxBuffers >= 8 && 384 + count * 256 <= budget.maxAllocatedBytes,
                        FE_OUT_OF_MEMORY);
            if (count) {
                auto allocate = [&](uint64_t bytes, uint32_t stride, bool raw, bool cb, bool readback,
                                    bool uav) {
                    BufferDesc desc;
                    desc.byteSize = bytes;
                    desc.structStride = stride;
                    desc.canHaveRawViews = raw;
                    desc.isConstantBuffer = cb;
                    desc.canHaveUAVs = uav;
                    desc.cpuAccess = readback ? CpuAccessMode::Read : CpuAccessMode::None;
                    desc.initialState = cb ? ResourceStates::ConstantBuffer : ResourceStates::CopyDest;
                    desc.keepInitialState = true;
                    desc.debugName = "GVDB retained ray query";
                    BufferHandle result;
                    check(device->createBuffer(desc, &result));
                    require(bool(result), FE_OUT_OF_MEMORY);
                    return result;
                };
                t->constants = allocate(256, 0, false, true, false, false);
                t->rays.assign(d.rays, d.rays + count);
                t->input = allocate(count * 64, 0, true, false, false, false);
                t->output = allocate(count * 64, 0, true, false, false, true);
                t->hits = allocate(count * 16, 16, false, false, false, true);
                t->normals = allocate(count * 16, 16, false, false, false, true);
                t->staging = allocate(count * 96, 0, false, false, true, false);
                t->dummyRaw = allocate(64, 0, true, false, false, false);
                t->dummyLevels = allocate(64, 64, false, false, false, false);
                BindingSetDesc b;
                b.trackLiveness = true;
                b.addItem(BindingSetItem::ConstantBuffer(0, t->constants));
                auto raw = [&](uint32_t slot, const BufferView& view) {
                    b.addItem(BindingSetItem::RawBuffer_SRV(
                        slot, view.buffer ? view.buffer.Get() : t->dummyRaw.Get(),
                        view.buffer ? BufferRange(view.byteOffset, view.byteSize) : BufferRange(0, 64)));
                };
                raw(0, channel.raw);
                raw(1, s.nodes);
                raw(2, s.children);
                b.addItem(BindingSetItem::StructuredBuffer_SRV(
                    3, s.levelMetadata.buffer ? s.levelMetadata.buffer.Get() : t->dummyLevels.Get(),
                    Format::UNKNOWN,
                    s.levelMetadata.buffer ? BufferRange(s.levelMetadata.byteOffset, s.levelMetadata.byteSize)
                                           : BufferRange(0, 64)));
                b.addItem(BindingSetItem::RawBuffer_SRV(4, t->input));
                b.addItem(BindingSetItem::RawBuffer_UAV(0, t->output));
                b.addItem(BindingSetItem::StructuredBuffer_UAV(1, t->hits));
                b.addItem(BindingSetItem::StructuredBuffer_UAV(2, t->normals));
                check(device->createBindingSet(b, layout, &t->bindings));
                t->pipeline = pipeline;
                t->parameters = {
                    s.volume,
                    {t->count, d.maxLeafSteps, d.maxLeafVisits, s.activeLeafCount},
                    {d.step,   d.threshold,    d.epsilon,       d.bias           }
                };
            }
            *out = t.Detach();
        });
    }
    FRESULT record(RecordingContext context, IGVDBRayQueryTicket* ticket) noexcept override {
        return guarded([&] {
            require(ticket != nullptr);
            AutoPtr<RayQueryTicket> value;
            require(NVRHI_SUCCEEDED(ticket->QueryInterface(uuid_of<RayQueryTicket>(),
                                                           reinterpret_cast<void**>(value.GetAddressOf()))) &&
                    value.Get());
            auto& t = *value;
            require(t.identity == identity && !t.recorded && !t.completed);
            require(context.device == device.Get() && context.commands &&
                    context.commands->getDevice() == context.device);
            require(context.commands->getDesc().queueType != CommandQueue::Copy, FE_UNSUPPORTED);
            t.recorded = true;
            if (!t.count)
                return;
            context.commands->writeBuffer(t.input, t.rays.data(), size_t(t.count) * 64);
            std::array<uint8_t, 256> constants{};
            std::memcpy(constants.data(), &t.parameters, sizeof(t.parameters));
            context.commands->writeBuffer(t.constants, constants.data(), constants.size());
            std::array<uint8_t, 64> zero{};
            context.commands->writeBuffer(t.dummyRaw, zero.data(), 64);
            context.commands->writeBuffer(t.dummyLevels, zero.data(), 64);
            context.commands->setComputeState(ComputeState().setPipeline(t.pipeline).addBindingSet(t.bindings));
            const uint32_t groups = (t.count + 255) / 256;
            context.commands->dispatch(std::min(groups, 65535u), (groups + 65534) / 65535);
            context.commands->copyBuffer(t.staging, 0, t.output, 0, uint64_t(t.count) * 64);
            context.commands->copyBuffer(t.staging, uint64_t(t.count) * 64, t.hits, 0, uint64_t(t.count) * 16);
            context.commands->copyBuffer(t.staging, uint64_t(t.count) * 80, t.normals, 0,
                                         uint64_t(t.count) * 16);
        });
    }
};
}  // namespace
FRESULT gvdbCreateNvrhiRayQuery(uint32_t version, const RayQueryCreateDesc* desc,
                                IGVDBRayQuery** out) noexcept {
    if (!out)
        return FE_INVALID_ARGS;
    *out = nullptr;
    return guarded([&] {
        require(version == c_GVDBNvrhiApiVersion, FE_UNSUPPORTED);
        require(desc && desc->structSize == sizeof(*desc) && !desc->reserved && desc->device &&
                desc->artifact && desc->artifactBytes && desc->artifactBytes <= SIZE_MAX);
        auto op = MAKE_RC_OBJ_PTR(RayQuery);
        op->device = desc->device;
        check(createShaderProgram(op->device, desc->artifact, size_t(desc->artifactBytes), &op->program));
        uint32_t entry;
        check(op->program->findEntryPoint("queryRayRecords", ShaderType::Compute, &entry));
        BindingLayoutDesc layout;
        check(op->program->getBindingLayoutDesc(entry, 0, layout));
        validateArtifact(op->program, entry, layout);
        check(op->device->createBindingLayout(layout, &op->layout));
        ShaderHandle shader;
        check(op->program->createShader(entry, &shader));
        check(op->device->createComputePipeline(
            ComputePipelineDesc().setComputeShader(shader).addBindingLayout(op->layout), &op->pipeline));
        *out = op.Detach();
    });
}
}  // namespace gvdb::portable
