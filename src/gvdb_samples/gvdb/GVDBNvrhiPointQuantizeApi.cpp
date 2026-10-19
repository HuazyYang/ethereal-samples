// SPDX-License-Identifier: Apache-2.0
#include "GVDBNvrhiPointQuantizeApi.h"
#include "GVDBNvrhiPointApiInternal.h"
#include <nvrhi/core/foundation.h>
#include <nvrhi/shader-program.h>
#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
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
FRESULT guarded(F&& function) noexcept {
    try {
        function();
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
bool finite(Float4 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w);
}
struct alignas(16) Parameters {
    slang::UInt4 info;
    Float4 minimum, range;
};
static_assert(sizeof(Parameters) == 48);
struct Program {
    ShaderProgramHandle program;
    BindingLayoutHandle layout;
    ComputePipelineHandle pipeline;
};
Program loadProgram(IDevice* device, const void* bytes, uint64_t size, const char* name) {
    require(bytes && size && size <= SIZE_MAX);
    Program result;
    check(createShaderProgram(device, bytes, size_t(size), &result.program));
    uint32_t entry;
    check(result.program->findEntryPoint(name, ShaderType::Compute, &entry));
    ShaderEntryPointDesc desc;
    check(result.program->getEntryPointDesc(entry, desc));
    require(desc.entryParamsSize == 0 && desc.threadGroupSize[0] == 256 && desc.threadGroupSize[1] == 1 &&
                desc.threadGroupSize[2] == 1,
            FE_UNSUPPORTED);
    BindingLayoutDesc layout;
    check(result.program->getBindingLayoutDesc(entry, 0, layout));
    const ResourceType types[] = {ResourceType::ConstantBuffer, ResourceType::RawBuffer_SRV,
                                  ResourceType::RawBuffer_UAV, ResourceType::RawBuffer_UAV};
    const uint32_t slots[] = {0, 0, 0, 1};
    require(layout.bindings.size() == 4, FE_UNSUPPORTED);
    for (uint32_t i = 0; i < 4; ++i)
        require(std::count_if(layout.bindings.begin(), layout.bindings.end(),
                              [&](const auto& b) { return b.type == types[i] && b.slot == slots[i]; }) == 1,
                FE_UNSUPPORTED);
    uint32_t globals = 0, members = 0, mask = 0, resources = 0, constant = UINT32_MAX;
    for (uint32_t i = 0; i < result.program->getParameterCount(entry); ++i) {
        ShaderParameterDesc item;
        check(result.program->getParameterDesc(entry, i, item));
        require(item.registerSpace == 0 && item.count == 1, FE_UNSUPPORTED);
        if (item.scope == ShaderParameterScope::Global) {
            ++globals;
            require(item.parentParameter == UINT32_MAX, FE_UNSUPPORTED);
            uint32_t found = 4;
            for (uint32_t j = 0; j < 4; ++j)
                if (item.resourceType == types[j] && item.registerIndex == slots[j])
                    found = j;
            require(found < 4 && !(resources & (1u << found)), FE_UNSUPPORTED);
            resources |= 1u << found;
            require(item.elementStride == (found == 0 ? sizeof(Parameters) : 1), FE_UNSUPPORTED);
            if (found == 0)
                constant = i;
        } else {
            require(item.scope == ShaderParameterScope::ConstantBuffer &&
                        item.resourceType == ResourceType::None && item.size == 16 &&
                        item.elementStride == 16 && item.offset < 48 && item.offset % 16 == 0,
                    FE_UNSUPPORTED);
            const uint32_t bit = 1u << (item.offset / 16);
            require(!(mask & bit), FE_UNSUPPORTED);
            mask |= bit;
            ++members;
        }
    }
    require(globals == 4 && resources == 15 && members == 3 && mask == 7 && constant != UINT32_MAX,
            FE_UNSUPPORTED);
    for (uint32_t i = 0; i < result.program->getParameterCount(entry); ++i) {
        ShaderParameterDesc item;
        check(result.program->getParameterDesc(entry, i, item));
        if (item.scope == ShaderParameterScope::ConstantBuffer)
            require(item.parentParameter == constant, FE_UNSUPPORTED);
    }
    check(device->createBindingLayout(layout, &result.layout));
    ShaderHandle shader;
    check(result.program->createShader(entry, &shader));
    check(device->createComputePipeline(
        ComputePipelineDesc().setComputeShader(shader).addBindingLayout(result.layout), &result.pipeline));
    return result;
}
struct Kernels {
    DeviceHandle device;
    Program encode, decode;
};
struct Identity {};
NVRHI_CLASS_CLSID(QuantizeTicket, "e3cd67a9-bcf1-4983-b317-c7d0ed79a8be")
class QuantizeTicket final : public ObjectImpl<IGVDBPointQuantizeTicket> {
 public:
    NVRHI_DECLARE_UUID_TRAITS(QuantizeTicket)
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(QuantizeTicket)
    NVRHI_IMPLEMENTS_INTERFACE(IGVDBPointQuantizeTicket)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_IMPLEMENTS_CLASS(QuantizeTicket)
    NVRHI_END_INTERFACE_TABLE()
    std::shared_ptr<Kernels> kernels;
    std::shared_ptr<Identity> identity;
    detail::ResolvedPointSet source;
    Parameters parameters{};
    BufferHandle constants, codes, decoded, codeStaging, positionStaging;
    BindingSetHandle encodeBindings, decodeBindings;
    std::vector<uint8_t> hostCodes;
    uint64_t codeBytes = 0, codeAllocation = 0, positionBytes = 0;
    bool recorded = false, completed = false;
    IDevice* getDevice() const noexcept override { return kernels->device.Get(); }
    void getBounds(Float4& minimum, Float4& range) const noexcept override {
        minimum = parameters.minimum;
        range = parameters.range;
    }
    FRESULT finishAfterCallerCompletion(IGVDBPointSet** output) noexcept override {
        if (!output)
            return FE_INVALID_ARGS;
        *output = nullptr;
        return guarded([&] {
            require(recorded && !completed);
            auto read = [&](IBuffer* staging, uint64_t bytes) {
                std::vector<uint8_t> result(size_t(bytes), uint8_t(0));
                if (bytes) {
                    const void* mapped = getDevice()->mapBuffer(staging, CpuAccessMode::Read);
                    require(mapped, FE_GENERIC_ERROR);
                    std::memcpy(result.data(), mapped, size_t(bytes));
                    getDevice()->unmapBuffer(staging);
                }
                return result;
            };
            auto newCodes = read(codeStaging, codeBytes), positions = read(positionStaging, positionBytes);
            PointSetHandle result;
            check(detail::clonePointSetPositions(source.retained, getDevice(), decoded, std::move(positions),
                                                 &result));
            hostCodes = std::move(newCodes);
            completed = true;
            *output = result.Detach();
        });
    }
    FRESULT getCodes(BorrowedBytes& output) const noexcept override {
        if (!completed)
            return FE_INVALID_ARGS;
        output = {hostCodes.empty() ? nullptr : hostCodes.data(), hostCodes.size()};
        return FS_OK;
    }
    FRESULT getCodeBuffer(BorrowedBufferBinding& output) const noexcept override {
        if (!recorded)
            return FE_INVALID_ARGS;
        if (!codeAllocation)
            return FE_NOT_FOUND;
        output = {codes.Get(), 0, codeAllocation};
        return FS_OK;
    }
};
class Quantize final : public ObjectImpl<IGVDBPointQuantize> {
    std::shared_ptr<Kernels> kernels;
    std::shared_ptr<Identity> identity = std::make_shared<Identity>();

 public:
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Quantize)
    NVRHI_IMPLEMENTS_INTERFACE(IGVDBPointQuantize)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    explicit Quantize(std::shared_ptr<Kernels> value)
        : kernels(std::move(value)) {}
    IDevice* getDevice() const noexcept override { return kernels->device.Get(); }
    FRESULT prepare(const PointQuantizeRequestDesc& desc, const PointQuantizePreparationBudget& budget,
                    IGVDBPointQuantizeTicket** output) noexcept override {
        if (!output)
            return FE_INVALID_ARGS;
        *output = nullptr;
        return guarded([&] {
            require(desc.structSize == sizeof(desc) && !desc.reserved && !desc.reserved2 && !budget.reserved);
            require(uint32_t(desc.bounds) <= uint32_t(PointQuantizationBounds::LegacyPadded));
            require(finite(desc.minimum) && finite(desc.range));
            detail::ResolvedPointSet source;
            check(detail::resolvePointSet(desc.points, getDevice(), source));
            const uint64_t count = source.info.count;
            require(count <= budget.maxPoints, FE_OUT_OF_MEMORY);
            require(count <= UINT32_MAX / 16, FE_UNSUPPORTED);
            Float4 minimum = desc.minimum, range = desc.range;
            if (desc.bounds == PointQuantizationBounds::LegacyPadded) {
                minimum = {};
                range = {1, 1, 1, 0};
                if (count) {
                    const float lo[] = {source.info.boundsMin.x, source.info.boundsMin.y,
                                        source.info.boundsMin.z};
                    const float hi[] = {source.info.boundsMax.x, source.info.boundsMax.y,
                                        source.info.boundsMax.z};
                    float m[3], r[3];
                    for (uint32_t axis = 0; axis < 3; ++axis) {
                        require(std::abs(lo[axis]) <= 4000000 && std::abs(hi[axis]) <= 4000000, FE_UNSUPPORTED);
                        m[axis] = lo[axis] < 0 ? lo[axis] - 1.f : std::max(0.f, lo[axis] - 1.f);
                        r[axis] = (hi[axis] + 2.f) - m[axis];
                    }
                    minimum = {m[0], m[1], m[2], 0};
                    range = {r[0], r[1], r[2], 0};
                }
            }
            const float minima[] = {minimum.x, minimum.y, minimum.z}, ranges[] = {range.x, range.y, range.z};
            for (uint32_t axis = 0; axis < 3; ++axis)
                require(ranges[axis] >= 16 * FLT_MIN &&
                            double(std::abs(minima[axis])) + ranges[axis] <= double(FLT_MAX) * .5,
                        FE_UNSUPPORTED);
            const uint64_t codeBytes = count * 6, codeAllocation = (codeBytes + 3) & ~uint64_t(3),
                           positionBytes = count * source.positionStream.byteStride;
            const uint64_t gpuBytes = count ? 256 + 2 * (codeAllocation + positionBytes) : 0;
            uint64_t hostBytes = codeBytes + positionBytes;
            for (auto attribute : {PointAttribute::Velocity, PointAttribute::Color})
                if (source.info.attributeMask & (1u << uint32_t(attribute))) {
                    BorrowedBytes bytes;
                    check(source.retained->getData(attribute, bytes));
                    require(bytes.byteCount <= UINT64_MAX - hostBytes, FE_UNSUPPORTED);
                    hostBytes += bytes.byteCount;
                }
            require(gpuBytes <= budget.maxAllocatedBytes && hostBytes <= budget.maxHostBytes &&
                        (!count || budget.maxBuffers >= 5),
                    FE_OUT_OF_MEMORY);
            auto ticket = MAKE_RC_OBJ_PTR(QuantizeTicket);
            ticket->kernels = kernels;
            ticket->identity = identity;
            ticket->source = std::move(source);
            ticket->codeBytes = codeBytes;
            ticket->codeAllocation = codeAllocation;
            ticket->positionBytes = positionBytes;
            ticket->parameters = {
                {uint32_t(count), uint32_t(ticket->source.positionStream.byteStride),
                 ticket->source.positionStream.components, 0},
                minimum,
                range
            };
            if (count) {
                auto allocate = [&](uint64_t bytes, bool raw, bool constant = false, bool staging = false) {
                    BufferDesc allocation;
                    allocation.byteSize = bytes;
                    allocation.canHaveRawViews = raw;
                    allocation.canHaveUAVs = raw && !staging;
                    allocation.isConstantBuffer = constant;
                    allocation.cpuAccess = staging ? CpuAccessMode::Read : CpuAccessMode::None;
                    allocation.initialState =
                        constant ? ResourceStates::ConstantBuffer : ResourceStates::CopyDest;
                    allocation.keepInitialState = true;
                    allocation.debugName = "GVDB point quantization";
                    BufferHandle result;
                    check(getDevice()->createBuffer(allocation, &result));
                    require(bool(result), FE_OUT_OF_MEMORY);
                    return result;
                };
                ticket->constants = allocate(256, false, true);
                ticket->codes = allocate(codeAllocation, true);
                ticket->decoded = allocate(positionBytes, true);
                ticket->codeStaging = allocate(codeAllocation, false, false, true);
                ticket->positionStaging = allocate(positionBytes, false, false, true);
                BindingSetDesc bindings;
                bindings.trackLiveness = true;
                bindings.addItem(BindingSetItem::ConstantBuffer(0, ticket->constants));
                bindings.addItem(BindingSetItem::RawBuffer_SRV(0, ticket->source.positionStream.buffer));
                bindings.addItem(BindingSetItem::RawBuffer_UAV(0, ticket->codes));
                bindings.addItem(BindingSetItem::RawBuffer_UAV(1, ticket->decoded));
                check(getDevice()->createBindingSet(bindings, kernels->encode.layout, &ticket->encodeBindings));
                check(getDevice()->createBindingSet(bindings, kernels->decode.layout, &ticket->decodeBindings));
            }
            *output = ticket.Detach();
        });
    }
    FRESULT record(RecordingContext context, IGVDBPointQuantizeTicket* input) noexcept override {
        return guarded([&] {
            require(input);
            AutoPtr<QuantizeTicket> ticket;
            require(NVRHI_SUCCEEDED(input->QueryInterface(uuid_of<QuantizeTicket>(),
                                                          reinterpret_cast<void**>(ticket.GetAddressOf()))) &&
                    ticket.Get());
            require(ticket->identity == identity && !ticket->recorded);
            require(context.device == getDevice() && context.commands &&
                    context.commands->getDevice() == getDevice());
            require(context.commands->getDesc().queueType != CommandQueue::Copy, FE_UNSUPPORTED);
            if (ticket->parameters.info.x) {
                std::array<uint8_t, 256> data{};
                std::memcpy(data.data(), &ticket->parameters, sizeof(Parameters));
                context.commands->writeBuffer(ticket->constants, data.data(), data.size());
                auto dispatch = [&](const Program& program, IBindingSet* bindings, uint32_t count) {
                    ComputeState state;
                    state.pipeline = program.pipeline;
                    state.bindings = {bindings};
                    context.commands->setComputeState(state);
                    const uint32_t groups = count / 256 + uint32_t(count % 256 != 0);
                    context.commands->dispatch(std::min(groups, 65535u), (groups + 65534) / 65535u);
                };
                dispatch(kernels->encode, ticket->encodeBindings, uint32_t(ticket->codeAllocation / 4));
                dispatch(kernels->decode, ticket->decodeBindings, ticket->parameters.info.x);
                context.commands->copyBuffer(ticket->codeStaging, 0, ticket->codes, 0, ticket->codeAllocation);
                context.commands->copyBuffer(ticket->positionStaging, 0, ticket->decoded, 0,
                                             ticket->positionBytes);
            }
            ticket->recorded = true;
        });
    }
};
}  // namespace
nvrhi::FRESULT gvdbCreateNvrhiPointQuantize(uint32_t version, const PointQuantizeCreateDesc* desc,
                                            IGVDBPointQuantize** output) noexcept {
    if (!output)
        return nvrhi::FE_INVALID_ARGS;
    *output = nullptr;
    return guarded([&] {
        require(version == c_GVDBNvrhiApiVersion, nvrhi::FE_UNSUPPORTED);
        require(desc && desc->structSize == sizeof(*desc) && !desc->reserved && desc->device);
        auto kernels = std::make_shared<Kernels>();
        kernels->device = desc->device;
        kernels->encode =
            loadProgram(desc->device, desc->encodeArtifact, desc->encodeArtifactBytes, "encodePointU16");
        kernels->decode =
            loadProgram(desc->device, desc->decodeArtifact, desc->decodeArtifactBytes, "decodePointU16");
        auto result = MAKE_RC_OBJ_PTR(Quantize, std::move(kernels));
        *output = result.Detach();
    });
}
}  // namespace gvdb::portable
