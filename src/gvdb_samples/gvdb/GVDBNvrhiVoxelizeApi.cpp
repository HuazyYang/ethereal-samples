// SPDX-License-Identifier: Apache-2.0
#include "GVDBNvrhiVoxelizeApi.h"
#include <nvrhi/shader-program.h>
#include <nvrhi/core/foundation.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <limits>
#include <stdexcept>
#include <vector>
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
uint64_t product(uint64_t a, uint64_t b) {
    require(!a || b <= UINT64_MAX / a, FE_UNSUPPORTED);
    return a * b;
}
bool finite(Float4 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w);
}
struct Identity {};
using Parameters = slang::VoxelizeParameters;
constexpr const char* entries[] = {"clearTriangleBins", "countTriangleBins", "scatterTriangleBins",
                                   "solidVoxelize"};
struct Resource {
    ResourceType type;
    uint32_t slot, stride;
};
constexpr Resource resources[] = {
    {ResourceType::ConstantBuffer,       0, 128},
    {ResourceType::StructuredBuffer_SRV, 0, 16 },
    {ResourceType::StructuredBuffer_SRV, 1, 4  },
    {ResourceType::StructuredBuffer_SRV, 2, 4  },
    {ResourceType::StructuredBuffer_SRV, 3, 4  },
    {ResourceType::StructuredBuffer_SRV, 4, 48 },
    {ResourceType::StructuredBuffer_UAV, 0, 4  },
    {ResourceType::StructuredBuffer_UAV, 1, 48 },
    {ResourceType::RawBuffer_UAV,        2, 1  }
};
void validateArtifact(IShaderProgram* program, uint32_t entry, const BindingLayoutDesc& layout) {
    ShaderEntryPointDesc desc;
    check(program->getEntryPointDesc(entry, desc));
    require(!desc.entryParamsSize && desc.threadGroupSize[0] == 256 && desc.threadGroupSize[1] == 1 &&
                desc.threadGroupSize[2] == 1,
            FE_UNSUPPORTED);
    require(layout.bindings.size() == 9, FE_UNSUPPORTED);
    for (auto r : resources)
        require(std::count_if(layout.bindings.begin(), layout.bindings.end(),
                              [&](auto item) { return item.type == r.type && item.slot == r.slot; }) == 1,
                FE_UNSUPPORTED);
    uint32_t globalMask = 0, memberMask = 0, constant = UINT32_MAX;
    for (uint32_t i = 0; i < program->getParameterCount(entry); ++i) {
        ShaderParameterDesc p;
        check(program->getParameterDesc(entry, i, p));
        require(!p.registerSpace, FE_UNSUPPORTED);
        if (p.scope == ShaderParameterScope::Global) {
            require(p.parentParameter == UINT32_MAX && p.count == 1, FE_UNSUPPORTED);
            uint32_t r = 0;
            for (; r < 9; ++r)
                if (p.resourceType == resources[r].type && p.registerIndex == resources[r].slot)
                    break;
            require(r < 9 && !(globalMask & (1u << r)), FE_UNSUPPORTED);
            globalMask |= 1u << r;
            require(p.elementStride == resources[r].stride, FE_UNSUPPORTED);
            if (!r)
                constant = i;
        } else {
            require(p.scope == ShaderParameterScope::ConstantBuffer && p.resourceType == ResourceType::None &&
                        p.registerIndex == 0 && p.offset <= 80 && p.offset % 16 == 0 && p.elementStride == 16 &&
                        p.size == (p.offset == 80 ? 48u : 16u) && p.count == (p.offset == 80 ? 3u : 1u),
                    FE_UNSUPPORTED);
            const auto bit = 1u << (p.offset / 16);
            require(!(memberMask & bit), FE_UNSUPPORTED);
            memberMask |= bit;
        }
    }
    require(globalMask == 511 && memberMask == 63 && constant != UINT32_MAX, FE_UNSUPPORTED);
    for (uint32_t i = 0; i < program->getParameterCount(entry); ++i) {
        ShaderParameterDesc p;
        check(program->getParameterDesc(entry, i, p));
        if (p.scope == ShaderParameterScope::ConstantBuffer)
            require(p.parentParameter == constant, FE_UNSUPPORTED);
    }
}
struct Kernels {
    DeviceHandle device;  // Last resource released.
    std::array<ShaderProgramHandle, 4> programs;
    std::array<BindingLayoutHandle, 4> layouts;
    std::array<ComputePipelineHandle, 4> pipelines;
};
BufferHandle allocate(IDevice* device, uint64_t bytes, uint32_t stride = 0, bool uav = false, bool raw = false,
                      bool staging = false, bool constant = false) {
    BufferDesc d;
    d.byteSize = bytes;
    d.structStride = stride;
    d.canHaveUAVs = uav;
    d.canHaveRawViews = raw;
    d.isConstantBuffer = constant;
    d.cpuAccess = staging ? CpuAccessMode::Read : CpuAccessMode::None;
    d.keepInitialState = true;
    d.initialState = constant ? ResourceStates::ConstantBuffer : ResourceStates::CopyDest;
    d.debugName = "GVDB public mesh voxelization";
    BufferHandle result;
    check(device->createBuffer(d, &result));
    require(bool(result), FE_OUT_OF_MEMORY);
    return result;
}
void dispatch(ICommandList* commands, ComputePipelineHandle pipeline, BindingSetHandle bindings,
              uint32_t count) {
    if (!count)
        return;
    const uint32_t groups = count / 256 + uint32_t(count % 256 != 0);
    commands->setComputeState(ComputeState().setPipeline(pipeline).addBindingSet(bindings));
    commands->dispatch(std::min(groups, 65535u), (groups + 65534) / 65535, 1);
}
NVRHI_CLASS_CLSID(VoxelizeTicket, "d5e00399-cb02-447d-b012-68ccc58f0dda")
class VoxelizeTicket final : public ObjectImpl<IGVDBVoxelizeTicket> {
 public:
    std::shared_ptr<Kernels> kernels;
    std::shared_ptr<Identity> identity;
    Parameters parameters{};
    VoxelizePreparationBudget budget;
    uint64_t initialBytes = 0, logicalBytes = 0, paddedBytes = 0;
    enum Stage {
        Prepared,
        CountsRecorded,
        CountsCompleted,
        VoxelsRecorded,
        Completed
    } stage = Prepared;
    std::vector<Float4> vertices;
    std::vector<uint32_t> indices, offsets;
    std::vector<uint8_t> result;
    BufferHandle constants, vertexBuffer, indexBuffer, counts, countStaging;
    // Unused module globals have distinct valid storage, avoiding SRV/UAV aliasing.
    std::array<BufferHandle, 5> dummy;
    BufferHandle offsetBuffer, triangles, output, outputStaging;
    std::array<BindingSetHandle, 4> bindings;
    NVRHI_DECLARE_UUID_TRAITS(VoxelizeTicket)
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(VoxelizeTicket)
    NVRHI_IMPLEMENTS_INTERFACE(IGVDBVoxelizeTicket)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_IMPLEMENTS_CLASS(VoxelizeTicket)
    NVRHI_END_INTERFACE_TABLE()
    IDevice* getDevice() const noexcept override { return kernels->device; }
    UInt3 getResolution() const noexcept override {
        return {parameters.resolution.x, parameters.resolution.y, parameters.resolution.z};
    }
    MeshVoxelFormat getFormat() const noexcept override { return MeshVoxelFormat(parameters.counts.w); }
    BindingSetHandle bind(uint32_t phase, IBuffer* newOffsets = nullptr, IBuffer* newTriangles = nullptr,
                          IBuffer* newOutput = nullptr) {
        BindingSetDesc d;
        d.trackLiveness = true;
        d.addItem(BindingSetItem::ConstantBuffer(0, constants));
        d.addItem(BindingSetItem::StructuredBuffer_SRV(0, vertexBuffer));
        d.addItem(BindingSetItem::StructuredBuffer_SRV(1, indexBuffer));
        d.addItem(BindingSetItem::StructuredBuffer_SRV(2, phase == 3 ? counts.Get() : dummy[0].Get()));
        d.addItem(BindingSetItem::StructuredBuffer_SRV(3, phase >= 2 ? newOffsets : dummy[0].Get()));
        d.addItem(BindingSetItem::StructuredBuffer_SRV(4, phase == 3 ? newTriangles : dummy[1].Get()));
        d.addItem(BindingSetItem::StructuredBuffer_UAV(0, phase == 3 ? dummy[2].Get() : counts.Get()));
        d.addItem(BindingSetItem::StructuredBuffer_UAV(1, phase == 2 ? newTriangles : dummy[3].Get()));
        d.addItem(BindingSetItem::RawBuffer_UAV(2, phase == 3 ? newOutput : dummy[4].Get()));
        BindingSetHandle result;
        check(getDevice()->createBindingSet(d, kernels->layouts[phase], &result));
        return result;
    }
    FRESULT finishCountsAfterCallerCompletion() noexcept override {
        return guarded([&] {
            require(stage == CountsRecorded);
            std::vector<uint32_t> prefix(parameters.counts.z);
            uint64_t total = 0;
            {
                const auto* mapped =
                    static_cast<const uint32_t*>(getDevice()->mapBuffer(countStaging, CpuAccessMode::Read));
                require(mapped, FE_GENERIC_ERROR);
                struct Unmap {
                    IDevice* d;
                    IBuffer* b;
                    ~Unmap() { d->unmapBuffer(b); }
                } unmap{getDevice(), countStaging};
                for (size_t i = 0; i < prefix.size(); ++i) {
                    require(mapped[i] <= parameters.counts.y, FE_GENERIC_ERROR);
                    prefix[i] = uint32_t(total);
                    total += mapped[i];
                    require(total <= UINT32_MAX / 48, FE_UNSUPPORTED);
                }
            }
            const uint64_t triangleBytes = std::max(uint64_t(1), total) * 48;
            const uint64_t allocation = initialBytes + prefix.size() * 4 + triangleBytes + paddedBytes * 2;
            require(budget.maxBuffers >= 14 && allocation <= budget.maxAllocatedBytes, FE_OUT_OF_MEMORY);
            auto nextOffsets = allocate(getDevice(), prefix.size() * 4, 4);
            auto nextTriangles = allocate(getDevice(), triangleBytes, 48, true);
            auto nextOutput = allocate(getDevice(), paddedBytes, 0, true, true);
            auto nextStaging = allocate(getDevice(), paddedBytes, 0, false, false, true);
            auto scatter = bind(2, nextOffsets, nextTriangles, nextOutput),
                 voxel = bind(3, nextOffsets, nextTriangles, nextOutput);
            offsets.swap(prefix);
            offsetBuffer = std::move(nextOffsets);
            triangles = std::move(nextTriangles);
            output = std::move(nextOutput);
            outputStaging = std::move(nextStaging);
            bindings[2] = std::move(scatter);
            bindings[3] = std::move(voxel);
            stage = CountsCompleted;
        });
    }
    FRESULT getOutputBuffer(BorrowedBufferBinding& value) const noexcept override {
        if (stage < VoxelsRecorded)
            return FE_INVALID_ARGS;
        value = {output, 0, logicalBytes};
        return FS_OK;
    }
    FRESULT finishAfterCallerCompletion(BorrowedBytes& value) noexcept override {
        return guarded([&] {
            require(stage == VoxelsRecorded);
            std::vector<uint8_t> bytes(size_t(logicalBytes), uint8_t(0));
            const void* mapped = getDevice()->mapBuffer(outputStaging, CpuAccessMode::Read);
            require(mapped, FE_GENERIC_ERROR);
            std::memcpy(bytes.data(), mapped, bytes.size());
            getDevice()->unmapBuffer(outputStaging);
            result.swap(bytes);
            stage = Completed;
            value = {result.data(), logicalBytes};
        });
    }
    FRESULT getValues(BorrowedBytes& value) const noexcept override {
        if (stage != Completed)
            return FE_INVALID_ARGS;
        value = {result.data(), logicalBytes};
        return FS_OK;
    }
};
void validate(const VoxelizeRequestDesc& d, const VoxelizePreparationBudget& b, Parameters& p,
              uint64_t& initial, uint64_t& logical) {
    require(d.structSize == sizeof(d) && !d.reserved && !d.reserved2[0] && !d.reserved2[1] && !b.reserved);
    require(d.resolution.x && d.resolution.y && d.resolution.z && d.indexCount % 3 == 0);
    require(d.vertexCount <= UINT32_MAX / 16 && d.indexCount <= UINT32_MAX / 4, FE_UNSUPPORTED);
    require((!d.vertexCount || d.vertices) && (!d.indexCount || d.indices));
    const uint64_t voxels = product(product(d.resolution.x, d.resolution.y), d.resolution.z);
    require(voxels <= UINT32_MAX / 4, FE_UNSUPPORTED);
    require(d.format == MeshVoxelFormat::R8_UINT || d.format == MeshVoxelFormat::R32_FLOAT ||
            d.format == MeshVoxelFormat::R32_SINT);
    require(finite(d.boundsMinimum) && finite(d.boundsMaximum) && finite(d.values) && d.values.w == 0);
    for (auto row : d.transformRows)
        require(finite(row));
    const float fills[] = {d.values.x, d.values.y, d.values.z};
    for (float v : fills) {
        if (d.format == MeshVoxelFormat::R8_UINT)
            require(v >= 0 && v <= 255);
        if (d.format == MeshVoxelFormat::R32_SINT)
            require(double(v) >= INT32_MIN && double(v) <= INT32_MAX);
    }
    const uint32_t bins = std::min(d.resolution.y, 65535u * 256u);
    const auto triangles = d.indexCount / 3;
    require(product(product(triangles, bins), 2) <= b.maxTriangleBinVisits &&
                product(voxels, triangles) <= b.maxVoxelTriangleVisits,
            FE_OUT_OF_MEMORY);
    require(d.vertexCount * 16 + d.indexCount * 4 <= b.maxInputBytes, FE_OUT_OF_MEMORY);
    logical = voxels * (d.format == MeshVoxelFormat::R8_UINT ? 1 : 4);
    initial = 256 + std::max(uint64_t(16), d.vertexCount * 16) + std::max(uint64_t(4), d.indexCount * 4) +
              uint64_t(bins) * 8 + 108;
    // At least one dummy triangle and the complete dense readback are always needed.
    require(b.maxBuffers >= 14 &&
                initial + uint64_t(bins) * 4 + 48 + ((logical + 3) & ~uint64_t(3)) * 2 <= b.maxAllocatedBytes,
            FE_OUT_OF_MEMORY);
    const float low[] = {d.boundsMinimum.x, d.boundsMinimum.y, d.boundsMinimum.z},
                high[] = {d.boundsMaximum.x, d.boundsMaximum.y, d.boundsMaximum.z};
    const uint32_t resolution[] = {d.resolution.x, d.resolution.y, d.resolution.z};
    double widths[3], centers[3];
    for (unsigned axis = 0; axis < 3; ++axis) {
        const float extent = high[axis] - low[axis], width = extent / float(resolution[axis]);
        require(low[axis] < high[axis] && std::isfinite(extent) && std::isfinite(width) &&
                width >= 16 * std::numeric_limits<float>::min());
        widths[axis] = width;
        centers[axis] = std::max(std::abs(double(low[axis])), std::abs(double(high[axis]))) + double(width);
        require(centers[axis] <= 1e30 && 2 * centers[axis] / width <= 65536);
    }
    // Conservative absolute-term bounds cover transform cancellation, center
    // construction, normalized triangle coordinates, and degree-six parity products.
    for (uint64_t i = 0; i < d.vertexCount; ++i) {
        Float4 v;
        std::memcpy(&v, d.vertices + i, sizeof(v));
        require(finite(v));
        for (unsigned axis = 0; axis < 3; ++axis) {
            const auto row = d.transformRows[axis];
            const double magnitude = std::abs(double(v.x) * row.x) + std::abs(double(v.y) * row.y) +
                                     std::abs(double(v.z) * row.z) + std::abs(double(row.w));
            require(magnitude <= 1e30 && 2 * (magnitude + centers[axis]) / widths[axis] <= 65536);
        }
    }
    for (uint64_t i = 0; i < d.indexCount; ++i) {
        uint32_t index;
        std::memcpy(&index, d.indices + i, 4);
        require(index < d.vertexCount);
    }
    p.boundsMinimum = d.boundsMinimum;
    p.boundsMaximum = d.boundsMaximum;
    p.values = d.values;
    p.resolution = {d.resolution.x, d.resolution.y, d.resolution.z, uint32_t(voxels)};
    p.counts = {uint32_t(d.vertexCount), uint32_t(triangles), bins, uint32_t(d.format)};
    p.values.w = (d.boundsMaximum.y - d.boundsMinimum.y) / float(bins);
    require(std::isfinite(p.values.w) && p.values.w > 0);
    std::copy(std::begin(d.transformRows), std::end(d.transformRows), p.transformRows);
}
class Voxelize final : public ObjectImpl<IGVDBVoxelize> {
 public:
    std::shared_ptr<Kernels> kernels;
    std::shared_ptr<Identity> identity = std::make_shared<Identity>();
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Voxelize)
    NVRHI_IMPLEMENTS_INTERFACE(IGVDBVoxelize)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    IDevice* getDevice() const noexcept override { return kernels->device; }
    FRESULT prepare(const VoxelizeRequestDesc& d, const VoxelizePreparationBudget& b,
                    IGVDBVoxelizeTicket** output) noexcept override {
        if (!output)
            return FE_INVALID_ARGS;
        *output = nullptr;
        return guarded([&] {
            auto t = MAKE_RC_OBJ_PTR(VoxelizeTicket);
            t->kernels = kernels;
            t->identity = identity;
            t->budget = b;
            validate(d, b, t->parameters, t->initialBytes, t->logicalBytes);
            t->paddedBytes = (t->logicalBytes + 3) & ~uint64_t(3);
            t->vertices.resize(size_t(std::max(uint64_t(1), d.vertexCount)));
            t->indices.resize(size_t(std::max(uint64_t(1), d.indexCount)));
            if (d.vertexCount)
                std::memcpy(t->vertices.data(), d.vertices, size_t(d.vertexCount) * 16);
            if (d.indexCount)
                std::memcpy(t->indices.data(), d.indices, size_t(d.indexCount) * 4);
            t->constants = allocate(getDevice(), 256, 0, false, false, false, true);
            t->vertexBuffer = allocate(getDevice(), t->vertices.size() * 16, 16);
            t->indexBuffer = allocate(getDevice(), t->indices.size() * 4, 4);
            t->counts = allocate(getDevice(), uint64_t(t->parameters.counts.z) * 4, 4, true);
            t->countStaging =
                allocate(getDevice(), uint64_t(t->parameters.counts.z) * 4, 0, false, false, true);
            t->dummy[0] = allocate(getDevice(), 4, 4);
            t->dummy[1] = allocate(getDevice(), 48, 48);
            t->dummy[2] = allocate(getDevice(), 4, 4, true);
            t->dummy[3] = allocate(getDevice(), 48, 48, true);
            t->dummy[4] = allocate(getDevice(), 4, 0, true, true);
            t->bindings[0] = t->bind(0);
            t->bindings[1] = t->bind(1);
            *output = t.Detach();
        });
    }
    AutoPtr<VoxelizeTicket> authenticate(RecordingContext context, IGVDBVoxelizeTicket* ticket,
                                         VoxelizeTicket::Stage stage) {
        require(ticket);
        AutoPtr<VoxelizeTicket> t;
        require(NVRHI_SUCCEEDED(ticket->QueryInterface(uuid_of<VoxelizeTicket>(),
                                                       reinterpret_cast<void**>(t.GetAddressOf()))) &&
                t.Get());
        require(t->identity == identity && t->stage == stage);
        require(context.device == getDevice() && context.commands &&
                context.commands->getDevice() == context.device);
        require(context.commands->getDesc().queueType != CommandQueue::Copy, FE_UNSUPPORTED);
        return t;
    }
    FRESULT recordCounts(RecordingContext c, IGVDBVoxelizeTicket* ticket) noexcept override {
        return guarded([&] {
            auto t = authenticate(c, ticket, VoxelizeTicket::Prepared);
            t->stage = VoxelizeTicket::CountsRecorded;
            std::array<uint8_t, 256> padded{};
            std::memcpy(padded.data(), &t->parameters, sizeof(Parameters));
            c.commands->writeBuffer(t->constants, padded.data(), padded.size());
            c.commands->writeBuffer(t->vertexBuffer, t->vertices.data(), t->vertices.size() * 16);
            c.commands->writeBuffer(t->indexBuffer, t->indices.data(), t->indices.size() * 4);
            for (auto buffer : t->dummy)
                c.commands->writeBuffer(buffer, padded.data(), size_t(buffer->getDesc().byteSize));
            dispatch(c.commands, kernels->pipelines[0], t->bindings[0], t->parameters.counts.z);
            dispatch(c.commands, kernels->pipelines[1], t->bindings[1], t->parameters.counts.y);
            c.commands->copyBuffer(t->countStaging, 0, t->counts, 0, uint64_t(t->parameters.counts.z) * 4);
        });
    }
    FRESULT recordVoxelization(RecordingContext c, IGVDBVoxelizeTicket* ticket) noexcept override {
        return guarded([&] {
            auto t = authenticate(c, ticket, VoxelizeTicket::CountsCompleted);
            t->stage = VoxelizeTicket::VoxelsRecorded;
            c.commands->writeBuffer(t->offsetBuffer, t->offsets.data(), t->offsets.size() * 4);
            dispatch(c.commands, kernels->pipelines[0], t->bindings[0], t->parameters.counts.z);
            dispatch(c.commands, kernels->pipelines[2], t->bindings[2], t->parameters.counts.y);
            const uint32_t count = t->parameters.resolution.w;
            dispatch(c.commands, kernels->pipelines[3], t->bindings[3],
                     t->parameters.counts.w == 0 ? (count + 3) / 4 : count);
            c.commands->copyBuffer(t->outputStaging, 0, t->output, 0, t->paddedBytes);
        });
    }
};
}  // namespace
FRESULT gvdbCreateNvrhiVoxelize(uint32_t version, const VoxelizeCreateDesc* desc,
                                IGVDBVoxelize** output) noexcept {
    if (!output)
        return FE_INVALID_ARGS;
    *output = nullptr;
    return guarded([&] {
        require(version == c_GVDBNvrhiApiVersion, FE_UNSUPPORTED);
        require(desc && desc->structSize == sizeof(*desc) && !desc->reserved && desc->device);
        for (auto a : desc->artifacts)
            require(a.data && a.byteCount && a.byteCount <= SIZE_MAX);
        auto kernels = std::make_shared<Kernels>();
        kernels->device = desc->device;
        for (uint32_t i = 0; i < 4; ++i) {
            const auto a = desc->artifacts[i];
            check(createShaderProgram(desc->device, a.data, size_t(a.byteCount), &kernels->programs[i]));
            uint32_t entry;
            check(kernels->programs[i]->findEntryPoint(entries[i], ShaderType::Compute, &entry));
            BindingLayoutDesc layout;
            check(kernels->programs[i]->getBindingLayoutDesc(entry, 0, layout));
            validateArtifact(kernels->programs[i], entry, layout);
            check(desc->device->createBindingLayout(layout, &kernels->layouts[i]));
            ShaderHandle shader;
            check(kernels->programs[i]->createShader(entry, &shader));
            check(desc->device->createComputePipeline(
                ComputePipelineDesc().setComputeShader(shader).addBindingLayout(kernels->layouts[i]),
                &kernels->pipelines[i]));
        }
        auto op = MAKE_RC_OBJ_PTR(Voxelize);
        op->kernels = std::move(kernels);
        *output = op.Detach();
    });
}
}  // namespace gvdb::portable
