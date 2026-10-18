// SPDX-License-Identifier: Apache-2.0
#include "OwnedVolumeSurface.h"
#include <nvrhi/shader-program.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
namespace gvdb::sample
{
namespace
{
using namespace nvrhi;
void require(bool value, const char *text)
{
    if (!value)
        throw std::invalid_argument(text);
}
void check(FRESULT status, const char *text)
{
    if (NVRHI_FAILED(status))
        throw std::runtime_error(std::string(text) + ": " + std::to_string(status));
}
struct alignas(16) Parameters
{
    slang::RaycastParameters ray;
    slang::Float4 options;
};
static_assert(sizeof(Parameters) == 240);
BufferRange range(const portable::BorrowedBufferBinding &view)
{
    return {view.byteOffset, view.byteSize};
}
void raw(const portable::BorrowedBufferBinding &view, uint64_t bytes)
{
    require(bool(portable::validateRawView({view.buffer, view.byteOffset, view.byteSize}, bytes, false)),
            "Owned surface raw range");
}
void validate(IShaderProgram *program, uint32_t entry, const BindingLayoutDesc &layout)
{
    const auto valid = [](bool value) { require(value, "Owned surface artifact ABI mismatch"); };
    ShaderEntryPointDesc desc;
    check(program->getEntryPointDesc(entry, desc), "surface entry metadata");
    valid(desc.threadGroupSize[0] == 8 && desc.threadGroupSize[1] == 8 && desc.threadGroupSize[2] == 1 &&
          !desc.entryParamsSize);
    const ResourceType types[] = {
        ResourceType::ConstantBuffer, ResourceType::RawBuffer_SRV,        ResourceType::RawBuffer_SRV,
        ResourceType::RawBuffer_SRV,  ResourceType::StructuredBuffer_SRV, ResourceType::StructuredBuffer_SRV,
        ResourceType::RawBuffer_SRV,  ResourceType::StructuredBuffer_UAV, ResourceType::StructuredBuffer_UAV};
    const uint32_t slots[] = {0, 0, 1, 2, 3, 4, 5, 0, 1}, strides[] = {sizeof(Parameters), 1, 1, 1, 64, 16, 1, 16, 4};
    valid(layout.bindings.size() == 9);
    for (uint32_t i = 0; i < 9; ++i)
        valid(std::count_if(layout.bindings.begin(), layout.bindings.end(),
                            [&](const auto &item) { return item.type == types[i] && item.slot == slots[i]; }) == 1);
    uint32_t globals = 0, resources = 0, members = 0, memberMask = 0, constant = UINT32_MAX;
    std::vector<ShaderParameterDesc> parameters;
    for (uint32_t i = 0; i < program->getParameterCount(entry); ++i)
    {
        ShaderParameterDesc item;
        check(program->getParameterDesc(entry, i, item), "surface parameter metadata");
        parameters.push_back(item);
        valid(item.registerSpace == 0 && item.count == 1);
        if (item.scope == ShaderParameterScope::Global)
        {
            ++globals;
            valid(item.parentParameter == UINT32_MAX);
            uint32_t found = 9;
            for (uint32_t j = 0; j < 9; ++j)
                if (item.resourceType == types[j] && item.registerIndex == slots[j])
                    found = j;
            valid(found < 9);
            valid(!(resources & (1u << found)) && item.elementStride == strides[found]);
            resources |= 1u << found;
            if (found == 0)
                constant = i;
        }
        else
        {
            valid(item.scope == ShaderParameterScope::ConstantBuffer && item.resourceType == ResourceType::None &&
                  item.size == 16 && item.elementStride == 16 && item.offset < sizeof(Parameters) &&
                  item.offset % 16 == 0);
            const uint32_t bit = 1u << (item.offset / 16);
            valid(!(memberMask & bit));
            memberMask |= bit;
            ++members;
        }
    }
    valid(globals == 9 && resources == 511 && members == 15 && memberMask == 32767 && constant != UINT32_MAX);
    for (const auto &item : parameters)
        if (item.scope == ShaderParameterScope::ConstantBuffer)
            valid(item.parentParameter == constant);
}
BufferHandle buffer(IDevice *device, uint64_t bytes, uint32_t stride, bool rawView = false, bool constant = false)
{
    BufferDesc desc;
    desc.byteSize = bytes;
    desc.structStride = stride;
    desc.canHaveRawViews = rawView;
    desc.canHaveUAVs = !constant;
    desc.isConstantBuffer = constant;
    desc.keepInitialState = true;
    desc.initialState = constant ? ResourceStates::ConstantBuffer : ResourceStates::CopyDest;
    BufferHandle result;
    check(device->createBuffer(desc, &result), "surface buffer");
    return result;
}
} // namespace
struct OwnedVolumeSurface::Impl
{
    nvrhi::DeviceHandle device;
    nvrhi::ShaderProgramHandle program;
    nvrhi::BindingLayoutHandle layout;
    nvrhi::ComputePipelineHandle pipeline;
    nvrhi::CommandListHandle commands;
    nvrhi::BufferHandle dummyRaw, dummyLevels;
    bool initialized = false;
};
OwnedVolumeSurface::OwnedVolumeSurface(nvrhi::IDevice *device, const std::filesystem::path &artifact)
{
    require(device, "Owned surface device");
    auto state = std::make_unique<Impl>();
    state->device = device;
    std::ifstream input(artifact, std::ios::binary);
    require(bool(input), "Missing owned surface artifact");
    std::vector<char> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    check(nvrhi::createShaderProgram(device, bytes.data(), bytes.size(), &state->program), "surface program");
    uint32_t entry;
    check(state->program->findEntryPoint("renderSurface", nvrhi::ShaderType::Compute, &entry), "surface entry");
    nvrhi::BindingLayoutDesc layout;
    check(state->program->getBindingLayoutDesc(entry, 0, layout), "surface bindings");
    validate(state->program, entry, layout);
    check(device->createBindingLayout(layout, &state->layout), "surface layout");
    nvrhi::ShaderHandle shader;
    check(state->program->createShader(entry, &shader), "surface shader");
    check(device->createComputePipeline(
              nvrhi::ComputePipelineDesc().setComputeShader(shader).addBindingLayout(state->layout), &state->pipeline),
          "surface pipeline");
    check(device->createCommandList({}, &state->commands), "surface recording list");
    state->dummyRaw = buffer(device, 4, 0, true);
    state->dummyLevels = buffer(device, sizeof(slang::LevelInfo), sizeof(slang::LevelInfo));
    m_impl = std::move(state);
}
OwnedVolumeSurface::~OwnedVolumeSurface() = default;
slang::RaycastOutput OwnedVolumeSurface::render(portable::SnapshotHandle snapshot, uint32_t scalarChannel,
                                                uint32_t colorChannel, slang::RaycastParameters parameters,
                                                const std::vector<slang::Float4> &transfer, float scalarBackground)
{
    using namespace nvrhi;
    auto &state = *m_impl;
    require(snapshot && snapshot->getDevice() == state.device.Get(), "Owned surface snapshot device");
    require(std::isfinite(scalarBackground), "Owned surface background");
    require(parameters.light.w >= 0 && parameters.light.w <= 1, "Owned surface ambient light");
    require(parameters.image.x <= 65535u * 8u && parameters.image.y <= 65535u * 8u,
            "Owned surface dispatch dimensions");
    require(transfer.size() <= portable::c_AddressSpaceBytes / sizeof(slang::Float4), "Owned surface transfer range");
    slang::validateRaycastParameters(parameters, transfer);
    const uint64_t pixels = uint64_t(parameters.image.x) * parameters.image.y;
    require(pixels <= portable::c_AddressSpaceBytes / sizeof(slang::Float4), "Owned surface output range");
    const auto info = snapshot->getDesc();
    require(info.levelCount && info.levelCount <= portable::c_MaxLevels && info.volume.hierarchy.z == info.levelCount,
            "Owned surface metadata");
    const auto channel = [&](uint32_t index, portable::AtlasFormat format) {
        portable::ChannelSnapshotDesc result;
        check(snapshot->getChannel(index, result), "surface channel");
        require(result.desc.format == format, "Owned surface channel format");
        portable::ChannelStorage storage;
        const auto res = info.volume.atlasResolutionAndApron;
        require(bool(portable::describeChannel(format, {res.x, res.y, res.z}, storage)) &&
                    storage.voxelCount == result.storage.voxelCount &&
                    storage.logicalBytes == result.storage.logicalBytes &&
                    storage.allocatedBytes == result.storage.allocatedBytes && result.storage.voxelStride == 4,
                "Owned surface atlas extent");
        raw(result.raw, storage.allocatedBytes);
        return result;
    };
    const auto scalar = channel(scalarChannel, portable::AtlasFormat::R32_FLOAT);
    portable::ChannelSnapshotDesc color;
    if (colorChannel != UINT32_MAX)
    {
        require(colorChannel != scalarChannel, "Owned surface distinct channels");
        color = channel(colorChannel, portable::AtlasFormat::RGBA8_UINT);
    }
    portable::BorrowedBufferBinding nodes, children, levels;
    if (info.activeLeafCount)
    {
        require(scalar.storage.voxelCount && info.volume.hierarchy.x < info.levelCount,
                "Owned surface active hierarchy");
        check(snapshot->getTopologyBuffer(portable::TopologyBuffer::Nodes, nodes), "surface nodes");
        check(snapshot->getTopologyBuffer(portable::TopologyBuffer::Children, children), "surface children");
        check(snapshot->getTopologyBuffer(portable::TopologyBuffer::Levels, levels), "surface levels");
        raw(nodes, sizeof(portable::Node));
        raw(children, 0);
        require(levels.buffer && levels.buffer->getDesc().structStride == sizeof(slang::LevelInfo) &&
                    portable::containsBytes(levels.buffer->getDesc().byteSize, levels.byteOffset, levels.byteSize) &&
                    levels.byteOffset % sizeof(slang::LevelInfo) == 0 &&
                    levels.byteSize >= uint64_t(info.levelCount) * sizeof(slang::LevelInfo),
                "Owned surface level range");
    }
    const auto rawItem = [&](uint32_t slot, const portable::BorrowedBufferBinding &value) {
        return value.buffer ? BindingSetItem::RawBuffer_SRV(slot, value.buffer, range(value))
                            : BindingSetItem::RawBuffer_SRV(slot, state.dummyRaw);
    };
    Parameters p{parameters,
                 {scalarBackground, colorChannel != UINT32_MAX ? 1.f : 0.f, info.activeLeafCount ? 1.f : 0.f, 0}};
    p.ray.volume = info.volume;
    p.ray.image.z = uint32_t(transfer.size());
    auto tf = buffer(state.device, transfer.size() * sizeof(slang::Float4), sizeof(slang::Float4));
    auto constants = buffer(state.device, 256, 0, false, true);
    slang::RaycastOutput result;
    result.width = parameters.image.x;
    result.height = parameters.image.y;
    result.color = buffer(state.device, pixels * sizeof(slang::Float4), sizeof(slang::Float4));
    result.depth = buffer(state.device, pixels * sizeof(float), sizeof(float));
    BindingSetDesc bindings;
    bindings.trackLiveness = true;
    bindings.bindings = {BindingSetItem::ConstantBuffer(0, constants),
                         rawItem(0, scalar.raw),
                         rawItem(1, nodes),
                         rawItem(2, children),
                         levels.buffer
                             ? BindingSetItem::StructuredBuffer_SRV(3, levels.buffer, Format::UNKNOWN, range(levels))
                             : BindingSetItem::StructuredBuffer_SRV(3, state.dummyLevels),
                         BindingSetItem::StructuredBuffer_SRV(4, tf),
                         rawItem(5, color.raw),
                         BindingSetItem::StructuredBuffer_UAV(0, result.color),
                         BindingSetItem::StructuredBuffer_UAV(1, result.depth)};
    BindingSetHandle set;
    check(state.device->createBindingSet(bindings, state.layout, &set), "surface binding set");
    std::array<uint8_t, 256> padded{};
    std::memcpy(padded.data(), &p, sizeof(p));
    struct Close
    {
        ICommandList *list;
        bool open = false;
        ~Close()
        {
            if (open)
                list->close();
        }
    } recording{state.commands};
    state.commands->open();
    recording.open = true;
    if (!state.initialized)
    {
        const std::array<uint8_t, 64> zero{};
        state.commands->writeBuffer(state.dummyRaw, zero.data(), 4);
        state.commands->writeBuffer(state.dummyLevels, zero.data(), 64);
    }
    state.commands->writeBuffer(constants, padded.data(), padded.size());
    state.commands->writeBuffer(tf, transfer.data(), transfer.size() * sizeof(slang::Float4));
    state.commands->setComputeState(ComputeState().setPipeline(state.pipeline).addBindingSet(set));
    state.commands->dispatch((result.width + 7) / 8, (result.height + 7) / 8, 1);
    state.commands->close();
    recording.open = false;
    const auto submitted = state.device->executeCommandList(state.commands);
    require(submitted || state.device->getGraphicsAPI() == GraphicsAPI::D3D11, "Owned surface submission failed");
    state.initialized = true;
    return result;
}
} // namespace gvdb::sample
