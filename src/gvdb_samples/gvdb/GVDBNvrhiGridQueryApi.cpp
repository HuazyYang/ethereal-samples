// SPDX-License-Identifier: Apache-2.0
#include "GVDBNvrhiGridQueryApi.h"
#include "GVDBNvrhiApiInternal.h"
#include "GVDBNvrhiPointApiInternal.h"
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
    slang::UInt4 atlas, work, hierarchy;
};
static_assert(sizeof(Parameters) == 48);
void validateArtifact(IShaderProgram* program, uint32_t entry, const BindingLayoutDesc& layout) {
    ShaderEntryPointDesc desc;
    check(program->getEntryPointDesc(entry, desc));
    require(desc.entryParamsSize == 0 && desc.threadGroupSize[0] == 256 && desc.threadGroupSize[1] == 1 &&
                desc.threadGroupSize[2] == 1,
            FE_UNSUPPORTED);
    const ResourceType types[] = {ResourceType::ConstantBuffer,       ResourceType::RawBuffer_SRV,
                                  ResourceType::RawBuffer_SRV,        ResourceType::RawBuffer_SRV,
                                  ResourceType::StructuredBuffer_SRV, ResourceType::RawBuffer_SRV,
                                  ResourceType::StructuredBuffer_UAV};
    const uint32_t slots[] = {0, 0, 1, 2, 3, 4, 0};
    const uint32_t strides[] = {sizeof(Parameters), 1, 1, 1, 64, 1, 16};
    require(layout.bindings.size() == 7, FE_UNSUPPORTED);
    for (uint32_t i = 0; i < 7; ++i)
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
            uint32_t found = 7;
            for (uint32_t j = 0; j < 7; ++j)
                if (item.resourceType == types[j] && item.registerIndex == slots[j])
                    found = j;
            require(found < 7, FE_UNSUPPORTED);
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
    require(globals == 7 && resources == 127 && members == 3 && mask == 7 && constant != UINT32_MAX,
            FE_UNSUPPORTED);
    for (uint32_t i = 0; i < program->getParameterCount(entry); ++i) {
        ShaderParameterDesc item;
        check(program->getParameterDesc(entry, i, item));
        if (item.scope == ShaderParameterScope::ConstantBuffer)
            require(item.parentParameter == constant, FE_UNSUPPORTED);
    }
}
NVRHI_CLASS_CLSID(GridQueryTicket, "6257496f-b536-4369-b222-860bb173de30")
class GridQueryTicket final : public ObjectImpl<IGVDBGridQueryTicket> {
 public:
    DeviceHandle device;  // Released after GPU resources and generations.
    std::shared_ptr<Identity> identity;
    std::shared_ptr<const VolumeGeneration> source;
    detail::ResolvedPointSet points;
    ComputePipelineHandle pipeline;
    BindingSetHandle bindings;
    BufferHandle constants, output, staging, dummyRaw, dummyLevels;
    std::vector<Float4> values;
    Parameters parameters{};
    uint32_t count = 0;
    bool recorded = false, completed = false;
    NVRHI_DECLARE_UUID_TRAITS(GridQueryTicket)
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(GridQueryTicket)
    NVRHI_IMPLEMENTS_INTERFACE(IGVDBGridQueryTicket)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_IMPLEMENTS_CLASS(GridQueryTicket)
    NVRHI_END_INTERFACE_TABLE()
    IDevice* getDevice() const noexcept override { return device.Get(); }
    uint64_t getCount() const noexcept override { return count; }
    FRESULT getOutputBuffer(BorrowedBufferBinding& out) const noexcept override {
        if (!recorded)
            return FE_INVALID_ARGS;
        out = {output.Get(), 0, uint64_t(count) * 16};
        return FS_OK;
    }
    FRESULT getValues(BorrowedBytes& out) const noexcept override {
        if (!completed)
            return FE_INVALID_ARGS;
        out = {values.empty() ? nullptr : values.data(), uint64_t(count) * 16};
        return FS_OK;
    }
    FRESULT finishAfterCallerCompletion(BorrowedBytes& out) noexcept override {
        return guarded([&] {
            require(recorded && !completed);
            std::vector<Float4> result(count);
            if (count) {
                const void* data = device->mapBuffer(staging, CpuAccessMode::Read);
                require(data, FE_GENERIC_ERROR);
                std::memcpy(result.data(), data, size_t(count) * 16);
                device->unmapBuffer(staging);
            }
            values.swap(result);
            completed = true;
            check(getValues(out));
        });
    }
};
class GridQuery final : public ObjectImpl<IGVDBGridQuery> {
 public:
    DeviceHandle device;
    std::shared_ptr<Identity> identity = std::make_shared<Identity>();
    ShaderProgramHandle program;
    BindingLayoutHandle layout;
    ComputePipelineHandle pipeline;
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(GridQuery)
    NVRHI_IMPLEMENTS_INTERFACE(IGVDBGridQuery)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    IDevice* getDevice() const noexcept override { return device.Get(); }
    FRESULT prepare(const GridQueryRequestDesc& d, const GridQueryPreparationBudget& budget,
                    IGVDBGridQueryTicket** out) noexcept override {
        if (!out)
            return FE_INVALID_ARGS;
        *out = nullptr;
        return guarded([&] {
            require(d.structSize == sizeof(d) && !d.reserved && !budget.reserved && d.channel < c_MaxChannels &&
                    (d.mode == GridQueryMode::ScalarVoxel || d.mode == GridQueryMode::MacYDiagnostic));
            auto t = MAKE_RC_OBJ_PTR(GridQueryTicket);
            t->device = device;
            t->identity = identity;
            check(detail::resolveSnapshot(d.snapshot, t->source));
            check(detail::resolvePointSet(d.points, device, t->points));
            const auto& s = t->source->bindings();
            require(s.device == device);
            require(s.channelMask & (1u << d.channel), FE_NOT_FOUND);
            const auto& channel = s.channels[d.channel];
            require(channel.desc.format == AtlasFormat::R32_FLOAT, FE_UNSUPPORTED);
            const uint64_t count = t->points.info.count;
            require(count <= UINT32_MAX / 16, FE_UNSUPPORTED);
            t->count = uint32_t(count);
            require(count * 16 <= budget.maxHostBytes, FE_OUT_OF_MEMORY);
            if (count)
                require(budget.maxBuffers >= 5 && 384 + count * 32 <= budget.maxAllocatedBytes,
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
                    desc.debugName = "GVDB grid query mapping";
                    BufferHandle result;
                    check(device->createBuffer(desc, &result));
                    require(bool(result), FE_OUT_OF_MEMORY);
                    return result;
                };
                t->constants = allocate(256, 0, false, true, false, false);
                t->output = allocate(count * 16, 16, false, false, false, true);
                t->staging = allocate(count * 16, 0, false, false, true, false);
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
                raw(0, s.nodes);
                raw(1, s.children);
                raw(2, channel.raw);
                b.addItem(BindingSetItem::StructuredBuffer_SRV(
                    3, s.levelMetadata.buffer ? s.levelMetadata.buffer.Get() : t->dummyLevels.Get(),
                    Format::UNKNOWN,
                    s.levelMetadata.buffer ? BufferRange(s.levelMetadata.byteOffset, s.levelMetadata.byteSize)
                                           : BufferRange(0, 64)));
                const auto& p = t->points.positionStream;
                b.addItem(BindingSetItem::RawBuffer_SRV(4, p.buffer, BufferRange(p.byteOffset, p.byteSize)));
                b.addItem(BindingSetItem::StructuredBuffer_UAV(0, t->output));
                check(device->createBindingSet(b, layout, &t->bindings));
                t->pipeline = pipeline;
                t->parameters = {
                    s.volume.atlasResolutionAndApron,
                    {t->count, uint32_t(p.byteStride), uint32_t(d.mode), s.activeLeafCount},
                    s.volume.hierarchy
                };
            }
            *out = t.Detach();
        });
    }
    FRESULT record(RecordingContext context, IGVDBGridQueryTicket* ticket) noexcept override {
        return guarded([&] {
            require(ticket != nullptr);
            AutoPtr<GridQueryTicket> value;
            require(NVRHI_SUCCEEDED(ticket->QueryInterface(uuid_of<GridQueryTicket>(),
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
            std::array<uint8_t, 256> constants{};
            std::memcpy(constants.data(), &t.parameters, sizeof(t.parameters));
            context.commands->writeBuffer(t.constants, constants.data(), constants.size());
            std::array<uint8_t, 64> zero{};
            context.commands->writeBuffer(t.dummyRaw, zero.data(), 64);
            context.commands->writeBuffer(t.dummyLevels, zero.data(), 64);
            context.commands->setComputeState(ComputeState().setPipeline(t.pipeline).addBindingSet(t.bindings));
            const uint32_t groups = (t.count + 255) / 256;
            context.commands->dispatch(std::min(groups, 65535u), (groups + 65534) / 65535);
            context.commands->copyBuffer(t.staging, 0, t.output, 0, uint64_t(t.count) * 16);
        });
    }
};
}  // namespace
FRESULT gvdbCreateNvrhiGridQuery(uint32_t version, const GridQueryCreateDesc* desc,
                                 IGVDBGridQuery** out) noexcept {
    if (!out)
        return FE_INVALID_ARGS;
    *out = nullptr;
    return guarded([&] {
        require(version == c_GVDBNvrhiApiVersion, FE_UNSUPPORTED);
        require(desc && desc->structSize == sizeof(*desc) && !desc->reserved && desc->device &&
                desc->artifact && desc->artifactBytes && desc->artifactBytes <= SIZE_MAX);
        auto op = MAKE_RC_OBJ_PTR(GridQuery);
        op->device = desc->device;
        check(createShaderProgram(op->device, desc->artifact, size_t(desc->artifactBytes), &op->program));
        uint32_t entry;
        check(op->program->findEntryPoint("queryScalarGrid", ShaderType::Compute, &entry));
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
