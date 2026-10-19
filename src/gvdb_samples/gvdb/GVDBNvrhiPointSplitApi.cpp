// SPDX-License-Identifier: Apache-2.0
#include "GVDBNvrhiPointSplitApi.h"
#include "GVDBNvrhiPointApiInternal.h"
#include <cmath>
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
    slang::UInt4 work;
    Float4 translation;
};
static_assert(sizeof(Parameters) == 32);
void validateArtifact(IShaderProgram* program, uint32_t entry, const BindingLayoutDesc& layout) {
    ShaderEntryPointDesc desc;
    check(program->getEntryPointDesc(entry, desc));
    require(desc.entryParamsSize == 0 && desc.threadGroupSize[0] == 256 && desc.threadGroupSize[1] == 1 &&
                desc.threadGroupSize[2] == 1,
            FE_UNSUPPORTED);
    const ResourceType types[] = {ResourceType::ConstantBuffer, ResourceType::RawBuffer_SRV,
                                  ResourceType::RawBuffer_UAV};
    const uint32_t slots[] = {0, 0, 0};
    const uint32_t strides[] = {sizeof(Parameters), 1, 1};
    require(layout.bindings.size() == 3, FE_UNSUPPORTED);
    for (uint32_t i = 0; i < 3; ++i)
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
            uint32_t found = 3;
            for (uint32_t j = 0; j < 3; ++j)
                if (item.resourceType == types[j] && item.registerIndex == slots[j])
                    found = j;
            require(found < 3, FE_UNSUPPORTED);
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
    require(globals == 3 && resources == 7 && members == 2 && mask == 3 && constant != UINT32_MAX,
            FE_UNSUPPORTED);
    for (uint32_t i = 0; i < program->getParameterCount(entry); ++i) {
        ShaderParameterDesc item;
        check(program->getParameterDesc(entry, i, item));
        if (item.scope == ShaderParameterScope::ConstantBuffer)
            require(item.parentParameter == constant, FE_UNSUPPORTED);
    }
}
NVRHI_CLASS_CLSID(PointSplitTicket, "8b840033-59b1-4e98-b661-692e0e60a6ed")
class PointSplitTicket final : public ObjectImpl<IGVDBPointSplitTicket> {
 public:
    DeviceHandle device;  // Released after GPU resources and generations.
    std::shared_ptr<Identity> identity;
    detail::ResolvedPointSet points;
    ComputePipelineHandle pipeline;
    BindingSetHandle bindings;
    BufferHandle constants, output, staging;
    std::vector<float> values;
    Float4 minimum{}, maximum{};
    BorrowedPointStream stream;
    Parameters parameters{};
    uint32_t count = 0;
    bool recorded = false, completed = false;
    NVRHI_DECLARE_UUID_TRAITS(PointSplitTicket)
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(PointSplitTicket)
    NVRHI_IMPLEMENTS_INTERFACE(IGVDBPointSplitTicket)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_IMPLEMENTS_CLASS(PointSplitTicket)
    NVRHI_END_INTERFACE_TABLE()
    IDevice* getDevice() const noexcept override { return device.Get(); }
    uint64_t getCount() const noexcept override { return count; }
    FRESULT getAxisBuffer(uint32_t axis, BorrowedBufferBinding& out) const noexcept override {
        if (axis >= 3 || !recorded)
            return FE_INVALID_ARGS;
        out = {output.Get(), uint64_t(axis) * count * 4, uint64_t(count) * 4};
        return FS_OK;
    }
    FRESULT getAxisData(uint32_t axis, BorrowedBytes& out) const noexcept override {
        if (axis >= 3 || !completed)
            return FE_INVALID_ARGS;
        out = {count ? values.data() + size_t(axis) * count : nullptr, uint64_t(count) * 4};
        return FS_OK;
    }
    FRESULT getBounds(Float4& lo, Float4& hi) const noexcept override {
        if (!completed)
            return FE_INVALID_ARGS;
        lo = minimum;
        hi = maximum;
        return FS_OK;
    }
    FRESULT finishAfterCallerCompletion() noexcept override {
        return guarded([&] {
            require(recorded && !completed);
            std::vector<float> result(size_t(count) * 3);
            if (count) {
                const void* data = device->mapBuffer(staging, CpuAccessMode::Read);
                require(data, FE_GENERIC_ERROR);
                std::memcpy(result.data(), data, size_t(count) * 12);
                device->unmapBuffer(staging);
            }
            float lo[3]{}, hi[3]{};
            for (unsigned axis = 0; axis < 3; ++axis)
                for (uint32_t i = 0; i < count; ++i) {
                    const float v = result[size_t(axis) * count + i];
                    require(std::isfinite(v), FE_GENERIC_ERROR);
                    if (!i)
                        lo[axis] = hi[axis] = v;
                    else {
                        lo[axis] = std::min(lo[axis], v);
                        hi[axis] = std::max(hi[axis], v);
                    }
                }
            minimum = {lo[0], lo[1], lo[2], 0};
            maximum = {hi[0], hi[1], hi[2], 0};
            values.swap(result);
            completed = true;
        });
    }
};
class PointSplit final : public ObjectImpl<IGVDBPointSplit> {
 public:
    DeviceHandle device;
    std::shared_ptr<Identity> identity = std::make_shared<Identity>();
    ShaderProgramHandle program;
    BindingLayoutHandle layout;
    ComputePipelineHandle pipeline;
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(PointSplit)
    NVRHI_IMPLEMENTS_INTERFACE(IGVDBPointSplit)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    IDevice* getDevice() const noexcept override { return device.Get(); }
    FRESULT prepare(const PointSplitRequestDesc& d, const PointSplitPreparationBudget& budget,
                    IGVDBPointSplitTicket** out) noexcept override {
        if (!out)
            return FE_INVALID_ARGS;
        *out = nullptr;
        return guarded([&] {
            require(d.structSize == sizeof(d) && !budget.reserved &&
                    (d.attribute == PointAttribute::Position || d.attribute == PointAttribute::Velocity) &&
                    std::isfinite(d.translation.x) && std::isfinite(d.translation.y) &&
                    std::isfinite(d.translation.z) && d.translation.w == 0);
            auto t = MAKE_RC_OBJ_PTR(PointSplitTicket);
            t->device = device;
            t->identity = identity;
            check(detail::resolvePointSet(d.points, device, t->points));
            BorrowedBytes input;
            check(t->points.retained->getData(d.attribute, input));
            check(t->points.retained->getStream(d.attribute, t->stream));
            const uint64_t count = t->points.info.count;
            require(count <= UINT32_MAX / 12, FE_UNSUPPORTED);
            t->count = uint32_t(count);
            require(count * 12 <= budget.maxHostBytes, FE_OUT_OF_MEMORY);
            if (count)
                require(budget.maxBuffers >= 3 && 256 + count * 24 <= budget.maxAllocatedBytes,
                        FE_OUT_OF_MEMORY);
            for (uint64_t i = 0; i < count; ++i) {
                Float4 p{};
                std::memcpy(&p, static_cast<const uint8_t*>(input.data) + size_t(i) * t->stream.byteStride, 12);
                require(std::isfinite(p.x + d.translation.x) && std::isfinite(p.y + d.translation.y) &&
                        std::isfinite(p.z + d.translation.z));
            }
            if (count) {
                auto allocate = [&](uint64_t bytes, bool constant, bool readback) {
                    BufferDesc desc;
                    desc.byteSize = bytes;
                    desc.isConstantBuffer = constant;
                    desc.canHaveRawViews = !constant && !readback;
                    desc.canHaveUAVs = desc.canHaveRawViews;
                    desc.cpuAccess = readback ? CpuAccessMode::Read : CpuAccessMode::None;
                    desc.initialState = constant ? ResourceStates::ConstantBuffer : ResourceStates::CopyDest;
                    desc.keepInitialState = true;
                    desc.debugName = "GVDB coordinate planes";
                    BufferHandle buffer;
                    check(device->createBuffer(desc, &buffer));
                    require(bool(buffer), FE_OUT_OF_MEMORY);
                    return buffer;
                };
                t->constants = allocate(256, true, false);
                t->output = allocate(count * 12, false, false);
                t->staging = allocate(count * 12, false, true);
                BindingSetDesc bindings;
                bindings.trackLiveness = true;
                bindings.addItem(BindingSetItem::ConstantBuffer(0, t->constants))
                    .addItem(BindingSetItem::RawBuffer_SRV(
                        0, t->stream.buffer, BufferRange(t->stream.byteOffset, t->stream.byteSize)))
                    .addItem(BindingSetItem::RawBuffer_UAV(0, t->output));
                check(device->createBindingSet(bindings, layout, &t->bindings));
                t->pipeline = pipeline;
                t->parameters = {
                    {t->count, uint32_t(t->stream.byteStride), 0, 0},
                    d.translation
                };
            }
            *out = t.Detach();
        });
    }
    FRESULT record(RecordingContext context, IGVDBPointSplitTicket* ticket) noexcept override {
        return guarded([&] {
            require(ticket != nullptr);
            AutoPtr<PointSplitTicket> value;
            require(NVRHI_SUCCEEDED(ticket->QueryInterface(uuid_of<PointSplitTicket>(),
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
            context.commands->setComputeState(ComputeState().setPipeline(t.pipeline).addBindingSet(t.bindings));
            const uint32_t groups = (t.count + 255) / 256;
            context.commands->dispatch(std::min(groups, 65535u), (groups + 65534) / 65535);
            context.commands->copyBuffer(t.staging, 0, t.output, 0, uint64_t(t.count) * 12);
        });
    }
};
}  // namespace
FRESULT gvdbCreateNvrhiPointSplit(uint32_t version, const PointSplitCreateDesc* desc,
                                  IGVDBPointSplit** out) noexcept {
    if (!out)
        return FE_INVALID_ARGS;
    *out = nullptr;
    return guarded([&] {
        require(version == c_GVDBNvrhiApiVersion, FE_UNSUPPORTED);
        require(desc && desc->structSize == sizeof(*desc) && !desc->reserved && desc->device &&
                desc->artifact && desc->artifactBytes && desc->artifactBytes <= SIZE_MAX);
        auto op = MAKE_RC_OBJ_PTR(PointSplit);
        op->device = desc->device;
        check(createShaderProgram(op->device, desc->artifact, size_t(desc->artifactBytes), &op->program));
        uint32_t entry;
        check(op->program->findEntryPoint("splitPointCoordinates", ShaderType::Compute, &entry));
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
