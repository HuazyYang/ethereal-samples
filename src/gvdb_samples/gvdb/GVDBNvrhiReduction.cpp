// SPDX-License-Identifier: Apache-2.0
#include "GVDBNvrhiReduction.h"
#include "GVDBNvrhiAdoptionInternal.h"
#include "GVDBNvrhiMutationInternal.h"
#include <nvrhi/shader-program.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <new>
#include <stdexcept>
namespace gvdb::portable {
namespace {
using namespace nvrhi;
struct Failure { FRESULT result; };
void require(bool condition, FRESULT result = FE_INVALID_ARGS) { if (!condition) throw Failure{result}; }
void check(FRESULT result) { require(NVRHI_SUCCEEDED(result), result); }
template<class F> FRESULT guarded(F&& function) noexcept {
    try { function(); return FS_OK; }
    catch (const Failure& failure) { return failure.result; }
    catch (const std::bad_alloc&) { return FE_OUT_OF_MEMORY; }
    catch (const std::length_error&) { return FE_OUT_OF_MEMORY; }
    catch (...) { return FE_GENERIC_ERROR; }
}
struct ReductionIdentity {};
struct alignas(16) Parameters { slang::UInt4 resolutionAndInterior, brickCountAndWidth, work; };
static_assert(sizeof(Parameters) == 48);
uint64_t product(uint64_t a, uint64_t b) {
    require(!a || b <= UINT64_MAX / a, FE_UNSUPPORTED);
    return a * b;
}
void validateArtifact(IShaderProgram* program, uint32_t entry, const BindingLayoutDesc& layout) {
    ShaderEntryPointDesc desc;
    check(program->getEntryPointDesc(entry, desc));
    require(desc.entryParamsSize == 0 && desc.threadGroupSize[0] == 256 && desc.threadGroupSize[1] == 1 && desc.threadGroupSize[2] == 1, FE_UNSUPPORTED);
    const ResourceType types[] = {ResourceType::ConstantBuffer, ResourceType::RawBuffer_SRV,
        ResourceType::StructuredBuffer_SRV, ResourceType::StructuredBuffer_UAV};
    const uint32_t slots[] = {0,0,1,0};
    require(layout.bindings.size() == 4, FE_UNSUPPORTED);
    for (uint32_t i = 0; i < 4; ++i) require(std::count_if(layout.bindings.begin(), layout.bindings.end(),
        [&](const auto& item) { return item.type == types[i] && item.slot == slots[i]; }) == 1, FE_UNSUPPORTED);
    uint32_t globals = 0, members = 0, mask = 0, constant = UINT32_MAX;
    for (uint32_t i = 0; i < program->getParameterCount(entry); ++i) {
        ShaderParameterDesc item;
        check(program->getParameterDesc(entry, i, item));
        require(item.registerSpace == 0 && item.count == 1, FE_UNSUPPORTED);
        if (item.scope == ShaderParameterScope::Global) {
            ++globals;
            require(item.parentParameter == UINT32_MAX, FE_UNSUPPORTED);
            if (item.resourceType == ResourceType::ConstantBuffer) {
                require(item.registerIndex == 0 && item.elementStride == sizeof(Parameters), FE_UNSUPPORTED);
                constant = i;
            } else if (item.resourceType == ResourceType::RawBuffer_SRV)
                require(item.registerIndex == 0 && item.elementStride == 1, FE_UNSUPPORTED);
            else if (item.resourceType == ResourceType::StructuredBuffer_SRV)
                require(item.registerIndex == 1 && item.elementStride == sizeof(slang::AtlasNode), FE_UNSUPPORTED);
            else require(item.resourceType == ResourceType::StructuredBuffer_UAV && item.registerIndex == 0 &&
                item.elementStride == sizeof(float), FE_UNSUPPORTED);
        } else {
            require(item.scope == ShaderParameterScope::ConstantBuffer && item.resourceType == ResourceType::None &&
                item.offset < sizeof(Parameters) && item.offset % 16 == 0 && item.size == 16 && item.elementStride == 16,
                FE_UNSUPPORTED);
            const uint32_t bit = 1u << (item.offset / 16);
            require(!(mask & bit), FE_UNSUPPORTED);
            mask |= bit;
            ++members;
        }
    }
    require(globals == 4 && members == 3 && mask == 7 && constant != UINT32_MAX, FE_UNSUPPORTED);
    for (uint32_t i = 0; i < program->getParameterCount(entry); ++i) {
        ShaderParameterDesc item;
        check(program->getParameterDesc(entry, i, item));
        if (item.scope == ShaderParameterScope::ConstantBuffer) require(item.parentParameter == constant, FE_UNSUPPORTED);
    }
}
} // namespace
struct PreparedReduction::Impl {
    DeviceHandle device;
    std::shared_ptr<ReductionIdentity> identity;
    std::shared_ptr<const VolumeGeneration> snapshot;
    ComputePipelineHandle pipeline;
    BindingSetHandle bindings;
    BufferHandle constants, columns, staging;
    Parameters parameters{};
    uint64_t voxelCount = 0;
    uint32_t groupsX = 0, groupsY = 0;
    bool recorded = false, completed = false;
};
struct NvrhiReduction::Impl {
    NvrhiVolume* owner = nullptr;
    DeviceHandle device;
    std::shared_ptr<ReductionIdentity> identity = std::make_shared<ReductionIdentity>();
    ShaderProgramHandle program;
    BindingLayoutHandle layout;
    ComputePipelineHandle pipeline;
};
PreparedReduction::PreparedReduction(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}
PreparedReduction::~PreparedReduction() = default;
NvrhiReduction::NvrhiReduction(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}
NvrhiReduction::~NvrhiReduction() = default;
FRESULT NvrhiReduction::create(const ReductionDesc& desc, std::unique_ptr<NvrhiReduction>& output) noexcept {
    return guarded([&] {
        require(desc.volume && desc.artifact && desc.artifactBytes);
        auto impl = std::make_unique<Impl>();
        impl->owner = desc.volume;
        impl->device = detail::VolumeMutationBuilder::device(*desc.volume);
        check(createShaderProgram(impl->device, desc.artifact, desc.artifactBytes, &impl->program));
        uint32_t entry;
        check(impl->program->findEntryPoint("reduceActiveColumns", ShaderType::Compute, &entry));
        BindingLayoutDesc layout;
        check(impl->program->getBindingLayoutDesc(entry, 0, layout));
        validateArtifact(impl->program, entry, layout);
        check(impl->device->createBindingLayout(layout, &impl->layout));
        ShaderHandle shader;
        check(impl->program->createShader(entry, &shader));
        check(impl->device->createComputePipeline(ComputePipelineDesc().setComputeShader(shader).addBindingLayout(impl->layout), &impl->pipeline));
        output = std::unique_ptr<NvrhiReduction>(new NvrhiReduction(std::move(impl)));
    });
}
FRESULT NvrhiReduction::prepare(std::shared_ptr<const VolumeGeneration> snapshot, uint32_t channel,
    const PreparationBudget& budget, std::unique_ptr<PreparedReduction>& output) noexcept {
    return guarded([&] {
        require(snapshot && channel < c_MaxChannels);
        check(detail::VolumeAdoptionBuilder::validateGeneration(*m_impl->owner, *snapshot));
        const auto& source = snapshot->bindings();
        require((source.channelMask & (uint32_t(1) << channel)) != 0, FE_NOT_FOUND);
        const auto& selected = source.channels[channel];
        require(selected.desc.format == AtlasFormat::R32_FLOAT, FE_UNSUPPORTED);
        auto ticket = std::make_unique<PreparedReduction::Impl>();
        ticket->device = m_impl->device;
        ticket->identity = m_impl->identity;
        ticket->snapshot = std::move(snapshot);
        if (source.activeLeafCount) {
            const auto grid = source.volume.atlasBrickCountAndWidth;
            const auto extent = source.volume.atlasResolutionAndApron;
            require(uint64_t(extent.w) * 2 < grid.w);
            const uint32_t interior = grid.w - extent.w * 2;
            const uint64_t packedX = product(grid.x, interior);
            const uint64_t columns = product(packedX, product(grid.y, interior));
            require(columns && columns <= c_AddressSpaceBytes / sizeof(float) && packedX <= UINT32_MAX, FE_UNSUPPORTED);
            ticket->voxelCount = product(source.activeLeafCount, product(product(interior,interior),interior));
            ticket->parameters = {{extent.x,extent.y,extent.z,interior},grid,{uint32_t(columns),uint32_t(packedX),extent.w,0}};
            uint64_t allocated = 0;
            uint32_t count = 0;
            auto allocate = [&](uint64_t bytes, bool constant, bool readback) {
                require(count < budget.maxBuffers && bytes <= budget.maxAllocatedBytes - allocated, FE_OUT_OF_MEMORY);
                BufferDesc desc;
                desc.byteSize = bytes;
                desc.structStride = constant || readback ? 0 : sizeof(float);
                desc.canHaveUAVs = !constant && !readback;
                desc.isConstantBuffer = constant;
                desc.cpuAccess = readback ? CpuAccessMode::Read : CpuAccessMode::None;
                desc.initialState = constant ? ResourceStates::ConstantBuffer : ResourceStates::CopyDest;
                desc.keepInitialState = true;
                desc.debugName = "GVDB active-interior reduction";
                BufferHandle buffer;
                check(ticket->device->createBuffer(desc, &buffer));
                require(bool(buffer), FE_OUT_OF_MEMORY);
                allocated += bytes;
                ++count;
                return buffer;
            };
            ticket->constants = allocate(256, true, false);
            ticket->columns = allocate(columns * sizeof(float), false, false);
            ticket->staging = allocate(columns * sizeof(float), false, true);
            BindingSetDesc bindings;
            bindings.trackLiveness = true;
            bindings.addItem(BindingSetItem::ConstantBuffer(0, ticket->constants));
            bindings.addItem(BindingSetItem::RawBuffer_SRV(0, selected.raw.buffer, {selected.raw.byteOffset,selected.raw.byteSize}));
            bindings.addItem(BindingSetItem::StructuredBuffer_SRV(1, source.atlasMap.buffer, Format::UNKNOWN,
                {source.atlasMap.byteOffset,source.atlasMap.byteSize}));
            bindings.addItem(BindingSetItem::StructuredBuffer_UAV(0, ticket->columns));
            check(ticket->device->createBindingSet(bindings, m_impl->layout, &ticket->bindings));
            ticket->pipeline = m_impl->pipeline;
            const uint32_t groups = (uint32_t(columns) + 255u) / 256u;
            ticket->groupsX = std::min(groups,65535u);
            ticket->groupsY = (groups + 65534u) / 65535u;
        }
        output = std::unique_ptr<PreparedReduction>(new PreparedReduction(std::move(ticket)));
    });
}
FRESULT NvrhiReduction::record(RecordingContext context, PreparedReduction& prepared) noexcept {
    return guarded([&] {
        auto& ticket = *prepared.m_impl;
        require(ticket.identity == m_impl->identity && !ticket.recorded && !ticket.completed);
        require(context.device == ticket.device.Get() && context.commands && context.commands->getDevice() == context.device);
        require(context.commands->getDesc().queueType != CommandQueue::Copy, FE_UNSUPPORTED);
        ticket.recorded = true;
        if (!ticket.voxelCount) return;
        std::array<uint8_t,256> constants{};
        std::memcpy(constants.data(), &ticket.parameters, sizeof(ticket.parameters));
        context.commands->writeBuffer(ticket.constants, constants.data(), constants.size());
        context.commands->setComputeState(ComputeState().setPipeline(ticket.pipeline).addBindingSet(ticket.bindings));
        context.commands->dispatch(ticket.groupsX, ticket.groupsY, 1);
        context.commands->copyBuffer(ticket.staging, 0, ticket.columns, 0, ticket.columns->getDesc().byteSize);
    });
}
FRESULT NvrhiReduction::finishAfterCallerCompletion(PreparedReduction& prepared, ReductionResult& output) noexcept {
    return guarded([&] {
        auto& ticket = *prepared.m_impl;
        require(ticket.recorded && !ticket.completed);
        ReductionResult result;
        result.voxelCount = ticket.voxelCount;
        if (ticket.voxelCount) {
            const auto* mapped = static_cast<const float*>(ticket.device->mapBuffer(ticket.staging,CpuAccessMode::Read));
            require(mapped, FE_GENERIC_ERROR);
            double sum = 0;
            for (uint32_t column = 0; column < ticket.parameters.work.x; ++column) sum += mapped[column];
            ticket.device->unmapBuffer(ticket.staging);
            result.sum = float(sum);
        }
        output = result;
        ticket.completed = true;
    });
}
} // namespace gvdb::portable
