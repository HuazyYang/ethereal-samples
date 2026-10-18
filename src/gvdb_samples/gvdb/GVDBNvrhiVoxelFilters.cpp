// SPDX-License-Identifier: Apache-2.0
#include "GVDBNvrhiVoxelFilters.h"
#include "GVDBNvrhiMutationInternal.h"
#include <nvrhi/shader-program.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
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
struct alignas(16) FilterParameters {
    VolumeInfo volume;
    slang::UInt4 work; // stride, voxels, words, last word valid bytes
    slang::UInt4 operation; // operation, apron pass, format, reserved
    Float4 values;
    slang::UInt4 boundaryBits;
};
static_assert(sizeof(FilterParameters) == 144);
bool finite(Float4 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
}
slang::UInt4 boundaryBits(AtlasFormat format, Float4 value) {
    slang::UInt4 bits{};
    const auto byte = [](float x) {
        require(x >= 0 && x <= 255 && std::trunc(x) == x);
        return uint32_t(x);
    };
    if (format == AtlasFormat::R8_UINT) bits.x = byte(value.x);
    else if (format == AtlasFormat::RGBA8_UINT)
        bits.x = byte(value.x) | byte(value.y) << 8 | byte(value.z) << 16 | byte(value.w) << 24;
    else std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}
void validate(const VoxelFilterDesc& desc, AtlasFormat format, const VolumeInfo& volume, uint32_t activeLeaves) {
    require(finite(desc.parameters) && finite(desc.boundaryValue));
    require(desc.iterations <= c_MaxVoxelFilterIterations, FE_UNSUPPORTED);
    bool stencil = false;
    switch (desc.operation) {
    case VoxelFilterOp::Smooth:
        require(format == AtlasFormat::R32_FLOAT || format == AtlasFormat::RGB32_FLOAT || format == AtlasFormat::RGBA32_FLOAT,
                FE_UNSUPPORTED);
        require(desc.parameters.x != -6.f);
        stencil = true;
        break;
    case VoxelFilterOp::Grow:
        require(format == AtlasFormat::R32_FLOAT, FE_UNSUPPORTED);
        require(std::abs(desc.parameters.x) <= std::numeric_limits<float>::max() / 10.f);
        break;
    case VoxelFilterOp::Noise:
    case VoxelFilterOp::Cut:
        require(format == AtlasFormat::R32_FLOAT, FE_UNSUPPORTED);
        break;
    case VoxelFilterOp::ExpandByte:
        require(format == AtlasFormat::R8_UINT, FE_UNSUPPORTED);
        require(desc.parameters.x >= 0 && desc.parameters.x <= 255 && desc.parameters.y >= 0 && desc.parameters.y <= 255);
        stencil = true;
        break;
    case VoxelFilterOp::ExpandColor:
        require(format == AtlasFormat::RGBA8_UINT, FE_UNSUPPORTED);
        // Bounds every truncated sum and its subsequent signed integer *255.
        require(std::abs(double(desc.parameters.x)) + 6.0 * std::abs(double(desc.parameters.y)) <= 32768.0);
        stencil = true;
        break;
    default: require(false);
    }
    if (stencil && activeLeaves && desc.iterations) require(volume.atlasResolutionAndApron.w >= 1, FE_UNSUPPORTED);
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
                require(parameter.registerIndex == 0 && parameter.elementStride == sizeof(FilterParameters), FE_UNSUPPORTED);
                constantIndex = i;
            } else if (parameter.resourceType == ResourceType::StructuredBuffer_SRV) {
                require((parameter.registerIndex == 2 && parameter.elementStride == sizeof(LevelInfo)) ||
                    (parameter.registerIndex == 3 && parameter.elementStride == sizeof(slang::AtlasNode)), FE_UNSUPPORTED);
            } else require((parameter.resourceType == ResourceType::RawBuffer_SRV ||
                            parameter.resourceType == ResourceType::RawBuffer_UAV) && parameter.elementStride == 1,
                           FE_UNSUPPORTED);
        } else {
            require(parameter.scope == ShaderParameterScope::ConstantBuffer && parameter.resourceType == ResourceType::None &&
                parameter.size == 16 && parameter.elementStride == 16 && parameter.offset < sizeof(FilterParameters) &&
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
struct NvrhiVoxelFilters::Impl {
    DeviceHandle device;
    NvrhiVolume* volume = nullptr;
    ShaderProgramHandle program;
    BindingLayoutHandle layout;
    ComputePipelineHandle pipeline;
};
NvrhiVoxelFilters::NvrhiVoxelFilters(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}
NvrhiVoxelFilters::~NvrhiVoxelFilters() = default;
FRESULT NvrhiVoxelFilters::create(const VoxelFiltersDesc& desc, std::unique_ptr<NvrhiVoxelFilters>& output) noexcept {
    return guarded([&] {
        require(desc.volume && desc.artifact && desc.artifactBytes);
        auto impl = std::make_unique<Impl>();
        impl->volume = desc.volume;
        impl->device = detail::VolumeMutationBuilder::device(*desc.volume);
        check(createShaderProgram(impl->device, desc.artifact, desc.artifactBytes, &impl->program));
        uint32_t entry;
        check(impl->program->findEntryPoint("filterAtlas", ShaderType::Compute, &entry));
        BindingLayoutDesc layout;
        check(impl->program->getBindingLayoutDesc(entry, 0, layout));
        validateArtifact(impl->program, entry, layout);
        check(impl->device->createBindingLayout(layout, &impl->layout));
        ShaderHandle shader;
        check(impl->program->createShader(entry, &shader));
        check(impl->device->createComputePipeline(ComputePipelineDesc().setComputeShader(shader).addBindingLayout(impl->layout),
                                                &impl->pipeline));
        output = std::unique_ptr<NvrhiVoxelFilters>(new NvrhiVoxelFilters(std::move(impl)));
    });
}
FRESULT NvrhiVoxelFilters::prepare(const VoxelFilterDesc& desc, const PreparationBudget& budget,
                                  std::unique_ptr<PreparedGeneration>& output) noexcept {
    return guarded([&] {
        require(desc.channel < c_MaxChannels);
        auto& state = *m_impl;
        const auto snapshot = state.volume->snapshot();
        require(bool(snapshot), FE_NOT_ALIVE_OBJECT);
        const auto& current = snapshot->bindings();
        const uint32_t mask = uint32_t(1) << desc.channel;
        require((current.channelMask & mask) != 0, FE_NOT_FOUND);
        const auto format = current.channels[desc.channel].desc.format;
        validate(desc, format, current.volume, current.activeLeafCount);
        const auto outside = boundaryBits(format, desc.boundaryValue);
        std::unique_ptr<detail::VolumeMutationBuilder> builder;
        check(detail::VolumeMutationBuilder::begin(*state.volume, desc.iterations ? mask : 0, budget, builder));
        if (!builder->hasWork()) { check(builder->finish(output)); return; }
        const auto& destination = builder->destinationBindings();
        const auto& binding = destination.channels[desc.channel];
        const bool apron = desc.updateApron && destination.volume.atlasResolutionAndApron.w != 0;
        const uint32_t passCount = desc.iterations * (apron ? 2u : 1u);
        BufferHandle scratch, dummy;
        if (passCount > 1) check(builder->allocate(binding.storage.allocatedBytes, 0, true, false, scratch));
        const uint32_t zero = 0;
        if (!current.nodes.buffer || !current.children.buffer) check(builder->upload(&zero, 4, 0, true, false, dummy));
        FilterParameters parameters{};
        parameters.volume = destination.volume;
        parameters.work = {binding.storage.voxelStride, binding.storage.voxelCount,
            uint32_t(binding.storage.allocatedBytes / 4), uint32_t((binding.storage.logicalBytes - 1) % 4 + 1)};
        parameters.operation = {uint32_t(desc.operation), 0, uint32_t(format), 0};
        parameters.values = desc.parameters;
        parameters.boundaryBits = outside;
        BufferHandle constants[2];
        check(builder->upload(&parameters, sizeof(parameters), 0, false, true, constants[0]));
        if (apron) {
            parameters.operation.y = 1;
            check(builder->upload(&parameters, sizeof(parameters), 0, false, true, constants[1]));
        }
        BufferHandle source = current.channels[desc.channel].raw.buffer;
        for (uint32_t pass = 0; pass < passCount; ++pass) {
            // Alternate allocations; the final pass always writes the published destination.
            const BufferHandle target = ((passCount - 1 - pass) & 1u) ? scratch : binding.raw.buffer;
            BindingSetDesc setDesc;
            setDesc.trackLiveness = true;
            setDesc.addItem(BindingSetItem::ConstantBuffer(0, constants[apron ? (pass & 1u) : 0]));
            setDesc.addItem(BindingSetItem::RawBuffer_SRV(0, current.nodes.buffer ? current.nodes.buffer : dummy));
            setDesc.addItem(BindingSetItem::RawBuffer_SRV(1, current.children.buffer ? current.children.buffer : dummy));
            setDesc.addItem(BindingSetItem::StructuredBuffer_SRV(2, current.levelMetadata.buffer));
            setDesc.addItem(BindingSetItem::StructuredBuffer_SRV(3, current.atlasMap.buffer));
            setDesc.addItem(BindingSetItem::RawBuffer_SRV(4, source));
            setDesc.addItem(BindingSetItem::RawBuffer_UAV(0, target));
            BindingSetHandle set;
            check(state.device->createBindingSet(setDesc, state.layout, &set));
            const uint32_t groups = (parameters.work.z + 255u) / 256u;
            check(builder->appendDispatch(state.pipeline, set, std::min(groups, 65535u), (groups + 65534u) / 65535u));
            source = target;
        }
        check(builder->finish(output));
    });
}
} // namespace gvdb::portable
