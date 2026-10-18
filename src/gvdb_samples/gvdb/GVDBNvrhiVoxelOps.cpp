// SPDX-License-Identifier: Apache-2.0
#include "GVDBNvrhiVoxelOps.h"
#include "GVDBNvrhiMutationInternal.h"
#include <nvrhi/shader-program.h>
#include <algorithm>
#include <cmath>
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
enum : uint32_t { Clear = 0, Fill = 1, Copy = 2, Apron = 3, Sine = 4 };
struct alignas(16) MutationParameters {
    VolumeInfo volume;
    slang::UInt4 work; // stride, voxels, words, last word valid bytes
    slang::UInt4 operation;
    slang::UInt4 fillBits;
    slang::UInt4 voidBits;
};
static_assert(sizeof(MutationParameters) == 144);
slang::UInt4 voidBits(const ChannelDesc& desc) {
    slang::UInt4 bits{};
    if (desc.format == AtlasFormat::R8_UINT) bits.x = uint32_t(desc.voidValue.x);
    else if (desc.format == AtlasFormat::RGBA8_UINT)
        bits.x = uint32_t(desc.voidValue.x) | uint32_t(desc.voidValue.y) << 8 |
            uint32_t(desc.voidValue.z) << 16 | uint32_t(desc.voidValue.w) << 24;
    else std::memcpy(&bits, &desc.voidValue, sizeof(bits));
    return bits;
}
slang::UInt4 fillBits(AtlasFormat format, Float4 value) {
    slang::UInt4 bits{};
    if (format == AtlasFormat::R8_UINT) {
        require(value.x >= 0 && value.x <= 255);
        bits.x = uint32_t(value.x);
    } else if (format == AtlasFormat::RGBA8_UINT) {
        require(value.x >= 0 && value.x <= 1 && value.y >= 0 && value.y <= 1 && value.z >= 0 && value.z <= 1);
        bits.x = uint32_t(value.x * 255.f) | uint32_t(value.y * 255.f) << 8 |
            uint32_t(value.z * 255.f) << 16 | 255u << 24;
    } else {
        value.w = 1;
        std::memcpy(&bits, &value, sizeof(bits));
    }
    return bits;
}
void validateArtifact(IShaderProgram* program, uint32_t entry, const BindingLayoutDesc& layout) {
    ShaderEntryPointDesc desc;
    check(program->getEntryPointDesc(entry, desc));
    require(desc.threadGroupSize[0] == 256 && desc.threadGroupSize[1] == 1 && desc.threadGroupSize[2] == 1,
            FE_UNSUPPORTED);
    const ResourceType types[] = {ResourceType::ConstantBuffer, ResourceType::RawBuffer_SRV,
        ResourceType::RawBuffer_SRV, ResourceType::StructuredBuffer_SRV, ResourceType::StructuredBuffer_SRV,
        ResourceType::RawBuffer_SRV, ResourceType::RawBuffer_UAV};
    const uint32_t slots[] = {0,0,1,2,3,4,0};
    require(layout.bindings.size() == 7, FE_UNSUPPORTED);
    for (uint32_t i = 0; i < 7; ++i)
        require(std::count_if(layout.bindings.begin(), layout.bindings.end(), [&](const auto& item) {
            return item.type == types[i] && item.slot == slots[i];
        }) == 1, FE_UNSUPPORTED);
    uint32_t globals = 0, members = 0, memberMask = 0, constantIndex = UINT32_MAX;
    for (uint32_t i = 0; i < program->getParameterCount(entry); ++i) {
        ShaderParameterDesc parameter;
        check(program->getParameterDesc(entry, i, parameter));
        require(parameter.registerSpace == 0 && parameter.count == 1, FE_UNSUPPORTED);
        if (parameter.scope == ShaderParameterScope::Global) {
            ++globals;
            require(parameter.parentParameter == UINT32_MAX, FE_UNSUPPORTED);
            if (parameter.resourceType == ResourceType::ConstantBuffer) {
                require(parameter.registerIndex == 0 && parameter.elementStride == sizeof(MutationParameters), FE_UNSUPPORTED);
                constantIndex = i;
            } else if (parameter.resourceType == ResourceType::StructuredBuffer_SRV) {
                require((parameter.registerIndex == 2 && parameter.elementStride == sizeof(LevelInfo)) ||
                    (parameter.registerIndex == 3 && parameter.elementStride == sizeof(slang::AtlasNode)), FE_UNSUPPORTED);
            } else require((parameter.resourceType == ResourceType::RawBuffer_SRV ||
                            parameter.resourceType == ResourceType::RawBuffer_UAV) && parameter.elementStride == 1,
                           FE_UNSUPPORTED);
        } else {
            require(parameter.scope == ShaderParameterScope::ConstantBuffer && parameter.resourceType == ResourceType::None &&
                parameter.size == 16 && parameter.elementStride == 16 && parameter.offset < sizeof(MutationParameters) &&
                parameter.offset % 16 == 0, FE_UNSUPPORTED);
            const uint32_t bit = 1u << (parameter.offset / 16);
            require(!(memberMask & bit), FE_UNSUPPORTED);
            memberMask |= bit;
            ++members;
        }
    }
    require(globals == 7 && members == 9 && memberMask == 511 && constantIndex != UINT32_MAX, FE_UNSUPPORTED);
    for (uint32_t i = 0; i < program->getParameterCount(entry); ++i) {
        ShaderParameterDesc parameter;
        check(program->getParameterDesc(entry, i, parameter));
        if (parameter.scope == ShaderParameterScope::ConstantBuffer)
            require(parameter.parentParameter == constantIndex, FE_UNSUPPORTED);
    }
}
} // namespace
struct NvrhiVoxelOps::Impl {
    DeviceHandle device;
    NvrhiVolume* volume = nullptr;
    ShaderProgramHandle program;
    BindingLayoutHandle layout;
    ComputePipelineHandle pipeline;
};
NvrhiVoxelOps::NvrhiVoxelOps(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}
NvrhiVoxelOps::~NvrhiVoxelOps() = default;
FRESULT NvrhiVoxelOps::create(const VoxelOpsDesc& desc, std::unique_ptr<NvrhiVoxelOps>& output) noexcept {
    return guarded([&] {
        require(desc.volume && desc.artifact && desc.artifactBytes);
        auto impl = std::make_unique<Impl>();
        impl->volume = desc.volume;
        impl->device = detail::VolumeMutationBuilder::device(*desc.volume);
        check(createShaderProgram(impl->device, desc.artifact, desc.artifactBytes, &impl->program));
        uint32_t entry;
        check(impl->program->findEntryPoint("mutateAtlas", ShaderType::Compute, &entry));
        BindingLayoutDesc layout;
        check(impl->program->getBindingLayoutDesc(entry, 0, layout));
        validateArtifact(impl->program, entry, layout);
        check(impl->device->createBindingLayout(layout, &impl->layout));
        ShaderHandle shader;
        check(impl->program->createShader(entry, &shader));
        check(impl->device->createComputePipeline(ComputePipelineDesc().setComputeShader(shader).addBindingLayout(impl->layout),
                                                &impl->pipeline));
        auto candidate = std::unique_ptr<NvrhiVoxelOps>(new NvrhiVoxelOps(std::move(impl)));
        output = std::move(candidate);
    });
}
FRESULT NvrhiVoxelOps::prepareFill(uint32_t channel, Float4 value, const PreparationBudget& budget,
                                  std::unique_ptr<PreparedGeneration>& output) noexcept {
    if (channel >= c_MaxChannels) return FE_INVALID_ARGS;
    return prepare(Fill, uint32_t(1) << channel, channel, value, budget, output);
}
FRESULT NvrhiVoxelOps::prepareClear(uint32_t mask, const PreparationBudget& budget,
                                   std::unique_ptr<PreparedGeneration>& output) noexcept {
    return prepare(Clear, mask, 0, {}, budget, output);
}
FRESULT NvrhiVoxelOps::prepareCopy(uint32_t source, uint32_t destination, const PreparationBudget& budget,
                                  std::unique_ptr<PreparedGeneration>& output) noexcept {
    if (source >= c_MaxChannels || destination >= c_MaxChannels) return FE_INVALID_ARGS;
    return prepare(Copy, uint32_t(1) << destination, source, {}, budget, output);
}
FRESULT NvrhiVoxelOps::prepareApron(uint32_t mask, const PreparationBudget& budget,
                                   std::unique_ptr<PreparedGeneration>& output) noexcept {
    return prepareApron(mask, nullptr, 0, budget, output);
}
FRESULT NvrhiVoxelOps::prepareApron(uint32_t mask, const Float4* values, uint32_t overrideMask,
    const PreparationBudget& budget, std::unique_ptr<PreparedGeneration>& output) noexcept {
    return prepare(Apron, mask, 0, {}, budget, output, values, overrideMask);
}
FRESULT NvrhiVoxelOps::prepare(uint32_t operation, uint32_t mask, uint32_t source, Float4 value,
    const PreparationBudget& budget, std::unique_ptr<PreparedGeneration>& output,
    const Float4* boundaryValues, uint32_t overrideMask) noexcept {
    return guarded([&] {
        auto& state = *m_impl;
        const auto snapshot = state.volume->snapshot();
        require(bool(snapshot), FE_NOT_ALIVE_OBJECT);
        const auto& current = snapshot->bindings();
        require(!(mask & ~current.channelMask), FE_NOT_FOUND);
        require(!(overrideMask & ~mask) && (!overrideMask || boundaryValues));
        for (uint32_t channel = 0; channel < c_MaxChannels; ++channel) {
            if (!(overrideMask & (uint32_t(1) << channel))) continue;
            ChannelDesc boundary = current.channels[channel].desc;
            boundary.voidValue = boundaryValues[channel];
            require(bool(validateChannelDesc(boundary)));
        }
        uint32_t writeMask = mask;
        slang::UInt4 fill{};
        if (operation == Fill) {
            require(std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w));
            const auto format = current.channels[source].desc.format;
            if (value.x == 0 && value.y == 0 && value.z == 0 && value.w == 0) operation = Clear;
            else {
                fill = fillBits(format, value);
                if (format == AtlasFormat::R32_FLOAT && value.z < 0) operation = Sine;
            }
        }
        if (operation == Copy) {
            require((current.channelMask & (uint32_t(1) << source)) != 0, FE_NOT_FOUND);
            for (uint32_t channel = 0; channel < c_MaxChannels; ++channel)
                if (mask & (uint32_t(1) << channel))
                    require(current.channels[source].desc.format == current.channels[channel].desc.format, FE_UNSUPPORTED);
            if (mask == (uint32_t(1) << source)) writeMask = 0;
        }
        if (operation == Apron && !current.volume.atlasResolutionAndApron.w) writeMask = 0;
        std::unique_ptr<detail::VolumeMutationBuilder> builder;
        check(detail::VolumeMutationBuilder::begin(*state.volume, writeMask, budget, builder));
        if (!builder->hasWork()) { check(builder->finish(output)); return; }
        const auto& destination = builder->destinationBindings();
        BufferHandle dummy;
        const uint32_t zero = 0;
        if (!current.nodes.buffer || !current.children.buffer)
            check(builder->upload(&zero, 4, 0, true, false, dummy));
        for (uint32_t channel = 0; channel < c_MaxChannels; ++channel) {
            if (!(writeMask & (uint32_t(1) << channel))) continue;
            const auto& binding = destination.channels[channel];
            MutationParameters parameters{};
            parameters.volume = destination.volume;
            parameters.work = {binding.storage.voxelStride, binding.storage.voxelCount,
                uint32_t(binding.storage.allocatedBytes / 4), uint32_t((binding.storage.logicalBytes - 1) % 4 + 1)};
            parameters.operation.x = operation;
            parameters.fillBits = fill;
            ChannelDesc boundary = binding.desc;
            if (overrideMask & (uint32_t(1) << channel)) boundary.voidValue = boundaryValues[channel];
            parameters.voidBits = voidBits(boundary);
            BufferHandle constants;
            check(builder->upload(&parameters, sizeof(parameters), 0, false, true, constants));
            BindingSetDesc desc;
            desc.trackLiveness = true;
            desc.addItem(BindingSetItem::ConstantBuffer(0, constants));
            desc.addItem(BindingSetItem::RawBuffer_SRV(0, current.nodes.buffer ? current.nodes.buffer : dummy));
            desc.addItem(BindingSetItem::RawBuffer_SRV(1, current.children.buffer ? current.children.buffer : dummy));
            desc.addItem(BindingSetItem::StructuredBuffer_SRV(2, current.levelMetadata.buffer));
            desc.addItem(BindingSetItem::StructuredBuffer_SRV(3, current.atlasMap.buffer));
            desc.addItem(BindingSetItem::RawBuffer_SRV(4, current.channels[operation == Copy ? source : channel].raw.buffer));
            desc.addItem(BindingSetItem::RawBuffer_UAV(0, binding.raw.buffer));
            BindingSetHandle set;
            check(state.device->createBindingSet(desc, state.layout, &set));
            const uint32_t groups = (parameters.work.z + 255u) / 256u;
            check(builder->appendDispatch(state.pipeline, set, std::min(groups, 65535u), (groups + 65534u) / 65535u));
        }
        check(builder->finish(output));
    });
}
} // namespace gvdb::portable
