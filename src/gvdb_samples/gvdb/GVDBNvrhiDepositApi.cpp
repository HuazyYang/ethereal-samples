// SPDX-License-Identifier: Apache-2.0
#include "GVDBNvrhiDepositApi.h"
#include "GVDBNvrhiApiInternal.h"
#include "GVDBNvrhiPointApiInternal.h"
#include "GVDBNvrhiMutationInternal.h"
#include "GVDBNvrhiAdoptionInternal.h"
#include <nvrhi/core/foundation.h>
#include <nvrhi/shader-program.h>
#include <algorithm>
#include <array>
#include <cmath>
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
float roundedAdd(float a, float b) {
    volatile float result = a + b;
    return result;
}
float roundedMultiply(float a, float b) {
    volatile float result = a * b;
    return result;
}
struct alignas(16) Parameters {
    slang::UInt4 resolutionAndApron, brickCountAndWidth, work, options;
    Float4 translationAndRadius, offset;
};
static_assert(sizeof(Parameters) == 96);
void validateArtifact(IShaderProgram* program, uint32_t entry, const BindingLayoutDesc& layout) {
    ShaderEntryPointDesc desc;
    check(program->getEntryPointDesc(entry, desc));
    require(desc.entryParamsSize == 0 && desc.threadGroupSize[0] == 256 && desc.threadGroupSize[1] == 1 &&
                desc.threadGroupSize[2] == 1,
            FE_UNSUPPORTED);
    const ResourceType types[] = {ResourceType::ConstantBuffer,       ResourceType::StructuredBuffer_SRV,
                                  ResourceType::RawBuffer_SRV,        ResourceType::RawBuffer_SRV,
                                  ResourceType::RawBuffer_SRV,        ResourceType::StructuredBuffer_SRV,
                                  ResourceType::StructuredBuffer_SRV, ResourceType::RawBuffer_SRV,
                                  ResourceType::RawBuffer_SRV,        ResourceType::RawBuffer_SRV,
                                  ResourceType::RawBuffer_UAV,        ResourceType::RawBuffer_UAV,
                                  ResourceType::RawBuffer_UAV};
    const uint32_t slots[] = {0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 0, 1, 2};
    const uint32_t strides[] = {sizeof(Parameters), 16, 1, 1, 1, 8, 8, 1, 1, 1, 1, 1, 1};
    require(layout.bindings.size() == 13, FE_UNSUPPORTED);
    for (uint32_t i = 0; i < 13; ++i)
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
            uint32_t found = 13;
            for (uint32_t j = 0; j < 13; ++j)
                if (item.resourceType == types[j] && item.registerIndex == slots[j])
                    found = j;
            require(found < 13, FE_UNSUPPORTED);
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
    require(globals == 13 && resources == 8191 && members == 6 && mask == 63 && constant != UINT32_MAX,
            FE_UNSUPPORTED);
    for (uint32_t i = 0; i < program->getParameterCount(entry); ++i) {
        ShaderParameterDesc item;
        check(program->getParameterDesc(entry, i, item));
        if (item.scope == ShaderParameterScope::ConstantBuffer)
            require(item.parentParameter == constant, FE_UNSUPPORTED);
    }
}
struct Bins {
    std::vector<slang::UInt2> cells, pairs;
};
Bins buildBins(NvrhiVolume& owner, const SnapshotBindings& source, const VolumeGeneration& snapshot,
               const detail::ResolvedPointSet& points, const DepositRequestDesc& request,
               const DepositPreparationBudget& budget, BorrowedPointStream& directionStream) {
    const uint64_t voxels = source.channels[request.channel].storage.voxelCount;
    const uint64_t leafCount = source.levels[0].nodes.z;
    const uint64_t topologyBytes = source.nodes.byteSize + source.children.byteSize +
                                   source.levelMetadata.byteSize + source.atlasMap.byteSize;
    const uint64_t fixedHostBytes = topologyBytes + leafCount * 4 + voxels * 16 + 256;
    require(fixedHostBytes <= budget.maxHostBytes, FE_OUT_OF_MEMORY);
    require(voxels <= c_AddressSpaceBytes / 8 && voxels <= SIZE_MAX / 8, FE_UNSUPPORTED);
    slang::PackedVolume topology;
    check(detail::VolumeAdoptionBuilder::exportTopology(snapshot, topology));
    std::vector<uint32_t> slots(size_t(leafCount), UINT32_MAX);
    for (uint32_t slot = 0; slot < topology.atlasMap.size(); ++slot) {
        const auto leaf = topology.atlasMap[slot].leaf;
        if (leaf >= 0) {
            require(uint64_t(leaf) < leafCount);
            slots[size_t(leaf)] = slot;
        }
    }
    BorrowedBytes directions;
    if (request.kind == DepositKind::SupportXZ && points.info.count) {
        check(points.retained->getData(PointAttribute::Velocity, directions));
        check(points.retained->getStream(PointAttribute::Velocity, directionStream));
    }
    const auto r = source.volume.atlasResolutionAndApron, grid = source.volume.atlasBrickCountAndWidth;
    const int32_t width = int32_t(grid.w - 2 * r.w);
    Bins result;
    result.cells.resize(size_t(voxels), {0, 0});
    uint64_t total = 0;
    auto enumerate = [&](auto&& emit) {
        for (uint32_t i = 0; i < points.info.count; ++i) {
            float base[3];
            std::memcpy(base,
                        static_cast<const uint8_t*>(points.positions.data) +
                            size_t(i) * points.positionStream.byteStride,
                        12);
            const float translation[] = {request.translation.x, request.translation.y, request.translation.z};
            for (uint32_t axis = 0; axis < 3; ++axis) {
                base[axis] = roundedAdd(base[axis], translation[axis]);
                require(std::isfinite(base[axis]));
            }
            float shifted[] = {base[0], base[1], base[2]};
            if (request.kind == DepositKind::SupportXZ) {
                float direction[3];
                std::memcpy(
                    direction,
                    static_cast<const uint8_t*>(directions.data) + size_t(i) * directionStream.byteStride, 12);
                for (uint32_t axis = 0; axis < 3; ++axis) {
                    require(std::isfinite(direction[axis]));
                    shifted[axis] =
                        roundedAdd(base[axis], roundedMultiply(direction[axis], request.directionOffset));
                    require(std::isfinite(shifted[axis]));
                }
            }
            bool addressable = true;
            int32_t world[3];
            for (uint32_t axis = 0; axis < 3; ++axis) {
                const double floor = std::floor(double(base[axis]));
                if (floor < INT32_MIN || floor > INT32_MAX) {
                    addressable = false;
                    break;
                }
                world[axis] = int32_t(floor);
            }
            if (!addressable)
                continue;
            const auto leaf = owner.host().findLeaf({world[0], world[1], world[2]});
            if (leaf.isNull())
                continue;
            require(leaf.index() < slots.size());
            const auto slot = slots[size_t(leaf.index())];
            if (slot == UINT32_MAX)
                continue;
            const auto& mapping = topology.atlasMap[slot];
            const int32_t origin[] = {mapping.x, mapping.y, mapping.z};
            int32_t local[3];
            bool inside = true;
            for (uint32_t axis = 0; axis < 3; ++axis) {
                const float v = roundedAdd(shifted[axis], -float(origin[axis]));
                require(std::isfinite(v));
                if (v <= -1 || v >= width) {
                    inside = false;
                    break;
                }
                local[axis] = int32_t(v);
            }
            if (!inside)
                continue;
            const int32_t brick[] = {int32_t((slot % grid.x) * grid.w + r.w),
                                     int32_t(((slot / grid.x) % grid.y) * grid.w + r.w),
                                     int32_t((slot / (grid.x * grid.y)) * grid.w + r.w)};
            auto visit = [&](int32_t dx, int32_t dy, int32_t dz, bool center) {
                const uint32_t x = uint32_t(brick[0] + local[0] + dx), y = uint32_t(brick[1] + local[1] + dy),
                               z = uint32_t(brick[2] + local[2] + dz);
                require(x < r.x && y < r.y && z < r.z);
                const uint64_t index = (uint64_t(z) * r.y + y) * r.x + x;
                require(index < voxels);
                emit(uint32_t(index), slang::UInt2{i, center ? 1u : 0u});
            };
            if (request.kind == DepositKind::SupportXZ)
                for (int32_t dx = -1; dx <= 1; ++dx)
                    for (int32_t dz = -1; dz <= 1; ++dz)
                        visit(dx, 0, dz, false);
            else {
                visit(0, 0, 0, true);
                if (request.kind == DepositKind::SixNeighbors) {
                    visit(-1, 0, 0, false);
                    visit(1, 0, 0, false);
                    visit(0, -1, 0, false);
                    visit(0, 1, 0, false);
                    visit(0, 0, -1, false);
                    visit(0, 0, 1, false);
                }
            }
        }
    };
    enumerate([&](uint32_t index, slang::UInt2) {
        require(total < budget.maxContributions, FE_OUT_OF_MEMORY);
        require(total < c_AddressSpaceBytes / 8, FE_UNSUPPORTED);
        ++total;
        ++result.cells[index].y;
    });
    const uint64_t pairCount = std::max(uint64_t(1), total);
    require(pairCount * 16 <= budget.maxHostBytes - fixedHostBytes, FE_OUT_OF_MEMORY);
    uint32_t offset = 0;
    for (auto& cell : result.cells) {
        const auto count = cell.y;
        cell = {offset, 0};
        offset += count;
    }
    result.pairs.resize(size_t(pairCount), {0, 0});
    enumerate([&](uint32_t index, slang::UInt2 pair) {
        auto& cell = result.cells[index];
        result.pairs[cell.x + cell.y++] = pair;
    });
    return result;
}
class Deposit final : public ObjectImpl<IGVDBDeposit> {
    VolumeHandle volume;
    NvrhiVolume* core;
    ShaderProgramHandle program;
    BindingLayoutHandle layout;
    ComputePipelineHandle pipeline;

 public:
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Deposit)
    NVRHI_IMPLEMENTS_INTERFACE(IGVDBDeposit)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    Deposit(VolumeHandle v, NvrhiVolume* c, ShaderProgramHandle p, BindingLayoutHandle l,
            ComputePipelineHandle pipeline)
        : volume(std::move(v)),
          core(c),
          program(std::move(p)),
          layout(std::move(l)),
          pipeline(std::move(pipeline)) {}
    IGVDBVolume* getVolume() const noexcept override { return volume.Get(); }
    FRESULT prepare(const DepositRequestDesc& request, const DepositPreparationBudget& budget,
                    IGVDBGenerationTicket** output) noexcept override {
        if (!output)
            return FE_INVALID_ARGS;
        *output = nullptr;
        return guarded([&] {
            require(request.structSize == sizeof(request) && !request.reserved && !request.reserved2 &&
                    !budget.reserved);
            require(uint32_t(request.kind) <= 2 && uint32_t(request.color) <= 2 &&
                    request.channel < c_MaxChannels);
            require(finite(request.translation) && std::isfinite(request.directionOffset) &&
                    std::isfinite(request.radius));
            require(request.radius >= 1e-4f && request.radius <= 1e8f, FE_UNSUPPORTED);
            const bool support = request.kind == DepositKind::SupportXZ,
                       colors = request.color != DepositColor::None;
            require(support ? (!colors && request.materialChannel < c_MaxChannels)
                            : (request.materialChannel == UINT32_MAX && request.directionOffset == 0));
            require(colors ? (request.colorChannel < c_MaxChannels && request.colorChannel != request.channel)
                           : request.colorChannel == UINT32_MAX);
            require(!support || request.materialChannel != request.channel);
            detail::ResolvedPointSet points;
            check(detail::resolvePointSet(request.points, volume->getDevice(), points));
            require(points.info.count <= budget.maxPoints, FE_OUT_OF_MEMORY);
            require(points.info.count <= UINT32_MAX / 16, FE_UNSUPPORTED);
            if (request.color == DepositColor::AverageRGB)
                require(points.info.count <= UINT32_MAX / 255, FE_UNSUPPORTED);
            require(!colors || !points.info.count || (points.info.attributeMask & 4), FE_NOT_FOUND);
            require(!support || !points.info.count || (points.info.attributeMask & 2), FE_NOT_FOUND);
            const auto snapshot = core->snapshot();
            require(bool(snapshot), FE_NOT_ALIVE_OBJECT);
            const auto& source = snapshot->bindings();
            uint32_t mask = 1u << request.channel;
            if (colors)
                mask |= 1u << request.colorChannel;
            if (support)
                mask |= 1u << request.materialChannel;
            require((source.channelMask & mask) == mask, FE_NOT_FOUND);
            require(source.channels[request.channel].desc.format == AtlasFormat::R32_FLOAT, FE_UNSUPPORTED);
            require(!colors || source.channels[request.colorChannel].desc.format == AtlasFormat::RGBA8_UINT,
                    FE_UNSUPPORTED);
            require(!support || source.channels[request.materialChannel].desc.format == AtlasFormat::R8_UINT,
                    FE_UNSUPPORTED);
            require(request.kind == DepositKind::Central || source.volume.atlasResolutionAndApron.w >= 1,
                    FE_UNSUPPORTED);
            std::unique_ptr<detail::VolumeMutationBuilder> builder;
            check(detail::VolumeMutationBuilder::begin(*core, mask,
                                                       {budget.maxAllocatedBytes, budget.maxBuffers}, builder));
            std::unique_ptr<PreparedGeneration> ticket;
            if (!builder->hasWork()) {
                check(builder->finish(ticket));
                check(detail::wrapGeneration(volume->getDevice(), std::move(ticket), output));
                return;
            }
            BorrowedPointStream directions;
            auto bins = buildBins(*core, source, *snapshot, points, request, budget, directions);
            const auto& destination = builder->destinationBindings();
            const auto& old = source.channels[request.channel].raw;
            const auto& target = destination.channels[request.channel].raw;
            BufferHandle cells, pairs, constants, dummyColor, dummyMaterial;
            check(builder->upload(bins.cells.data(), bins.cells.size() * 8, 8, false, false, cells));
            check(builder->upload(bins.pairs.data(), bins.pairs.size() * 8, 8, false, false, pairs));
            Parameters parameters{
                source.volume.atlasResolutionAndApron,
                source.volume.atlasBrickCountAndWidth,
                {source.channels[request.channel].storage.voxelCount,
                                                 uint32_t(points.positionStream.byteStride),                                 uint32_t(directions.byteStride),
                                                 uint32_t(request.kind)                                                                                                     },
                {uint32_t(request.color),                             colors ? 1u : 0u,      support ? 1u : 0u,               0             },
                {request.translation.x,                               request.translation.y, request.translation.z,           request.radius},
                {request.directionOffset,                             0,                     0,                               0             }
            };
            check(builder->upload(&parameters, sizeof(parameters), 0, false, true, constants));
            if (!colors)
                check(builder->allocate(4, 0, true, false, dummyColor));
            if (!support)
                check(builder->allocate(4, 0, true, false, dummyMaterial));
            BindingSetDesc bindings;
            bindings.trackLiveness = true;
            bindings.addItem(BindingSetItem::ConstantBuffer(0, constants));
            bindings.addItem(
                BindingSetItem::StructuredBuffer_SRV(0, source.atlasMap.buffer, Format::UNKNOWN,
                                                     {source.atlasMap.byteOffset, source.atlasMap.byteSize}));
            bindings.addItem(BindingSetItem::RawBuffer_SRV(
                1, points.info.count ? points.positionStream.buffer : old.buffer.Get()));
            bindings.addItem(BindingSetItem::RawBuffer_SRV(
                2, support && points.info.count ? directions.buffer : old.buffer.Get()));
            bindings.addItem(BindingSetItem::RawBuffer_SRV(
                3, colors && points.info.count ? points.colorStream.buffer : old.buffer.Get()));
            bindings.addItem(BindingSetItem::StructuredBuffer_SRV(4, cells));
            bindings.addItem(BindingSetItem::StructuredBuffer_SRV(5, pairs));
            bindings.addItem(BindingSetItem::RawBuffer_SRV(6, old.buffer, {old.byteOffset, old.byteSize}));
            const auto& oldColor = colors ? source.channels[request.colorChannel].raw : old;
            const auto& oldMaterial = support ? source.channels[request.materialChannel].raw : old;
            bindings.addItem(
                BindingSetItem::RawBuffer_SRV(7, oldColor.buffer, {oldColor.byteOffset, oldColor.byteSize}));
            bindings.addItem(BindingSetItem::RawBuffer_SRV(8, oldMaterial.buffer,
                                                           {oldMaterial.byteOffset, oldMaterial.byteSize}));
            bindings.addItem(
                BindingSetItem::RawBuffer_UAV(0, target.buffer, {target.byteOffset, target.byteSize}));
            if (colors) {
                const auto& v = destination.channels[request.colorChannel].raw;
                bindings.addItem(BindingSetItem::RawBuffer_UAV(1, v.buffer, {v.byteOffset, v.byteSize}));
            } else
                bindings.addItem(BindingSetItem::RawBuffer_UAV(1, dummyColor));
            if (support) {
                const auto& v = destination.channels[request.materialChannel].raw;
                bindings.addItem(BindingSetItem::RawBuffer_UAV(2, v.buffer, {v.byteOffset, v.byteSize}));
            } else
                bindings.addItem(BindingSetItem::RawBuffer_UAV(2, dummyMaterial));
            BindingSetHandle binding;
            check(volume->getDevice()->createBindingSet(bindings, layout, &binding));
            const uint32_t count = parameters.work.x / 4 + uint32_t(parameters.work.x % 4 != 0),
                           groups = count / 256 + uint32_t(count % 256 != 0);
            check(builder->appendDispatch(pipeline, binding, std::min(groups, 65535u),
                                          (groups + 65534u) / 65535u));
            check(builder->finish(ticket));
            check(detail::wrapGeneration(volume->getDevice(), std::move(ticket), output));
        });
    }
};
}  // namespace
nvrhi::FRESULT gvdbCreateNvrhiDeposit(uint32_t version, const DepositCreateDesc* desc,
                                      IGVDBDeposit** output) noexcept {
    if (!output)
        return nvrhi::FE_INVALID_ARGS;
    *output = nullptr;
    return guarded([&] {
        require(version == c_GVDBNvrhiApiVersion, FE_UNSUPPORTED);
        require(desc && desc->structSize == sizeof(*desc) && !desc->reserved && desc->volume &&
                desc->artifact && desc->artifactBytes && desc->artifactBytes <= SIZE_MAX);
        VolumeHandle volume = desc->volume;
        NvrhiVolume* core = nullptr;
        check(detail::resolveVolume(volume, &core));
        ShaderProgramHandle program;
        check(createShaderProgram(volume->getDevice(), desc->artifact, size_t(desc->artifactBytes), &program));
        uint32_t entry;
        check(program->findEntryPoint("depositOwnedPoints", ShaderType::Compute, &entry));
        BindingLayoutDesc layoutDesc;
        check(program->getBindingLayoutDesc(entry, 0, layoutDesc));
        validateArtifact(program, entry, layoutDesc);
        BindingLayoutHandle layout;
        check(volume->getDevice()->createBindingLayout(layoutDesc, &layout));
        ShaderHandle shader;
        check(program->createShader(entry, &shader));
        ComputePipelineHandle pipeline;
        check(volume->getDevice()->createComputePipeline(
            ComputePipelineDesc().setComputeShader(shader).addBindingLayout(layout), &pipeline));
        auto result = MAKE_RC_OBJ_PTR(Deposit, std::move(volume), core, std::move(program), std::move(layout),
                                      std::move(pipeline));
        *output = result.Detach();
    });
}
}  // namespace gvdb::portable
